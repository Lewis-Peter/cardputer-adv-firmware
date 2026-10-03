#!/usr/bin/env python3
"""Cardputer 声学分析 —— spec_view.py 采回来的频谱流，在这头做语义。

`spec_view.py` 只负责把设备的频谱流原样存成 ndjson；这个脚本是它的下半场：
读那份 ndjson，算出这段时间里的声学环境是什么样子。

## 先说这份数据做不到什么

分工是「设备采集、电脑分析」，但电脑再有算力也变不出设备没采到的信息。
固件端 `SPEC_MIN_MS = 200` 限速 5 帧/秒，每帧是一次 256 点 @16kHz 的 FFT ——
也就是每 224ms（实测中位值，比名义 200ms 慢，采样本身 16ms 加上串口打印的开销）
只看了 **16ms** 的声音。**占空比约 7%，93% 的时间是失明的。**

这条约束不是细节，它决定了这份数据能回答哪些问题：

| 想问的 | 行不行 | 为什么 |
|---|---|---|
| 这个环境的声学性格（安静？嗡嗡？有哨音？） | ✅ | 采样虽稀疏但**无偏**，采够时间统计上就准 |
| 有没有持续性声源（风扇/空调/变压器啸叫/马路） | ✅ | 持续存在的东西，7% 采样照样每帧都撞得到 |
| 绝对电平的长时趋势（现在比一小时前吵多少） | ✅ | 电平取自 AGC **之前**，帧间可比 |
| 有没有窄带音，多少赫兹 | ✅ | 单帧的 62.5Hz 分辨率就够认出哨音/嗡鸣 |
| 一次咳嗽 / 一次拍手发生了没有 | ❌ | 30ms 的事件单次捕获概率只有 20% |
| 说了什么话 | ❌ | 只有幅度谱，相位早丢了，波形重建不回来 |
| 谁在说话 | ❌ | 同上，而且 16ms 窗撑不起说话人特征 |
| 旋翼/引擎的周期性调制（10~25Hz） | ❌ | 见下面「关于调制分析」 |

脚本每次运行都会照实测帧率把这几条边界重算一遍打印出来，别照抄上面的数。

## 关于调制分析

`spec_view.py` 的 docstring 里曾建议「对 rms 序列做 FFT 就能看见直升机桨叶通过频率
（10~25Hz）」，还提醒要先按 ms 字段重采样。**那条建议是错的**，而且错得有迷惑性——
重采样只修不均匀，修不了根本问题：rms 序列本身就是 4.5Hz 采样的，**Nyquist 只有
2.2Hz**。10/15/20/25Hz 会分别混叠到 1.07/1.61/2.14/1.79Hz，四个全折进 0~2.2Hz、
互相不可分，还和呼吸、语速这些真实的慢变搅在一起。看见「一个 1.6Hz 的峰」完全
说明不了天上有直升机。

雪上加霜的是每帧那 16ms 窗对 10~25Hz（周期 40~100ms）是**部分平均**，本来就会
把调制深度压掉一截。

真要做调制分析，得从固件那头改：`SPEC_MIN_MS` 至少压到 20ms（50fps，Nyquist 25Hz）。
带宽上是够的——320 字节/帧 × 50 = 16KB/s，超了 115200 的 11.5KB/s，所以还得同时
砍 bin 数或改二进制编码。这个脚本不假装能绕过它。

## 用法

    python3 tools/spec_analyze.py /tmp/audio.ndjson
    python3 tools/spec_analyze.py /tmp/audio.ndjson --json out.json
    python3 tools/spec_analyze.py /tmp/audio.ndjson --png spec.png   # 带刻度的语谱图

需要 numpy。（`spec_view.py` 那头刻意不依赖第三方库，因为它要在采集现场跑、
装不了东西最难受；分析这头没有这个顾虑，而 numpy 省下的代码量是实打实的。）
"""
import argparse
import json
import math
import struct
import sys
import zlib

import numpy as np

# ---------------------------------------------------------------------------
# 加载
# ---------------------------------------------------------------------------


class Capture:
    """一段采集。db 存成 uint8（原始的 dB×2），要用时再转——长时采集帧数很大，
    10 小时约 16 万帧，uint8 是 20MB，float64 直接 165MB。"""

    def __init__(self, ms, rms, db, sr, hz):
        self.ms = ms            # 设备 millis()，不均匀
        self.rms = rms          # 时域宽带 RMS，原始量纲
        self.db = db            # (帧, bin) uint8，值 = dB×2
        self.sr = sr
        self.hz = hz            # 每 bin 多少 Hz
        self.nbin = db.shape[1]
        self.fft = (self.nbin - 1) * 2
        self.win_s = self.fft / sr                       # 每帧看了多久
        gaps = np.diff(ms) / 1000.0
        self.gap_s = float(np.median(gaps)) if len(gaps) else float("nan")
        self.fps = 1.0 / self.gap_s if self.gap_s > 0 else float("nan")
        self.duty = self.win_s / self.gap_s if self.gap_s > 0 else float("nan")
        self.dur_s = (ms[-1] - ms[0]) / 1000.0 if len(ms) > 1 else 0.0
        self.gaps = gaps

    def dbf(self):
        """转成真正的 dB（float）。"""
        return self.db.astype(np.float32) / 2.0

    def freqs(self):
        return np.arange(self.nbin) * self.hz


def load(path):
    ms, rms, rows = [], [], []
    sr = hz = None
    bad = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                p = json.loads(line)
                raw = p["db"]
                row = bytes.fromhex(raw)
            except (json.JSONDecodeError, KeyError, ValueError):
                bad += 1
                continue
            if sr is None:
                sr, hz = p["sr"], p["hz"]
            if len(row) != p["n"]:
                bad += 1
                continue
            ms.append(p["ms"])
            rms.append(p["rms"])
            rows.append(row)
    if not rows:
        sys.exit(f"{path} 里没有可用的帧（坏行 {bad}）")
    db = np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(len(rows), -1)
    cap = Capture(np.array(ms, dtype=np.int64), np.array(rms, dtype=np.float32),
                  db, sr, hz)
    cap.bad = bad
    return cap


# ---------------------------------------------------------------------------
# 本底
# ---------------------------------------------------------------------------

# bin0 是 DC bin。麦克风链路的直流偏置全堆在这儿（实测比邻居高 20 多 dB），
# 它跟「声音」没有任何关系，一切分析都必须先把它扔掉，否则谱质心会被死死拽向 0Hz。
FIRST_BIN = 1


def noise_floor(cap, pct=10):
    """每个 bin 取低分位数当本底。

    用分位数而不是最小值：最小值会撞上偶发的量化归零（实测有 bin 掉到 0dB），
    那是坏点不是本底。用 10% 分位的前提是「大部分时间这个 bin 处于本底」——
    对环境记录成立，对一段从头吵到尾的录音不成立，那种情况本底会被高估、
    所有相对量一起偏小，报告里会标出来。"""
    return np.percentile(cap.dbf(), pct, axis=0)


# ---------------------------------------------------------------------------
# 帧级特征
# ---------------------------------------------------------------------------


def features(cap, floor):
    """逐帧算一组标准谱特征。

    全部在**扣掉本底之后**算：本底在 4kHz 以上平坦、在低频抬起 15~20dB，
    不扣的话每一帧的谱质心都会被本底的形状主导，帧和帧之间反而分不出差别。
    """
    db = cap.dbf()[:, FIRST_BIN:]
    fl = floor[FIRST_BIN:]
    f = cap.freqs()[FIRST_BIN:]

    excess = np.maximum(db - fl, 0.0)                  # 高出本底多少 dB
    # dB 是 20log10(幅度)，功率 ∝ 幅度²，所以功率权重是 10^(dB/10)。
    # 减 1 是让「正好在本底」的 bin 权重归零，不然本底自己就贡献一大片权重。
    w = np.power(10.0, excess / 10.0) - 1.0
    tot = w.sum(axis=1)
    silent = tot <= 1e-6        # 整帧一个 bin 都没超过本底。⚠️ 这跟状态标签里的
                                # "quiet" 不是一回事：那个是按宽带电平相对安静基准
                                # 判的，会把「比本底高一点点但还是很轻」也算 quiet。
                                # 两个都叫「安静」曾让报告自相矛盾（100% vs 67%）。

    safe = np.where(silent, 1.0, tot)
    centroid = (w * f).sum(axis=1) / safe
    var = (w * (f - centroid[:, None]) ** 2).sum(axis=1) / safe
    bandwidth = np.sqrt(np.maximum(var, 0))

    # 谱滚降：能量累积到 85% 的频率
    cum = np.cumsum(w, axis=1)
    idx = np.argmax(cum >= 0.85 * tot[:, None], axis=1)
    rolloff = f[idx]

    # 谱平坦度（Wiener entropy）：几何均值 / 算术均值，在功率域算。
    # 1 = 白噪声那样平，趋近 0 = 能量全挤在少数 bin 里（有调）。
    # 在**扣掉本底之后**的功率上算：本底自己在低频抬了近 20dB，直接拿原始谱算的话
    # 每一帧都被这个固定的形状拽低，安静房间也报 0.31，看着像"有调"其实是本底的形状。
    p = np.power(10.0, excess / 10.0)
    flat = np.exp(np.mean(np.log(p + 1e-9), axis=1)) / (np.mean(p, axis=1) + 1e-9)

    # 谱通量：跟上一帧比变了多少。⚠️ 相邻帧隔了 224ms，这不是常规意义上
    # 「帧间」的通量（那通常是 10ms 量级），只能当「这 224ms 里换场景了没」用。
    d = np.diff(db, axis=0, prepend=db[:1])
    flux = np.sqrt((np.maximum(d, 0) ** 2).mean(axis=1))

    centroid[silent] = 0.0
    bandwidth[silent] = 0.0
    rolloff[silent] = 0.0

    spl = 20 * np.log10(np.maximum(cap.rms, 1e-3))     # 相对 dB，无绝对标定

    return dict(excess=excess, centroid=centroid, bandwidth=bandwidth,
                rolloff=rolloff, flatness=flat, flux=flux, spl=spl,
                energy=tot, silent=silent)


# ---------------------------------------------------------------------------
# 窄带音
# ---------------------------------------------------------------------------


def spectral_envelope(x, k=9):
    """滑动中值当谱包络。

    两个讲究：
    1. 用**中值**不用均值——均值会被要检测的那个峰自己抬起来，峰越强被抬得越多，
       突出度就被自己吃掉了；中值对少数几个高点免疫。
    2. 边界用**边缘复制**填充。第一版用 np.convolve(mode='same')，边界处的窗口
       被零填充截断、包络被压低，结果 bin126~128 凭空出现 +14dB 的「窄带音」——
       纯粹是分析假象。凡是滑窗，边界都得单独交代清楚。
    """
    half = k // 2
    pad = np.pad(x, half, mode="edge")
    out = np.empty_like(x, dtype=np.float64)
    for i in range(len(x)):
        out[i] = np.median(pad[i:i + k])
    return out


def find_tones(cap, floor, min_prom=6.0, min_frac=0.5):
    """找**持续存在**的窄带音。

    判据两条，缺一不可：
    - 突出度：在该帧谱里比局部包络高 min_prom dB 以上
    - 持续性：至少在 min_frac 比例的帧里都满足上一条

    只报持续音是故意的：7% 的占空比下，「偶尔一帧冒出个峰」大概率是噪声涨落或
    一次采样正好撞上某个瞬态，报出来没法复核；而持续音（风扇/变压器/电子啸叫）
    恰恰是这份数据最擅长、也最可信的东西。
    """
    db = cap.dbf()
    nf = db.shape[0]
    hits = np.zeros(cap.nbin, dtype=np.int32)
    prom_sum = np.zeros(cap.nbin)

    for i in range(nf):
        row = db[i].astype(np.float64)
        env = spectral_envelope(row)
        prom = row - env
        m = np.zeros(cap.nbin, dtype=bool)
        m[FIRST_BIN:] = prom[FIRST_BIN:] >= min_prom
        # 只留局部极大，免得一个宽峰的肩膀被算成好几个独立音
        for b in np.flatnonzero(m):
            lo, hi = max(FIRST_BIN, b - 1), min(cap.nbin - 1, b + 1)
            if row[b] < row[lo:hi + 1].max():
                m[b] = False
        hits[m] += 1
        prom_sum[m] += prom[m]

    tones = []
    for b in np.flatnonzero(hits >= min_frac * nf):
        tones.append(dict(
            bin=int(b),
            hz=float(b * cap.hz),
            frac=float(hits[b] / nf),
            prom=float(prom_sum[b] / hits[b]),
            level=float(np.median(db[:, b])),
            over_floor=float(np.median(db[:, b]) - floor[b]),
        ))
    tones.sort(key=lambda t: -t["prom"])
    return tones


# ---------------------------------------------------------------------------
# 分段
# ---------------------------------------------------------------------------

STATES = ("quiet", "tonal", "broadband", "mixed")


def classify(cap, feat, floor):
    """给每帧贴一个状态标签。

    阈值是相对本底定的，不是绝对值——不同房间、不同增益下绝对电平没有可比性，
    但「比这个环境自己的安静时刻高多少」在哪都成立。
    """
    spl = feat["spl"]
    base = np.percentile(spl, 10)
    rel = spl - base

    # 每帧最强的窄带突出度
    db = cap.dbf()
    peak_prom = np.zeros(len(db))
    for i in range(len(db)):
        row = db[i].astype(np.float64)
        prom = row - spectral_envelope(row)
        peak_prom[i] = prom[FIRST_BIN:].max()

    lab = np.empty(len(db), dtype=object)
    for i in range(len(db)):
        if rel[i] < 3.0:
            lab[i] = "quiet"
        elif peak_prom[i] >= 8.0:
            lab[i] = "tonal" if feat["flatness"][i] < 0.5 else "mixed"
        elif feat["flatness"][i] >= 0.5:
            lab[i] = "broadband"
        else:
            lab[i] = "mixed"
    return lab, rel, peak_prom


def segments(cap, lab, min_frames=3):
    """把逐帧标签合成连续段，短于 min_frames 的段并进邻居。

    min_frames=3 ≈ 0.7 秒。比这更短的「段」在 4.5fps 下就是一两帧，
    多半是分类抖动而不是环境真的变了。
    """
    segs = []
    start = 0
    for i in range(1, len(lab) + 1):
        if i == len(lab) or lab[i] != lab[start]:
            segs.append([start, i, lab[start]])
            start = i
    # 吸收过短的段
    out = []
    for s in segs:
        if out and s[1] - s[0] < min_frames:
            out[-1][1] = s[1]
        else:
            out.append(s)
    merged = []
    for s in out:
        if merged and merged[-1][2] == s[2]:
            merged[-1][1] = s[1]
        else:
            merged.append(s)
    return [dict(i0=a, i1=b, state=c,
                 t0=float((cap.ms[a] - cap.ms[0]) / 1000.0),
                 t1=float((cap.ms[min(b, len(cap.ms) - 1)] - cap.ms[0]) / 1000.0))
            for a, b, c in merged]


# ---------------------------------------------------------------------------
# 语谱图（带刻度，分位数拉伸）
# ---------------------------------------------------------------------------


def png_write(path, rows, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += bytes(rows[y])

    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def render(cap, path, scale=3, lo_pct=5, hi_pct=99.5):
    """语谱图。跟 spec_view.py 的 render() 有两处不同：

    1. **按分位数拉伸**，不用 min/max。实测有 bin 会量化到 0dB，最大值又能到 84dB，
       min/max 拉伸等于把 84dB 塞进 255 级，本底那点结构全糊成一片灰噪。
    2. **放大 + 画刻度线**，129 像素高的原图人眼没法读出「这条亮线是几赫兹」。
    """
    db = cap.dbf()[:, FIRST_BIN:]
    lo, hi = np.percentile(db, lo_pct), np.percentile(db, hi_pct)
    span = max(hi - lo, 1.0)
    img = np.clip((db - lo) * 255.0 / span, 0, 255).astype(np.uint8)
    img = img[:, ::-1].T                                # 频率轴翻转：低频在下
    img = np.repeat(np.repeat(img, scale, axis=0), scale, axis=1)
    h, w = img.shape

    # 每 1kHz 一条刻度线（画成虚线，免得盖住数据）
    for khz in range(1, int(cap.nbin * cap.hz / 1000) + 1):
        b = khz * 1000.0 / cap.hz - FIRST_BIN
        y = int((db.shape[1] - 1 - b) * scale)
        if 0 <= y < h:
            img[y, ::6] = 255
    rows = [img[y].tolist() for y in range(h)]
    png_write(path, rows, w, h)
    return w, h, lo, hi


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------


def report(cap, floor, feat, tones, lab, segs, rel, peak_prom):
    L = []
    a = L.append

    a("=" * 68)
    a("采集")
    a("=" * 68)
    a(f"  {len(cap.ms)} 帧 / {cap.dur_s:.1f}s"
      + (f"，坏行 {cap.bad}" if cap.bad else ""))
    a(f"  {cap.fft} 点 FFT @ {cap.sr}Hz → 每帧窗长 {cap.win_s*1000:.1f}ms，"
      f"{cap.nbin} bin × {cap.hz:.1f}Hz，覆盖 0~{cap.hz*(cap.nbin-1)/1000:.1f}kHz")
    a(f"  帧间隔中位 {cap.gap_s*1000:.0f}ms（{cap.fps:.2f} fps），"
      f"抖动 p10~p90 {np.percentile(cap.gaps,10)*1000:.0f}~{np.percentile(cap.gaps,90)*1000:.0f}ms")

    a("")
    a("=" * 68)
    a("这份数据的能力边界")
    a("=" * 68)
    a(f"  占空比 {cap.duty*100:.1f}% —— {100-cap.duty*100:.1f}% 的时间没有被看到")
    a(f"  rms 序列 Nyquist = {cap.fps/2:.2f}Hz，比这快的周期性调制一律混叠，不可信")
    a("  事件被至少采到一帧的概率 =（事件时长 + 窗长）/ 帧间隔：")
    for T in (0.03, 0.1, 0.3, 1.0):
        p = min(1.0, (T + cap.win_s) / cap.gap_s)
        mark = "  ← 不可靠" if p < 0.9 else ""
        a(f"      {T*1000:6.0f}ms 的事件 → {p*100:5.1f}%{mark}")
    a("  → 可信的是「持续存在的东西」和「长时统计」，不是「某一下发生了没有」。")

    a("")
    a("=" * 68)
    a("本底")
    a("=" * 68)
    hi_band = floor[cap.nbin // 2:]
    a(f"  4kHz 以上本底 {hi_band.mean():.1f}dB（±{hi_band.std():.1f}），基本是传感器/量化底")
    a(f"  低频抬升：bin1({cap.hz:.0f}Hz) {floor[1]:.1f}dB，"
      f"bin2 {floor[2]:.1f}dB，bin3 {floor[3]:.1f}dB")
    a(f"  bin0(DC) {floor[0]:.1f}dB —— 已丢弃，那是直流偏置不是声音")
    quiet_frac = (lab == "quiet").mean()
    if quiet_frac < 0.3:
        a(f"  ⚠️ 只有 {quiet_frac*100:.0f}% 的帧处于安静态，本底（取 10% 分位）多半被高估，"
          "所有「高出本底多少」的读数会偏小")

    a("")
    a("=" * 68)
    a("电平")
    a("=" * 68)
    spl = feat["spl"]
    a(f"  相对 dB（无绝对标定，只能自比）：安静基准 {np.percentile(spl,10):.1f}，"
      f"中位 {np.median(spl):.1f}，p95 {np.percentile(spl,95):.1f}，峰 {spl.max():.1f}")
    a(f"  动态范围（p95 - 安静基准）{np.percentile(spl,95)-np.percentile(spl,10):.1f}dB")

    a("")
    a("=" * 68)
    a("持续窄带音")
    a("=" * 68)
    if not tones:
        a("  没有。（判据：比局部谱包络高 ≥6dB，且在 ≥50% 的帧里都成立）")
    else:
        for t in tones:
            a(f"  {t['hz']:7.1f}Hz (bin{t['bin']:3d})  突出 {t['prom']:5.1f}dB  "
              f"出现率 {t['frac']*100:5.1f}%  高出本底 {t['over_floor']:5.1f}dB")
        a("")
        a("  ⚠️ 频率分辨率就是 bin 宽本身，报出来的 Hz 只精确到 ±%.0fHz。"
          % (cap.hz / 2))

    a("")
    a("=" * 68)
    a("谱形状（仅统计非安静帧）")
    a("=" * 68)
    act = ~feat["quiet"]
    if act.sum() == 0:
        a("  全程都在本底上，没有可分析的帧。")
    else:
        a(f"  非安静帧 {act.sum()}/{len(act)}（{act.mean()*100:.0f}%）")
        a(f"  谱质心   中位 {np.median(feat['centroid'][act]):7.1f}Hz  "
          f"p10~p90 {np.percentile(feat['centroid'][act],10):.0f}~{np.percentile(feat['centroid'][act],90):.0f}Hz")
        a(f"  谱带宽   中位 {np.median(feat['bandwidth'][act]):7.1f}Hz")
        a(f"  85% 滚降 中位 {np.median(feat['rolloff'][act]):7.1f}Hz")
        a(f"  谱平坦度 中位 {np.median(feat['flatness'][act]):7.3f}  "
          "（→1 越像白噪声，→0 越有调）")

    a("")
    a("=" * 68)
    a("时间线")
    a("=" * 68)
    tally = {}
    for s in segs:
        tally[s["state"]] = tally.get(s["state"], 0) + (s["i1"] - s["i0"])
    total = len(lab)
    for st in STATES:
        if st in tally:
            a(f"  {st:10s} {tally[st]*100/total:5.1f}%  ({tally[st]} 帧)")
    a("")
    show = [s for s in segs if s["state"] != "quiet"] or segs
    for s in show[:20]:
        a(f"  {s['t0']:7.1f}s ~ {s['t1']:7.1f}s  {s['state']:10s} "
          f"({s['i1']-s['i0']:3d} 帧)")
    if len(show) > 20:
        a(f"  ...（还有 {len(show)-20} 段）")

    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description="Cardputer 频谱流分析")
    ap.add_argument("ndjson")
    ap.add_argument("--json", help="把结构化结果写到这里")
    ap.add_argument("--png", help="渲染带刻度的语谱图")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--tone-prom", type=float, default=6.0,
                    help="窄带音突出度门限 dB（默认 6）")
    ap.add_argument("--tone-frac", type=float, default=0.5,
                    help="窄带音出现率门限（默认 0.5）")
    a = ap.parse_args()

    cap = load(a.ndjson)
    floor = noise_floor(cap)
    feat = features(cap, floor)
    tones = find_tones(cap, floor, a.tone_prom, a.tone_frac)
    lab, rel, peak_prom = classify(cap, feat, floor)
    segs = segments(cap, lab)

    print(report(cap, floor, feat, tones, lab, segs, rel, peak_prom))

    if a.png:
        w, h, lo, hi = render(cap, a.png, a.scale)
        print(f"\n语谱图 {a.png}  {w}x{h}  灰阶映射 {lo:.0f}~{hi:.0f}dB（5%~99.5% 分位）")

    if a.json:
        out = dict(
            capture=dict(frames=len(cap.ms), duration_s=cap.dur_s, fps=cap.fps,
                         gap_ms=cap.gap_s * 1000, window_ms=cap.win_s * 1000,
                         duty=cap.duty, sr=cap.sr, nbin=cap.nbin, hz=cap.hz,
                         nyquist_hz=cap.fps / 2),
            floor=dict(hi_band_db=float(floor[cap.nbin // 2:].mean()),
                       per_bin_db=[round(float(x), 1) for x in floor]),
            level=dict(quiet_base_db=float(np.percentile(feat["spl"], 10)),
                       median_db=float(np.median(feat["spl"])),
                       p95_db=float(np.percentile(feat["spl"], 95)),
                       peak_db=float(feat["spl"].max())),
            tones=tones,
            states={st: float((lab == st).mean()) for st in STATES},
            segments=segs,
        )
        with open(a.json, "w") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
        print(f"结构化结果 → {a.json}")


if __name__ == "__main__":
    main()

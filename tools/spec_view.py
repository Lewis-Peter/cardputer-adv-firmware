#!/usr/bin/env python3
"""
Cardputer 声学采集 —— 把设备的频谱流拉回电脑，存成可分析的格式。

分工是有意这么切的：**设备只管采集，语义留给电脑**。
ESP32-S3 没有 PSRAM（主画布就吃 64KB，最大连续块只剩 60 来 KB），装不下任何像样的
音频分类模型；但它有麦克风、能带出门、能用电池跑。电脑反过来——有算力有模型，
就是长不出麦克风。所以设备端一行语义都不做，只吐频谱。

固件端：进 Spectrum 页按 `s` 开始，每帧打一行
    SPEC {"ms":..,"sr":16000,"n":129,"hz":62.50,"rms":..,"db":"<258 hex>"}
`db` 是每个频点的 dB×2（0.5dB 一格），**取自自动增益之前的原始幅度**——
屏幕上那套 AGC 会把安静和响亮都拉到满量程，绝对电平一丢，"这声音有多大"就没了。

用法：
    python3 tools/spec_view.py --log /tmp/audio.ndjson          # 采集
    python3 tools/spec_view.py --log /tmp/audio.ndjson --png /tmp/spec.png
    python3 tools/spec_view.py --render /tmp/audio.ndjson --png /tmp/spec.png   # 只渲染已有日志

ndjson 每行一帧，字段见上。之后拿它做什么都行：
  - 渲染语谱图人眼看（--png）
  - 喂给本地模型做场景分类
  - 找周期性调制（直升机旋翼的桨叶通过频率在 10~25Hz，体现为宽带能量的周期起伏，
    对 rms 序列做 FFT 就能看见；⚠️ 采集是分帧突发的、不是连续录音，
    rms 序列的时间间隔不均匀，做调制分析前要按 ms 字段重采样）
需要 pyserial（采集时）：pip3 install pyserial
"""
import argparse, json, math, os, struct, sys, time, zlib

from serialport import find_port


def png_write(path, rows, w, h):
    """写灰度 PNG，不依赖任何第三方库（照抄 tools/shot.py 的做法）。"""
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


def render(frames, path):
    """语谱图：横轴时间（一帧一列）、纵轴频率（低频在下）、灰度=dB。"""
    if not frames:
        print("没有帧，不渲染"); return
    n = len(frames[0]["db"])
    w, h = len(frames), n
    lo = min(min(f["db"]) for f in frames)
    hi = max(max(f["db"]) for f in frames)
    span = max(hi - lo, 1)
    rows = [[0] * w for _ in range(h)]
    for x, f in enumerate(frames):
        for b in range(n):
            v = int((f["db"][b] - lo) * 255 / span)
            rows[h - 1 - b][x] = max(0, min(255, v))   # 低频画在下面
    png_write(path, rows, w, h)
    dur = (frames[-1]["ms"] - frames[0]["ms"]) / 1000.0
    print(f"{path}  {w}x{h}  {len(frames)} 帧 / {dur:.1f}s  "
          f"频率轴 0~{frames[0]['hz'] * (n - 1) / 1000:.1f}kHz  dB 范围 {lo/2:.0f}~{hi/2:.0f}")


def parse(line):
    p = json.loads(line)
    raw = p.pop("db")
    p["db"] = [int(raw[i:i + 2], 16) for i in range(0, len(raw), 2)]
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None, help="不给就自动找板子的串口")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--log", help="把每帧原样追加到这个 ndjson")
    ap.add_argument("--png", help="渲染语谱图到这个路径")
    ap.add_argument("--render", help="不采集，只把已有的 ndjson 渲染成图")
    ap.add_argument("-n", "--frames", type=int, default=0, help="采够这么多帧就停（0=一直采）")
    a = ap.parse_args()

    if a.render:
        frames = [parse(l) for l in open(a.render) if l.strip()]
        render(frames, a.png or "spec.png")
        return

    try:
        import serial
    except ImportError:
        sys.exit("需要 pyserial：pip3 install pyserial")

    port = find_port(a.port)
    s = serial.Serial(port, a.baud, timeout=1)
    print(f"连上 {port}。在设备上进 Spectrum 页按 s 开始推流。Ctrl-C 停止。")

    frames, logf = [], (open(a.log, "a") if a.log else None)
    t0 = time.time()
    try:
        while True:
            line = s.readline().decode("utf-8", "replace").strip()
            if not line.startswith("SPEC "):
                continue
            body = line[5:]
            try:
                p = parse(body)
            except (json.JSONDecodeError, ValueError):
                print("解析失败:", line[:80]); continue
            frames.append(p)
            if logf:
                logf.write(body + "\n"); logf.flush()
            if len(frames) % 10 == 0:
                peak = max(range(len(p["db"])), key=lambda i: p["db"][i])
                print(f"\r{len(frames)} 帧 / {time.time()-t0:.0f}s   "
                      f"rms {p['rms']:7.0f}   峰值 {peak*p['hz']/1000:5.2f}kHz", end="", flush=True)
            if a.frames and len(frames) >= a.frames:
                break
    except KeyboardInterrupt:
        pass
    finally:
        if logf: logf.close()
        s.close()
        print(f"\n共 {len(frames)} 帧" + (f"，已存 {a.log}" if a.log else ""))
    if a.png:
        render(frames, a.png)


if __name__ == "__main__":
    main()

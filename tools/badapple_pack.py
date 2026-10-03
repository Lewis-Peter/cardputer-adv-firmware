#!/usr/bin/env python3
"""把一段视频打包成 badapple.cpp 认的 1bpp 格式，供 SD 卡上的 /video/badapple.dat 用。

为什么不直接存原视频丢给固件解码：板子没 PSRAM，也没有 H.264/VP9 解码器，塞不下任何
标准视频编解码。240x135 屏幕本来就撑不起灰阶/彩色的细节，Bad Apple 这类近乎纯黑白剪影
的内容干脆存成 1bpp 位图序列——每帧只要 (W/8)*H 字节，SD 顺序读的带宽压根不是瓶颈
（240x135 @ 30fps 只要 121.5KB/s，20MHz SPI 随便甩），瓶颈在 SPI 推屏，所以格式越简单
越好：不压缩、不加行间 padding，直接是 LovyanGFX 1bpp Sprite 内部认的位序（MSB-first，
行按字节对齐）——固件那边整帧 memcpy 进 Sprite 缓冲区就能推屏，不用再转一次位。

处理流程：
  1. ffmpeg 缩放 + pillarbox（源常见是 4:3，屏幕是 16:9，两边留黑边而不是裁内容——
     Bad Apple 背景本来就是纯黑，黑边几乎无缝融入）+ 转灰阶，导出成 rawvideo。
     ⚠️ 这台机器上 ffmpeg 直接转 -pix_fmt monob 到 rawvideo 会因为自动插入的 format
     协商触发 "Padded dimensions cannot be smaller than input dimensions" 报错
     （filter graph reinit 的一个怪癖，具体版本见 `ffmpeg -version`）。绕过办法就是
     不让 ffmpeg 自己做 1bpp 转换，导出 8bit 灰阶交给下面的 numpy 步骤做阈值化，
     顺便也把 dither 要不要开的决定权留在这边——实测阈值化比 Floyd-Steinberg 干净
     （纯剪影内容没有渐变，dither 只会在纯黑/纯白色块里添噪点）。
  2. numpy 阈值化 + packbits（bitorder="big"，正好是 MSB-first）打包成最终二进制。

用法：
    python3 tools/badapple_pack.py source.mp4 -o /path/to/badapple.dat
    python3 tools/badapple_pack.py source.mp4 -o out.dat --fps 20   # 帧率打不满时降帧率用

依赖：ffmpeg/ffprobe（系统装的即可）、numpy。
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

W, H = 240, 135   # Cardputer ADV 的 ST7789 分辨率，横屏 rotation=1 后固定这个数


def probe_duration(video_path: str) -> float:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "json", video_path],
        capture_output=True, text=True, check=True,
    )
    return float(json.loads(out.stdout)["format"]["duration"])


def extract_gray_raw(video_path: str, raw_path: str, fps: int, content_w: int) -> None:
    pad_x = (W - content_w) // 2
    vf = f"fps={fps},scale={content_w}:{H}:flags=lanczos,pad={W}:{H}:{pad_x}:0:black,format=gray"
    cmd = ["ffmpeg", "-y", "-i", video_path, "-vf", vf, "-pix_fmt", "gray",
           "-f", "rawvideo", raw_path]
    subprocess.run(cmd, check=True, capture_output=True)


def pack(raw_path: str, out_path: str, fps: int) -> int:
    frame_sz = W * H
    data = np.fromfile(raw_path, dtype=np.uint8)
    n_frames = len(data) // frame_sz
    data = data[:n_frames * frame_sz].reshape(n_frames, H, W)

    bits = data >= 128
    packed = np.packbits(bits, axis=-1, bitorder="big")   # MSB-first, byte-aligned rows

    with open(out_path, "wb") as f:
        # struct BadAppleHeader{ char magic[4]; u16 w,h,fps,reserved; u32 frameCount; } — 见 badapple.cpp
        f.write(struct.pack("<4sHHHHI", b"BAP1", W, H, fps, 0, n_frames))
        f.write(packed.tobytes())
    return n_frames


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video", help="源视频（ffmpeg 能读的格式都行）")
    ap.add_argument("-o", "--out", default="badapple.dat", help="输出文件路径")
    ap.add_argument("--fps", type=int, default=30, help="目标帧率（默认 30；SPI 推屏跟不上就降这个）")
    ap.add_argument("--content-width", type=int, default=180,
                     help="pillarbox 内容宽度（默认 180=4:3 内容配 135 高，两边各留 30px 黑边）")
    args = ap.parse_args()

    dur = probe_duration(args.video)
    print(f"source: {args.video} ({dur:.1f}s)")

    with tempfile.TemporaryDirectory() as tmp:
        raw_path = os.path.join(tmp, "gray.raw")
        print(f"ffmpeg: extracting {args.fps}fps grayscale, pillarboxed to {W}x{H}...")
        extract_gray_raw(args.video, raw_path, args.fps, args.content_width)
        raw_sz = os.path.getsize(raw_path)
        print(f"  {raw_sz} bytes raw gray ({raw_sz // (W*H)} frames)")

        print("packing to 1bpp...")
        n = pack(raw_path, args.out, args.fps)

    out_sz = os.path.getsize(args.out)
    print(f"wrote {args.out}: {n} frames, {out_sz} bytes ({out_sz/1024/1024:.1f} MiB)")
    print(f"expected playback: {n/args.fps:.1f}s @ {args.fps}fps")
    print(f"copy to SD card as /video/badapple.dat")


if __name__ == "__main__":
    main()

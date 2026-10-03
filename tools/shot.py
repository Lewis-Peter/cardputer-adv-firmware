#!/usr/bin/env python3
"""串口截屏：给设备发 SHOT，把它吐回来的 RGB565 hex 拼成 PNG。

板子插着的时候，这是唯一能"真看到屏幕"的办法——比拿手机拍屏幕清楚，也比在模拟器里
猜坐标可靠（模拟器只能跑不依赖硬件的页面）。

    python3 tools/shot.py                        # 截当前画面
    python3 tools/shot.py --cmd GNSSMAP          # 先跳到某个页面再截
    python3 tools/shot.py -o /tmp/x.png --scale 4
"""
import argparse
import re
import sys
import time
import zlib

import serial

from serialport import find_port


def png_write(path, rows, w, h, scale):
    raw = bytearray()
    for y in range(h * scale):
        raw.append(0)                       # filter type 0
        src = rows[y // scale]
        for x in range(w * scale):
            r, g, b = src[x // scale]
            raw += bytes((r, g, b))

    def chunk(typ, data):
        c = typ + data
        return (len(data).to_bytes(4, 'big') + c +
                (zlib.crc32(c) & 0xffffffff).to_bytes(4, 'big'))

    ihdr = (w * scale).to_bytes(4, 'big') + (h * scale).to_bytes(4, 'big') + bytes((8, 2, 0, 0, 0))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 6)))
        f.write(chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', default=None, help='不给就自动找板子的串口')
    ap.add_argument('-o', '--out', default='shot.png')
    ap.add_argument('--cmd', help='截图前先发一条指令（比如 GNSSMAP）')
    ap.add_argument('--scale', type=int, default=3, help='放大倍数，240x135 原图太小看不清')
    ap.add_argument('--wait', type=float, default=0, help='发完 --cmd 等几秒再截（等页面加载完）')
    a = ap.parse_args()

    s = serial.Serial(find_port(a.port), 115200, timeout=0.5)
    time.sleep(2.0 if a.cmd else 0.8)
    s.reset_input_buffer()

    if a.cmd:
        s.write((a.cmd + '\n').encode())
        time.sleep(max(a.wait, 0.5))
        s.reset_input_buffer()

    s.write(b'SHOT\n')

    w = h = None
    rows = {}
    t0 = time.time()
    while time.time() - t0 < 60:
        line = s.readline().decode('utf-8', 'replace').strip()
        if not line:
            continue
        m = re.match(r'\[shot\] (\d+)x(\d+)', line)
        if m:
            w, h = int(m.group(1)), int(m.group(2))
            continue
        m = re.match(r'\[shotrow\] (\d+) ([0-9A-Fa-f]+)$', line)
        if m and w:
            y, hexs = int(m.group(1)), m.group(2)
            if len(hexs) != w * 4:
                print(f'  第 {y} 行长度不对（{len(hexs)} != {w*4}），丢弃', file=sys.stderr)
                continue
            px = []
            for i in range(0, len(hexs), 4):
                c = int(hexs[i:i + 4], 16)
                px.append((((c >> 11) & 0x1f) * 255 // 31,
                           ((c >> 5) & 0x3f) * 255 // 63,
                           (c & 0x1f) * 255 // 31))
            rows[y] = px
            continue
        if line.startswith('[shot] end'):
            break
    s.close()

    if not w or len(rows) < h:
        print(f'只收到 {len(rows)}/{h or "?"} 行，截图不完整', file=sys.stderr)
        if not rows:
            sys.exit(1)
        black = [(0, 0, 0)] * w
        for y in range(h):
            rows.setdefault(y, black)

    png_write(a.out, [rows[y] for y in range(h)], w, h, a.scale)
    print(f'{a.out}  ({w*a.scale}x{h*a.scale}, 原始 {w}x{h})')


if __name__ == '__main__':
    main()

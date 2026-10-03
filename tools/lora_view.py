#!/usr/bin/env python3
"""
Cardputer LoRa 监听查看器 —— 把 Cardputer 从 USB 串口吐出来的 LoRa 包在 Mac 上表格化显示。

固件端：进入 LoRa app（LISTEN/AUTO）后，每收到一个包会在串口打印一行
    LORAPKT {"ms":..,"freq":..,"sf":..,"rssi":..,"snr":..,"len":..,"hex":".."}
本脚本只挑 LORAPKT 前缀的行解析、其它日志忽略（可 -v 显示）。

用法：
    python3 tools/lora_view.py                 # 自动找串口
    python3 tools/lora_view.py -p /dev/ttyACM0
    python3 tools/lora_view.py -v              # 也打印其它串口日志（灰色）
需要 pyserial：pip3 install pyserial
（或用 PlatformIO 自带的 python：
 /opt/homebrew/Cellar/platformio/*/libexec/bin/python3 tools/lora_view.py）
"""
import argparse, json, sys, time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial：pip3 install pyserial")

from serialport import find_port

C = {"reset":"\033[0m","dim":"\033[2m","red":"\033[31m","grn":"\033[32m",
     "yel":"\033[33m","cyn":"\033[36m","mag":"\033[35m","bold":"\033[1m"}

def rssi_col(r):
    if r >= -80: return C["grn"]
    if r >= -100: return C["yel"]
    return C["red"]

def fmt(pkt, t0):
    ms = pkt.get("ms", 0)
    rel = ms/1000.0
    freq = pkt.get("freq", 0.0)
    sf = pkt.get("sf", "?"); bw = pkt.get("bw", "?")
    rssi = pkt.get("rssi", 0); snr = pkt.get("snr", 0)
    crc = pkt.get("crc", 0); ln = pkt.get("len", 0)
    hexs = pkt.get("hex", "")
    hexshow = hexs if len(hexs) <= 64 else hexs[:64] + "…"

    if "from" in pkt:                       # 解出了 Meshtastic 帧头
        who = f'{C["cyn"]}{pkt["from"]}{C["reset"]}→{pkt.get("to","")}'
        hop = f'hop {pkt.get("hop","?")}/{pkt.get("hopStart","?")}'
        ack = " ACK" if pkt.get("ack") else ""
        mq  = " MQTT" if pkt.get("mqtt") else ""
        meta = f'{who}  ch{pkt.get("ch","?")} {hop}{ack}{mq}'
    else:
        meta = f'{C["dim"]}(no mesh header){C["reset"]}'

    crcflag = f'{C["red"]}CRC✗{C["reset"]}' if crc else f'{C["grn"]}ok{C["reset"]}'
    rc = rssi_col(rssi)
    return (f'{rel:8.1f}s {freq:8.3f}MHz SF{sf}/BW{bw:<3} '
            f'{rc}{rssi:4d}dBm{C["reset"]} snr{snr:+3d} {crcflag} '
            f'{ln:3d}B  {meta}  {C["dim"]}{hexshow}{C["reset"]}')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None, help="不给就自动找板子的串口")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-v", "--verbose", action="store_true", help="也显示其它串口日志")
    args = ap.parse_args()
    port = find_port(args.port)

    while True:
        try:
            s = serial.Serial(port, args.baud, timeout=0.5)
            break
        except Exception as e:
            print(f"{C['dim']}等待端口 {port} … ({e}){C['reset']}")
            time.sleep(1)

    print(f"{C['bold']}连上 {port} @ {args.baud}。进入 Cardputer 的 LoRa 监听即可看到包。Ctrl-C 退出。{C['reset']}")
    t0 = time.time(); n = 0
    try:
        while True:
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            if line.startswith("LORAPKT "):
                try:
                    pkt = json.loads(line[len("LORAPKT "):])
                    n += 1
                    print(f'{C["dim"]}#{n:<4}{C["reset"]}' + fmt(pkt, t0))
                except json.JSONDecodeError:
                    print(f'{C["red"]}解析失败：{line}{C["reset"]}')
            elif args.verbose and line:
                print(f'{C["dim"]}{line}{C["reset"]}')
    except KeyboardInterrupt:
        print(f"\n{C['bold']}共 {n} 个包。{C['reset']}")
        s.close()

if __name__ == "__main__":
    main()

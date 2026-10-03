#!/usr/bin/env python3
"""
Cardputer Drone ID 实时查看器 —— 无人机 Remote ID 的流式监听。

固件端：进入 Drone ID app 后，每解出一帧就在串口打印一行
    RIDPKT {"ms":..,"mac":"..","id":"..","lat":..,"lon":..,"agl":..,"spd":..,"state":..}
本脚本只挑 RIDPKT 前缀的行解析，其它日志忽略（-v 显示）。

为什么要有这个脚本（而不是继续用串口的 RIDSCAN）：
    RIDSCAN 是阻塞式的——扫 N 秒、最后一次性打印。在无人机飞行这种"环境每几秒就变"
    的场景下，每次采集都是一段失明期：现场读数变了，而这边还在等上一次扫描结束。
    改成流式之后，数据一直在流，任何时刻想知道当前值，读一下快照文件即可，延迟接近零。

    --snapshot 就是为这个准备的：脚本持续把每架无人机的最新状态写成一个 JSON 文件，
    人看终端表格，需要随时查当前值的一方（比如结对调试时的另一边）直接读那个文件。

用法：
    python3 tools/rid_view.py                        # 自动找串口
    python3 tools/rid_view.py -p /dev/ttyACM0
    python3 tools/rid_view.py --snapshot /tmp/rid.json --log /tmp/rid.ndjson
    python3 tools/rid_view.py -v                     # 也打印其它串口日志
需要 pyserial：pip3 install pyserial
"""
import argparse, json, math, os, sys, time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：pip3 install pyserial")

from serialport import find_port

C = {"reset": "\033[0m", "bold": "\033[1m", "dim": "\033[2m",
     "red": "\033[31m", "grn": "\033[32m", "yel": "\033[33m",
     "cya": "\033[36m", "mag": "\033[35m"}

STATE = {0: "未报告", 1: "地面", 2: "空中", 3: "紧急", 4: "识别失效", 5: "失效+紧急"}
CARD = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]


def bearing(lat1, lon1, lat2, lon2):
    d = math.pi / 180
    dlon = (lon2 - lon1) * d
    y = math.sin(dlon) * math.cos(lat2 * d)
    x = math.cos(lat1 * d) * math.sin(lat2 * d) - math.sin(lat1 * d) * math.cos(lat2 * d) * math.cos(dlon)
    b = math.atan2(y, x) / d
    return b + 360 if b < 0 else b


def dist_m(lat1, lon1, lat2, lon2):
    dn = (lat2 - lat1) * 111320.0
    de = (lon2 - lon1) * 111320.0 * math.cos(math.radians(lat1))
    return math.hypot(dn, de)


def fmt(p):
    """一行人类可读的状态。"""
    st = p.get("state")
    stxt = STATE.get(st, "?")
    col = C["grn"] if st == 2 else C["red"] if st == 3 else C["dim"]
    out = [f'{C["bold"]}{p.get("id", p["mac"])[:22]:<22}{C["reset"]}', f'{col}{stxt:<5}{C["reset"]}']

    if "lat" in p:
        out.append(f'{p["lat"]:.6f},{p["lon"]:.6f}')
        # 自己有定位就顺手算距离方位——这是现场最想知道的数
        if "mylat" in p:
            d = dist_m(p["mylat"], p["mylon"], p["lat"], p["lon"])
            b = bearing(p["mylat"], p["mylon"], p["lat"], p["lon"])
            c = CARD[int((b + 22.5) / 45) % 8]
            out.append(f'{C["cya"]}{d:6.0f}m {c:<2}{C["reset"]}')
    else:
        out.append(f'{C["dim"]}(无有效定位){C["reset"]}')

    if "agl" in p: out.append(f'离地{p["agl"]:5.1f}m')
    if "spd" in p: out.append(f'{p["spd"]:5.2f}m/s')
    if "hdg" in p: out.append(f'{p["hdg"]:3d}°')
    out.append(f'{C["dim"]}{p["rssi"]}dBm ch{p["ch"]}{C["reset"]}')
    if p.get("fake"): out.append(f'{C["mag"]}[FAKE]{C["reset"]}')
    return "  ".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None, help="不给就自动找板子的串口")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-v", "--verbose", action="store_true", help="也打印其它串口日志")
    ap.add_argument("--snapshot", help="持续写入每架无人机的最新状态（JSON），供随时读取")
    ap.add_argument("--log", help="把每条 RIDPKT 原样追加到这个文件（ndjson）")
    a = ap.parse_args()

    port = find_port(a.port)

    s = serial.Serial(port, a.baud, timeout=1)
    print(f'{C["bold"]}连上 {port} @ {a.baud}。在设备上进 SIGNAL → Drone ID 即可看到数据。'
          f'Ctrl-C 退出。{C["reset"]}')
    if a.snapshot:
        print(f'{C["dim"]}快照持续写入 {a.snapshot}{C["reset"]}')

    latest, n = {}, 0
    logf = open(a.log, "a") if a.log else None
    try:
        while True:
            try:
                line = s.readline().decode("utf-8", "replace").strip()
            except serial.SerialException as e:
                print(f'{C["red"]}串口断了：{e}{C["reset"]}'); break
            if not line:
                continue
            if not line.startswith("RIDPKT "):
                if a.verbose: print(f'{C["dim"]}{line}{C["reset"]}')
                continue
            try:
                p = json.loads(line[7:])
            except json.JSONDecodeError:
                print(f'{C["red"]}解析失败：{line}{C["reset"]}'); continue

            n += 1
            p["recv"] = time.time()
            latest[p["mac"]] = p
            print(f'{C["dim"]}#{n:<4}{C["reset"]}' + fmt(p))
            if logf:
                logf.write(line[7:] + "\n"); logf.flush()
            if a.snapshot:
                # 原子替换，免得读的那一方撞见写到一半的文件
                tmp = a.snapshot + ".tmp"
                with open(tmp, "w") as f:
                    json.dump({"updated": time.time(), "drones": list(latest.values())},
                              f, ensure_ascii=False, indent=1)
                os.replace(tmp, a.snapshot)
    except KeyboardInterrupt:
        pass
    finally:
        if logf: logf.close()
        s.close()
        print(f'\n{C["bold"]}共 {n} 帧，{len(latest)} 架。{C["reset"]}')


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""串口自动探测 —— 四个查看器脚本共用。

原先每个脚本各自写死 `/dev/cu.usbmodem1101`（或 glob `cu.usbmodem*`），换到 Linux
上全都要手动 `-p`。板子是 ESP32-S3 原生 USB，VID 固定 0x303A，直接按 VID 认最准：
macOS 枚举成 /dev/cu.usbmodem*，Linux 是 /dev/ttyACM*，不用管命名差异。
"""
import glob
import sys

ESP_VID = 0x303A          # Espressif；S3 的原生 USB JTAG/serial


def find_port(explicit=None):
    """返回串口设备路径。explicit 非空就直接用它，否则自动找，找不到就退出。"""
    if explicit:
        return explicit

    try:
        from serial.tools import list_ports
        cands = [p.device for p in list_ports.comports() if p.vid == ESP_VID]
        if cands:
            return sorted(cands)[0]
    except ImportError:
        pass                                # 没装 pyserial 时退回纯 glob

    # VID 认不出来时的兜底（比如经过某些 USB hub / 虚拟机透传）
    cands = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not cands:
        sys.exit("找不到板子的串口（/dev/cu.usbmodem* 或 /dev/ttyACM*），用 -p 指定")
    return cands[0]

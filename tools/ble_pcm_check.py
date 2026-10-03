#!/usr/bin/env python3
"""ble_pcm_check.py —— Cardputer ADV BLE & PC MODE 自动化协议验证脚本

基于 docs/pc-mode-protocol.md §8.6 与固件实现规范进行严格断言：
1. 握手与能力：CAPS（normal / pc）、PCMODE 进入（enter + hello）、caps 包含 ble
2. BLE ON / BLE OFF 往返 30 次：记录每次 STAT 的 heap/largest/minEver 曲线
3. 数据平面协议校验：JSON 合法性、必选字段（t, ts, addr, at, rssi, phy）与可选字段（name, mfg, svc, sd, rid）
4. 射频互斥与冲突回执：
   - BLE ON 时 RID ON 回 busy
   - BLE ON 时 SCAN WIFI 回 busy 且无 PCM ack
   - RID ON 时 BLE ON 回 busy
5. 幂等与 PCEXIT 自动退流：
   - 重复 BLE OFF 均回 end 不报错
   - PCEXIT 自动关闭 BLE 流并回 end + exit
6. BTEXT 5 连续两次重初始化验证：
   - 验证控制器在干净状态下重启，started=1 且 reports>=0
"""

import argparse
import json
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要安装 pyserial: pip3 install pyserial")

STAT_RE = re.compile(r"\[stat\]\s+heap=(\d+)\s+largest=(\d+)\s+minEver=(\d+)\s+screen=(-?\d+)")
ADDR_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")


class BlePcmTester:
    def __init__(self, port, baud=115200, timeout=1.0, verbose=False):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.verbose = verbose
        self.ser = None
        self._buf = b""
        self.passed = 0
        self.failed = 0
        self.test_records = []
        self.curve_records = []

    def open(self):
        print(f"[*] 打开串口 {self.port} (波特率 {self.baud})...")
        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
        time.sleep(1.0)
        self.ser.reset_input_buffer()
        # 初始保持静音
        self.send_cmd("MUTE")
        time.sleep(0.3)
        self.drain_lines()

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_cmd(self, cmd: str):
        if self.verbose:
            print(f"  >>> {cmd}")
        self.ser.write((cmd + "\n").encode("utf-8"))
        self.ser.flush()

    def read_line(self, timeout=None):
        """返回一整行；超时返回 None，已收到的半行留在缓冲里等下次拼接。

        不能用 serial.readline()：超时时它会把还没收完的行从中间截断。
        USB CDC 本身有 >1s 的滞后，半行会被误判成非法 JSON。
        """
        deadline = time.time() + (self.timeout if timeout is None else timeout)
        while True:
            i = self._buf.find(b"\n")
            if i >= 0:
                raw, self._buf = self._buf[:i], self._buf[i + 1:]
                line = raw.decode("utf-8", "replace").strip()
                if self.verbose and line:
                    print(f"  <<< {line}")
                return line
            if time.time() >= deadline:
                return None
            self.ser.timeout = 0.05
            self._buf += self.ser.read(4096)

    def drain_lines(self, wait_sec=0.3):
        t0 = time.time()
        lines = []
        while time.time() - t0 < wait_sec:
            l = self.read_line(timeout=0.1)
            if l:
                lines.append(l)
        return lines

    def read_until(self, predicate, timeout_sec=3.0):
        t0 = time.time()
        collected = []
        while time.time() - t0 < timeout_sec:
            l = self.read_line(timeout=0.2)
            if l is not None:
                collected.append(l)
                if predicate(l):
                    return l, collected
        return None, collected

    def record_result(self, name: str, passed: bool, detail: str = ""):
        if passed:
            self.passed += 1
            print(f"  [PASS] {name} {detail}")
        else:
            self.failed += 1
            print(f"  [FAIL] {name} - {detail}")
        self.test_records.append({"name": name, "passed": passed, "detail": detail})

    def get_stat(self):
        self.send_cmd("STAT")
        time.sleep(0.1)
        stat_line, _ = self.read_until(lambda l: "[stat] heap=" in l, timeout_sec=2.0)
        if not stat_line:
            raise RuntimeError("未能从串口获取 STAT 响应")
        m = STAT_RE.search(stat_line)
        if not m:
            raise RuntimeError(f"解析 STAT 失败: {stat_line}")
        return {
            "heap": int(m.group(1)),
            "largest": int(m.group(2)),
            "minEver": int(m.group(3)),
            "screen": int(m.group(4)),
            "raw": stat_line,
        }

    # -------------------------------------------------------------------------
    # Phase 1: 握手与能力验证 (CAPS / PCMODE enter / hello)
    # -------------------------------------------------------------------------
    def test_handshake_and_caps(self):
        print("\n=======================================================")
        print("Phase 1: 握手与能力发现验证 (CAPS, PCMODE enter & hello)")
        print("=======================================================")
        # 1.1 普通模式下的 CAPS
        self.send_cmd("CAPS")
        caps_line, _ = self.read_until(lambda l: l.startswith("PCM ") and '"caps"' in l, 2.0)
        p1_ok = False
        if caps_line:
            try:
                doc = json.loads(caps_line[4:])
                p1_ok = (
                    doc.get("t") == "caps"
                    and doc.get("mode") in ("normal", "pc")
                    and any(c.get("id") == "ble" for c in doc.get("caps", []))
                )
            except Exception as e:
                caps_line = f"JSON parse error: {e}"
        self.record_result("Normal mode CAPS query", p1_ok, f"mode={doc.get('mode') if p1_ok else 'err'}")

        # 1.2 发送 PCMODE 进入 PC 主导模式
        self.send_cmd("PCMODE")
        enter_line, _ = self.read_until(lambda l: l.startswith('PCM {"t":"enter"}'), 2.0)
        self.record_result("PCMODE enter signal", enter_line is not None)

        hello_line, _ = self.read_until(lambda l: l.startswith('PCM {"t":"hello"'), 2.0)
        hello_ok = False
        if hello_line:
            try:
                hdoc = json.loads(hello_line[4:])
                hello_ok = (
                    hdoc.get("t") == "hello"
                    and hdoc.get("proto") == 1
                    and hdoc.get("proto_minor") == 1
                    and "ble" in hdoc.get("caps", [])
                )
            except Exception as e:
                hello_line = f"JSON parse error: {e}"
        self.record_result("PCMODE hello message (proto=1.1, caps contains ble)", hello_ok, f"{hello_line}")

        # 1.3 PC 模式下的 CAPS
        self.send_cmd("CAPS")
        caps_pcm, _ = self.read_until(lambda l: l.startswith("PCM ") and '"caps"' in l, 2.0)
        pcm_caps_ok = False
        if caps_pcm:
            try:
                pdoc = json.loads(caps_pcm[4:])
                pcm_caps_ok = (
                    pdoc.get("t") == "caps"
                    and pdoc.get("mode") == "pc"
                    and any(c.get("id") == "ble" and "BLE ON" in c.get("verbs", []) for c in pdoc.get("caps", []))
                )
            except Exception as e:
                caps_pcm = f"JSON parse error: {e}"
        self.record_result("PC MODE CAPS query (mode='pc', ble stream capability)", pcm_caps_ok)

    # -------------------------------------------------------------------------
    # Phase 2: BLE ON / BLE OFF 30 轮往返与内存曲线测量
    # -------------------------------------------------------------------------
    def test_ble_roundtrips(self, rounds=30):
        print("\n=======================================================")
        print(f"Phase 2: BLE ON / BLE OFF 往返 {rounds} 次 (记录内存曲线与报文格式)")
        print("=======================================================")
        baseline_stat = self.get_stat()
        print(f"[*] PC MODE BLE 前基线 STAT: heap={baseline_stat['heap']} largest={baseline_stat['largest']} minEver={baseline_stat['minEver']}")

        data_rows_validated = 0
        data_validation_errors = []

        for r in range(1, rounds + 1):
            # 2.1 BLE ON
            self.send_cmd("BLE ON")
            start_line, raw_lines = self.read_until(lambda l: l.startswith('BLE {"t":"start"}'), 2.0)
            if not start_line:
                self.record_result(f"Round {r:02d} BLE ON start", False, "未在 2s 内收到 BLE start")
                break

            # 读取 0.4s 广播并严格校验字段
            t_sample = time.time()
            while time.time() - t_sample < 0.4:
                l = self.read_line(timeout=0.1)
                if not l or not l.startswith("BLE "):
                    continue
                json_str = l[4:]
                try:
                    p = json.loads(json_str)
                    if p.get("t") == "d":
                        # 校验字段
                        if not isinstance(p.get("ts"), int) or p.get("ts") < 0:
                            data_validation_errors.append(f"ts 字段无效: {l}")
                        addr = p.get("addr")
                        if not isinstance(addr, str) or not ADDR_RE.match(addr):
                            data_validation_errors.append(f"addr 字段格式非 aa:bb:cc:dd:ee:ff: {l}")
                        if p.get("at") not in (0, 1, 2, 3):
                            data_validation_errors.append(f"at 字段无效: {l}")
                        if not isinstance(p.get("rssi"), int):
                            data_validation_errors.append(f"rssi 字段无效: {l}")
                        if p.get("phy") not in (1, 2, 3):
                            data_validation_errors.append(f"phy 字段无效: {l}")
                        if "name" in p and not isinstance(p.get("name"), str):
                            data_validation_errors.append(f"name 字段类型错误: {l}")
                        if "mfg" in p and not isinstance(p.get("mfg"), str):
                            data_validation_errors.append(f"mfg 字段类型错误: {l}")
                        if "svc" in p and (not isinstance(p.get("svc"), str) or len(p.get("svc")) != 4):
                            data_validation_errors.append(f"svc 字段格式错误: {l}")
                        data_rows_validated += 1
                except Exception as e:
                    data_validation_errors.append(f"非法 JSON: {l} ({e})")

            # 2.2 BLE OFF
            self.send_cmd("BLE OFF")
            end_line, _ = self.read_until(lambda l: l.startswith("BLE ") and '"t":"end"' in l, 2.0)
            if not end_line:
                self.record_result(f"Round {r:02d} BLE OFF end", False, "未在 2s 内收到 BLE end")
                break

            # 2.3 STAT 采样
            time.sleep(0.05)
            st = self.get_stat()
            self.curve_records.append({
                "round": r,
                "heap": st["heap"],
                "largest": st["largest"],
                "minEver": st["minEver"],
                "loopStackFree": st.get("loopStackFree", 0),
            })
            if r == 1 or r % 5 == 0 or r == rounds:
                print(f"  Round {r:02d}/{rounds:02d}: heap={st['heap']}  largest={st['largest']}  minEver={st['minEver']}")

        # 判定往返结果
        r30_completed = len(self.curve_records) == rounds
        self.record_result(f"BLE ON/OFF {rounds} cycles execution", r30_completed, f"成功执行 {len(self.curve_records)} 轮")

        # 内存稳定性分析（比较第 2 轮与第 30 轮，排除第 1 轮控制器挂起/启动的单次开销）
        if len(self.curve_records) >= 2:
            r2_heap = self.curve_records[1]["heap"]
            rN_heap = self.curve_records[-1]["heap"]
            r2_larg = self.curve_records[1]["largest"]
            rN_larg = self.curve_records[-1]["largest"]
            delta_heap = rN_heap - r2_heap
            delta_larg = rN_larg - r2_larg
            leak_ok = (delta_heap == 0 and delta_larg == 0)
            self.record_result(
                "Memory stability across cycles (Round 2 vs Round 30 delta)",
                leak_ok,
                f"Δheap={delta_heap:+d}, Δlargest={delta_larg:+d} (R2={r2_heap}/{r2_larg}, R30={rN_heap}/{rN_larg})",
            )

        # 广播包格式校验判定
        data_format_ok = (len(data_validation_errors) == 0 and data_rows_validated > 0)
        err_msg = f"{len(data_validation_errors)} errors" if data_validation_errors else f"已校验 {data_rows_validated} 个真实广播包，格式 100% 合规"
        self.record_result("BLE broadcast JSON data format compliance", data_format_ok, err_msg)

    # -------------------------------------------------------------------------
    # Phase 3: 射频互斥与冲突回执验证
    # -------------------------------------------------------------------------
    def test_mutual_exclusion(self):
        print("\n=======================================================")
        print("Phase 3: 射频互斥与冲突回复验证 (RID ON / SCAN WIFI)")
        print("=======================================================")
        # 3.1 开启 BLE ON，检查 RID ON 与 SCAN WIFI 冲突响应
        self.send_cmd("BLE ON")
        self.read_until(lambda l: l.startswith('BLE {"t":"start"}'), 2.0)
        time.sleep(0.2)
        self.drain_lines(0.2)

        # 在 BLE 流开着时发 RID ON
        self.send_cmd("RID ON")
        rid_resp, _ = self.read_until(lambda l: l.startswith("RID "), 2.0)
        rid_conflict_ok = (rid_resp is not None and '"busy: BLE stream on"' in rid_resp)
        self.record_result("BLE ON active -> RID ON conflict response", rid_conflict_ok, f"got: {rid_resp}")

        # 在 BLE 流开着时发 SCAN WIFI
        self.send_cmd("SCAN WIFI")
        wscan_resp, wscan_lines = self.read_until(lambda l: l.startswith("WSCAN "), 2.0)
        no_pcm_ack = not any('PCM {"t":"ack"' in l for l in wscan_lines)
        wscan_conflict_ok = (wscan_resp is not None and '"busy: BLE stream on"' in wscan_resp and no_pcm_ack)
        self.record_result("BLE ON active -> SCAN WIFI conflict response (busy, no PCM ack)", wscan_conflict_ok, f"got: {wscan_resp}")

        # 关闭 BLE 流
        self.send_cmd("BLE OFF")
        self.read_until(lambda l: l.startswith("BLE ") and '"t":"end"' in l, 2.0)
        time.sleep(0.3)

        # 3.2 开启 RID ON，检查 BLE ON 冲突响应
        self.send_cmd("RID ON")
        self.read_until(lambda l: l.startswith('RID {"t":"start"'), 2.0)
        time.sleep(0.2)
        self.drain_lines(0.2)

        self.send_cmd("BLE ON")
        ble_conflict_resp, _ = self.read_until(lambda l: l.startswith("BLE "), 2.0)
        ble_conflict_ok = (ble_conflict_resp is not None and '"busy: RID stream on"' in ble_conflict_resp)
        self.record_result("RID ON active -> BLE ON conflict response", ble_conflict_ok, f"got: {ble_conflict_resp}")

        # 关闭 RID 流
        self.send_cmd("RID OFF")
        self.read_until(lambda l: l.startswith('RID {"t":"end"'), 2.0)
        time.sleep(0.3)
        self.drain_lines(0.3)

    # -------------------------------------------------------------------------
    # Phase 4: 幂等性与 PCEXIT 自动退流验证
    # -------------------------------------------------------------------------
    def test_idempotency_and_pcexit(self):
        print("\n=======================================================")
        print("Phase 4: BLE OFF 幂等性与 PCEXIT 自动关流验证")
        print("=======================================================")
        # 4.1 在 BLE 未开启状态下连续发两次 BLE OFF
        self.send_cmd("BLE OFF")
        end1, _ = self.read_until(lambda l: l.startswith("BLE ") and '"t":"end"' in l, 2.0)
        self.send_cmd("BLE OFF")
        end2, _ = self.read_until(lambda l: l.startswith("BLE ") and '"t":"end"' in l, 2.0)
        idempotent_ok = (end1 is not None and end2 is not None)
        self.record_result("BLE OFF idempotency (consecutive OFF returns end)", idempotent_ok, f"end1={end1}, end2={end2}")

        # 4.2 开启 BLE ON，然后直接发送 PCEXIT
        self.send_cmd("BLE ON")
        self.read_until(lambda l: l.startswith('BLE {"t":"start"}'), 2.0)
        time.sleep(0.2)
        self.send_cmd("PCEXIT")
        exit_line, pcexit_lines = self.read_until(lambda l: l == 'PCM {"t":"exit"}', 3.0)
        ble_ended_on_exit = any(l.startswith("BLE ") and '"t":"end"' in l for l in pcexit_lines)
        canvas_restored = any("[canvas] restore OK" in l for l in pcexit_lines)
        pcexit_ok = (exit_line is not None and ble_ended_on_exit and canvas_restored)
        self.record_result("PCEXIT auto-closes BLE stream and restores canvas", pcexit_ok, f"exit={exit_line}, ble_ended={ble_ended_on_exit}")

        # 确认当前已回到普通模式
        time.sleep(0.3)
        st = self.get_stat()
        normal_screen_ok = (st["screen"] != 75)  # 75 为 SCREEN_PCMODE
        self.record_result("Returned to normal UI screen after PCEXIT", normal_screen_ok, f"screen={st['screen']}")

    # -------------------------------------------------------------------------
    # Phase 5: BTEXT 5 连跑两次重初始化验证
    # -------------------------------------------------------------------------
    def test_btext_reinit(self):
        print("\n=======================================================")
        print("Phase 5: BTEXT 5 连跑两次重新初始化验证 (普通模式)")
        print("=======================================================")
        # 5.1 第一次 BTEXT 5
        print("[*] 正在执行第 1 次 BTEXT 5 (约 5.5 秒)...")
        self.send_cmd("BTEXT 5")
        done1, lines1 = self.read_until(lambda l: "[btext] done." in l, 8.0)
        btext1_ok = False
        reports1 = 0
        if done1:
            m = re.search(r"started=(\d+)\s+reports=(\d+)", done1)
            if m:
                started1 = int(m.group(1))
                reports1 = int(m.group(2))
                btext1_ok = (started1 == 1)
        self.record_result("BTEXT 5 (1st run) started=1", btext1_ok, f"{done1}")

        time.sleep(0.5)

        # 5.2 第二次 BTEXT 5
        print("[*] 正在执行第 2 次 BTEXT 5 (约 5.5 秒)...")
        self.send_cmd("BTEXT 5")
        done2, lines2 = self.read_until(lambda l: "[btext] done." in l, 8.0)
        btext2_ok = False
        reports2 = 0
        if done2:
            m = re.search(r"started=(\d+)\s+reports=(\d+)", done2)
            if m:
                started2 = int(m.group(1))
                reports2 = int(m.group(2))
                btext2_ok = (started2 == 1)
        self.record_result("BTEXT 5 (2nd run) started=1 (re-init clean)", btext2_ok, f"{done2}")

    # -------------------------------------------------------------------------
    # 总结报告
    # -------------------------------------------------------------------------
    def print_summary(self):
        print("\n=======================================================")
        print("                 测试结果汇总                          ")
        print("=======================================================")
        print(f"总项数: {self.passed + self.failed} | 通过: {self.passed} | 失败: {self.failed}")
        print("-------------------------------------------------------")
        for rec in self.test_records:
            tag = "[PASS]" if rec["passed"] else "[FAIL]"
            print(f"{tag:<7} {rec['name']}")
            if rec["detail"]:
                print(f"        {rec['detail']}")
        print("=======================================================")

        if self.curve_records:
            print("\n内存曲线 (30 轮往返采样):")
            print("Round |   Heap   |  Largest |  MinEver")
            print("------+----------+----------+---------")
            for c in self.curve_records:
                print(f" {c['round']:>4} |  {c['heap']:>7} |  {c['largest']:>7} |  {c['minEver']:>7}")
            print("--------------------------------------")


def main():
    parser = argparse.ArgumentParser(description="BLE & PC MODE 自动化验证工具")
    parser.add_argument("-p", "--port", default="/dev/ttyACM0", help="串口设备 (默认: /dev/ttyACM0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="波特率 (默认: 115200)")
    parser.add_argument("-r", "--rounds", type=int, default=30, help="BLE ON/OFF 循环轮数 (默认: 30)")
    parser.add_argument("-v", "--verbose", action="store_true", help="打印串口原始收发")
    args = parser.parse_args()

    # 严格防护：禁止访问 /dev/ttyUSB0
    if "ttyUSB0" in args.port:
        sys.exit("CRITICAL ERROR: 禁止访问 /dev/ttyUSB0！本脚本只允许在 Cardputer (/dev/ttyACM0) 运行！")

    tester = BlePcmTester(args.port, args.baud, verbose=args.verbose)
    try:
        tester.open()
        tester.test_handshake_and_caps()
        tester.test_ble_roundtrips(rounds=args.rounds)
        tester.test_mutual_exclusion()
        tester.test_idempotency_and_pcexit()
        tester.test_btext_reinit()
        tester.print_summary()
        return 0 if tester.failed == 0 else 1
    except KeyboardInterrupt:
        print("\n用户中断测试")
        return 130
    except Exception as e:
        print(f"\n[!] 测试异常中断: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        tester.close()


if __name__ == "__main__":
    sys.exit(main())

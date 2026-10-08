#include "../../src/rid_opid.h"
#include <cassert>
#include <cstdio>

int main() {
  // 观察不够久：不报没播
  assert(opidCheck(false, "", "", false, 5000, 100) == OPID_PENDING);
  assert(opidCheck(false, "", "", false, 20000, 3) == OPID_PENDING);
  assert(opidCheck(false, "", "", false, OPID_WINDOW_MS - 1, 50) == OPID_PENDING);
  // 够久且包够多：没播
  assert(opidCheck(false, "", "", false, OPID_WINDOW_MS, OPID_MIN_PKTS) == OPID_NONE);
  // 正常内容（两种样例来自 tools/odidtest：ASTM 运营人编号、国标实名登记号）
  assert(opidCheck(true, "OP-CN-0099", "1581F5FMD230700ABCDE", true, 0, 1) == OPID_OK);
  assert(opidCheck(true, "07387413", "1581FA6QC25AH00C2EVW", true, 0, 1) == OPID_OK);
  // 有内容时不看观察窗口
  assert(opidCheck(true, "07387413", "", false, 0, 0) == OPID_OK);
  // 占位符：全空格、全相同、抄序列号
  assert(opidCheck(true, "        ", "X", true, 0, 1) == OPID_SUSPECT);
  assert(opidCheck(true, "00000000", "X", true, 0, 1) == OPID_SUSPECT);
  assert(opidCheck(true, "AAAAAAAAAAAAAAAAAAAA", "X", true, 0, 1) == OPID_SUSPECT);
  assert(opidCheck(true, "1581FA6QC25AH00C2EVW", "1581FA6QC25AH00C2EVW", true, 0, 1) == OPID_SUSPECT);
  // 首尾空格被忽略；两位重复不算占位符（太短，易误报）
  assert(opidCheck(true, "  AB-12345  ", "Z", true, 0, 1) == OPID_OK);
  assert(opidCheck(true, "11", "Z", true, 0, 1) == OPID_OK);
  // 没有序列号时不会误判成抄序列号
  assert(opidCheck(true, "ABC123", "ABC123", false, 0, 1) == OPID_OK);
  assert(!strcmp(opidName(OPID_NONE), "none") && !strcmp(opidName(OPID_PENDING), "pending"));
  std::puts("PASS: pending window, none, placeholder detection, trim, no-uasid");
}

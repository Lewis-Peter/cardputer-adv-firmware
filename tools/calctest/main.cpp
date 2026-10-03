// calctest/main.cpp — 计算器 + 单位换算主机端测试台
//
// 测试策略：
//   - 直接编译 src/calc.cpp 和 src/conv.cpp（真代码，不抄逻辑）。
//   - 求值引擎（parseExpr 等）是 file-static 的，通过暴露在 calc.cpp
//     里的公共接口 calcKey / calcExpr / calcResult / calcError 驱动。
//   - conv.cpp 通过 convCat / convFrom / convTo / convInput / convResult
//     以及 convKey / convEnter 驱动。
//   - 绘制函数（drawCalc / drawConv）也被编译进来；stubs/M5Unified.h 把
//     所有画布调用变成 no-op，让它们不崩溃即视为通过。
//
// 格式与风格参照 tools/clocktest/main.cpp：
//   [PASS] / [FAIL] + 计数，main 返回 fail 数的正负值供 CI 检测。

#include "Arduino.h"
#include "M5Unified.h"
#include "calc.h"
#include "conv.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cassert>

// ---- 全局替身 ----
uint32_t g_fakeNowMs = 0;
SimM5    M5;
M5Canvas cv;
Preferences prefs;

// globals.h 声明、但 calc/conv 不直接用，链接时需要提供定义
bool     dirty      = false;
bool     screenOff  = false;
int      SW = 240, SH = 135;
uint16_t ACCENT = 0x07E0, CARD_BG = 0x18C3, DIM_BORDER = 0x2965, ICON_DIM = 0x4A69;

// globals.h 里声明的辅助函数——calc/conv 的 draw* 函数会调，都不影响测试逻辑，置空即可
bool loadBool(const char*, const char*, bool def) { return def; }
void saveBool(const char*, const char*, bool) {}
uint32_t loadUInt(const char*, const char*, uint32_t def) { return def; }
void saveUInt(const char*, const char*, uint32_t) {}
int  loadInt(const char*, const char*, int def) { return def; }
void saveInt(const char*, const char*, int) {}
double loadDouble(const char*, const char*, double def) { return def; }
void saveDouble(const char*, const char*, double) {}
String loadString(const char*, const char*, const String& def) { return def; }
void saveString(const char*, const char*, const String&) {}
uint8_t loadUChar(const char*, const char*, uint8_t def) { return def; }
void saveUChar(const char*, const char*, uint8_t) {}

// ui_common.h 里被 draw* 引用的外部函数
void drawPageHeader(const char*, const char*, uint16_t) {}
String trunc(const String& s, int) { return s; }
void drawPageDots() {}
void drawPageDots(int, int) {}

// ---- 测试框架 ----
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name, const char* detail = "") {
  if (cond) {
    g_pass++;
    printf("  [PASS] %s\n", name);
  } else {
    g_fail++;
    printf("  [FAIL] %s %s\n", name, detail);
  }
}

// ---- 辅助：驱动 calc 求值，返回 calcResult 的 c_str() ----
// 用 calcKey 模拟逐字符输入然后按 Enter，跟真实按键路径完全一致。
static const char* calcEvalStr(const char* expr) {
  // 先 C 清空状态
  calcKey('C');
  for (const char* p = expr; *p; ++p) calcKey(*p);
  calcKey('=');
  return calcResult.c_str();
}

// 只关心求值是否出错
static bool calcEvalOk(const char* expr) {
  calcEvalStr(expr);
  return !calcError;
}

// 求值后返回数值（调用方自行判断 calcError）
static double calcEvalNum(const char* expr) {
  calcEvalStr(expr);
  return atof(calcResult.c_str());
}

// =================================================================
// 1. 基础四则运算
// =================================================================
void testBasicArithmetic() {
  printf("--- 1. 基础四则运算 ---\n");

  // 加法
  check(strcmp(calcEvalStr("1+2"), "3") == 0,          "1+2 = 3");
  check(strcmp(calcEvalStr("100+200"), "300") == 0,    "100+200 = 300");
  check(strcmp(calcEvalStr("0+0"), "0") == 0,          "0+0 = 0");

  // 减法
  check(strcmp(calcEvalStr("5-3"), "2") == 0,          "5-3 = 2");
  check(strcmp(calcEvalStr("0-1"), "-1") == 0,         "0-1 = -1");

  // 乘法
  check(strcmp(calcEvalStr("3*4"), "12") == 0,         "3*4 = 12");
  check(strcmp(calcEvalStr("0*99"), "0") == 0,         "0*99 = 0");

  // 除法
  check(strcmp(calcEvalStr("10/4"), "2.5") == 0,       "10/4 = 2.5");
  check(strcmp(calcEvalStr("9/3"), "3") == 0,          "9/3 = 3");

  // 取模
  check(strcmp(calcEvalStr("10%3"), "1") == 0,         "10%3 = 1");
  check(strcmp(calcEvalStr("7%7"), "0") == 0,          "7%7 = 0");
}

// =================================================================
// 2. 运算符优先级与结合性
// =================================================================
void testPrecedenceAndAssociativity() {
  printf("--- 2. 运算符优先级与结合性 ---\n");

  // * 优先于 +
  check(strcmp(calcEvalStr("2+3*4"), "14") == 0,       "2+3*4 = 14（乘先加后）");

  // 从左向右：减法左结合
  check(strcmp(calcEvalStr("10-3-2"), "5") == 0,       "10-3-2 = 5（左结合）");

  // 括号覆盖优先级
  check(strcmp(calcEvalStr("(2+3)*4"), "20") == 0,     "(2+3)*4 = 20");

  // 幂右结合：2^3^2 = 2^(3^2) = 2^9 = 512
  check(strcmp(calcEvalStr("2^3^2"), "512") == 0,      "2^3^2 = 512（幂右结合）");

  // 混合：-2^2 = -(2^2) = -4（幂优先于一元负）
  // 注：calc.cpp 注释明确说明 "unary = ('+'|'-')* power ; power = postfix '^' unary（右结合）"
  // 即 parseUnary 先调 parsePower，所以 -2^2 先计算 power=4，再取负 = -4
  {
    double v = calcEvalNum("-2^2");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - (-4.0)) < 1e-9, "-2^2 = -4（幂优先一元负）", det);
  }

  // 2^-1 = 0.5（幂的右侧接受一元负）
  {
    double v = calcEvalNum("2^-1");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - 0.5) < 1e-9, "2^-1 = 0.5", det);
  }
}

// =================================================================
// 3. 一元符号
// =================================================================
void testUnary() {
  printf("--- 3. 一元符号 ---\n");

  check(strcmp(calcEvalStr("-5"), "-5") == 0,          "-5 整数");
  check(strcmp(calcEvalStr("+5"), "5") == 0,           "+5 整数");
  // 双重否定
  {
    double v = calcEvalNum("--5");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - 5.0) < 1e-9, "--5 = 5（双重否定）", det);
  }
  // 三重否定
  {
    double v = calcEvalNum("---5");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - (-5.0)) < 1e-9, "---5 = -5（三重否定）", det);
  }
}

// =================================================================
// 4. 小数与特殊格式
// =================================================================
void testDecimals() {
  printf("--- 4. 小数输入与格式化 ---\n");

  // .5 = 0.5（strtod 支持无前导零）
  {
    double v = calcEvalNum(".5");
    char det[32]; snprintf(det, sizeof(det), "got %.8g", v);
    check(!calcError && fabs(v - 0.5) < 1e-9, ".5 解析为 0.5", det);
  }

  // 1.5 + 1.5 = 3
  check(strcmp(calcEvalStr("1.5+1.5"), "3") == 0,      "1.5+1.5 = 3");

  // 结果格式化：整数不带小数点
  check(strcmp(calcEvalStr("1.0"), "1") == 0,          "1.0 格式化为整数 '1'");
  check(strcmp(calcEvalStr("100.0"), "100") == 0,      "100.0 格式化为整数 '100'");

  // 8 位有效数字
  {
    const char* r = calcEvalStr("1/3");
    // %.8g 应该输出 0.33333333
    check(strlen(r) > 0 && !calcError, "1/3 有结果", r);
    double v = atof(r);
    check(fabs(v - 1.0/3.0) < 1e-7, "1/3 精度在 1e-7 内", r);
  }
}

// =================================================================
// 5. 错误处理
// =================================================================
void testErrors() {
  printf("--- 5. 错误处理 ---\n");

  // 5.1 除以零（整数 0）
  calcEvalStr("1/0");
  check(calcError, "1/0 报错");

  // 5.2 0/0
  calcEvalStr("0/0");
  check(calcError, "0/0 报错");

  // 5.3 取模除以零
  calcEvalStr("5%0");
  check(calcError, "5%0 报错");

  // 5.4 括号不匹配：多一个 (
  calcEvalStr("((1)");
  check(calcError, "((1) 括号不匹配报错");

  // 5.5 括号不匹配：多一个 )
  // "1)" —— 解析器读完 1，再遇到多余的 ) 时 *gp != '\0' 触发 error
  calcEvalStr("1)");
  check(calcError, "1) 多余右括号报错");

  // 5.6 非法字符
  // 注：calcKey 只接受 "0123456789.+-*/%^!()" 里的字符，'@' 会被忽略
  // 因此测试 "1@2" 实际发给 calcKey 时 '@' 被过滤，表达式变 "12"
  // 这是按键驱动层的过滤行为，不是求值层报错。
  // 改用 1..2（连续两个小数点）来测试非法输入：
  calcEvalStr("1..2");
  // strtod("1..2") 读到 "1."，解析完后 gp 停在第二个 "." 处，
  // parseTerm 继续读不出新运算符，parseExpr 返回。
  // 此时 *gp == '.' 不是 '\0'，触发 calcError。
  check(calcError, "1..2 连续小数点报错");

  // 5.7 空表达式（按 Enter 不输入任何内容）
  // calcKey('C') 后直接 calcKey('=')：calcEval 里 calcExpr.length()==0 直接返回，
  // calcResult 保持 "" 且 calcError 保持 false，calcJustEvaled 不被置位。
  calcKey('C');
  calcKey('=');
  // 空表达式时 calcResult 为 "" 且没有 Error
  check(calcResult == "" && !calcError, "空表达式不报错不产生结果");

  // 5.8 超长输入（> 48 字符不崩溃）
  calcKey('C');
  for (int i = 0; i < 60; i++) calcKey('1');
  calcKey('=');
  // 只要不崩溃即可；结果应该是个合法的大整数
  check(true, "超长输入（>48字符）不崩溃");
  check(!calcError, "超长输入截断后仍可求值");
}

// =================================================================
// 6. 阶乘
// =================================================================
void testFactorial() {
  printf("--- 6. 阶乘 ---\n");

  check(strcmp(calcEvalStr("0!"), "1") == 0,           "0! = 1");
  check(strcmp(calcEvalStr("1!"), "1") == 0,           "1! = 1");
  check(strcmp(calcEvalStr("5!"), "120") == 0,         "5! = 120");
  check(strcmp(calcEvalStr("10!"), "3628800") == 0,    "10! = 3628800");

  // 170! 是 double 范围内最大的（约 7.16e306）
  {
    const char* r = calcEvalStr("170!");
    check(!calcError && strlen(r) > 0, "170! 不报错有结果", r);
  }

  // 171! 超出 double 范围 → gerr = true → Error
  calcEvalStr("171!");
  check(calcError, "171! 超范围报错");

  // 负数阶乘 → 报错
  calcEvalStr("-1!");
  // -1! 按优先级 = -(1!) = -1，不是 (-1)!；
  // 所以 parsePower 先读 1，再读 !，得 1! = 1，再一元取负得 -1，不报错
  {
    double v = calcEvalNum("-1!");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - (-1.0)) < 1e-9, "-1! = -(1!) = -1", det);
  }

  // 小数阶乘 → 报错
  calcEvalStr("1.5!");
  check(calcError, "1.5! 非整数阶乘报错");
}

// =================================================================
// 7. 幂运算边界
// =================================================================
void testPower() {
  printf("--- 7. 幂运算边界 ---\n");

  check(strcmp(calcEvalStr("2^10"), "1024") == 0,      "2^10 = 1024");
  check(strcmp(calcEvalStr("2^0"),  "1") == 0,         "2^0 = 1");

  // 0^0 = 1（pow(0,0) 按 IEEE 返回 1）
  {
    double v = calcEvalNum("0^0");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - 1.0) < 1e-9, "0^0 = 1（IEEE pow行为）", det);
  }

  // 负数的幂
  {
    double v = calcEvalNum("(-2)^3");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - (-8.0)) < 1e-9, "(-2)^3 = -8", det);
  }
}

// =================================================================
// 8. 结果格式化
// =================================================================
void testFormatting() {
  printf("--- 8. 结果格式化 ---\n");

  // 整数不带小数点
  check(strcmp(calcEvalStr("7"), "7") == 0,             "纯整数 7 -> '7'");
  check(strcmp(calcEvalStr("-7"), "-7") == 0,           "负整数 -7 -> '-7'");

  // -0：IEEE 754 里 0*-1、-0、0/-5 的结果是负零，而 printf("%.0f", -0.0) 按 C 标准打出 "-0"
  // （glibc 和 ESP32 的 newlib 都这样）。计算器屏幕上出现 "-0" 是错的，结果必须显示 "0"。
  static const char* NEGZ[] = {"0*-1", "-0", "0/-5", "-0*3", "1-1*1"};
  for (const char* e : NEGZ) {
    const char* r = calcEvalStr(e);
    char name[48]; snprintf(name, sizeof(name), "%s 显示 '0' 不显示 '-0'", e);
    check(!calcError && strcmp(r, "0") == 0, name, r);
  }

  // 科学计数法：非常大的数
  // 1e15 > 1e14，走 %.8g 路径
  {
    const char* r = calcEvalStr("1000000000000000");  // 1e15
    check(!calcError, "1e15 不报错");
    // %.8g 对 1e15 输出 "1e+15" 或 "1e15"
    check(strstr(r, "e") != nullptr || strcmp(r, "1000000000000000") == 0,
          "1e15 格式化含科学计数或整数串", r);
  }

  // 非常小的数
  {
    double v = calcEvalNum("1/1000000000");
    char det[32]; snprintf(det, sizeof(det), "got %.8g", v);
    check(!calcError && fabs(v - 1e-9) < 1e-18, "1/1e9 精度", det);
  }

  // NaN / Inf → fmtNum 返回 "Error"（calc.cpp L107 显式检测）
  // sqrt(-1) 通过 pow？ 这里用 0^-1 = inf 或者 (-1)^0.5
  // 注：pow(-1, 0.5) 按 IEEE 返回 NaN
  {
    calcEvalStr("(-1)^0.5");
    // NaN → calcError = true 或 calcResult = "Error"
    // calc.cpp 里 isnan(v)||isinf(v) 会置 calcError = true
    check(calcError || strcmp(calcResult.c_str(), "Error") == 0,
          "(-1)^0.5 (NaN) 报错或 Error");
  }
}

// =================================================================
// 9. 连续运算符（边界模糊输入）
// =================================================================
void testConsecutiveOperators() {
  printf("--- 9. 连续运算符 ---\n");

  // "2++3" → 2 + (+3) = 5（一元 + 合法）
  {
    double v = calcEvalNum("2++3");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - 5.0) < 1e-9, "2++3 = 5（一元+）", det);
  }

  // "2+-3" → 2 + (-3) = -1
  {
    double v = calcEvalNum("2+-3");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - (-1.0)) < 1e-9, "2+-3 = -1", det);
  }

  // "2*+3" → 2 * (+3) = 6
  {
    double v = calcEvalNum("2*+3");
    char det[32]; snprintf(det, sizeof(det), "got %.6g", v);
    check(!calcError && fabs(v - 6.0) < 1e-9, "2*+3 = 6", det);
  }

  // "2**3" → 2 * *3：*3 不是合法一元，parsePrimary 期望数字却遇到 * 报错
  calcEvalStr("2**3");
  check(calcError, "2**3 连续乘号报错");
}

// =================================================================
// 10. 深层递归保护（gdepth 限制）
// =================================================================
void testDepthLimit() {
  printf("--- 10. 深层递归保护（gdepth 上限 24）---\n");

  // 25 个左括号：超出 GDEPTH_MAX=24，在 parseUnary 里拦截
  char deepExpr[128] = "";
  for (int i = 0; i < 25; i++) strcat(deepExpr, "(");
  strcat(deepExpr, "1");
  for (int i = 0; i < 25; i++) strcat(deepExpr, ")");

  // 因为 calc 输入上限 48 字符，25+1+25=51 会被 calcKey 截断；
  // 但 24 个括号 24+1+24=49 同样会被截断，达到 48 就停止追加。
  // 总之不应崩溃。
  calcKey('C');
  for (char c : std::string(deepExpr)) calcKey(c);
  calcKey('=');
  check(true, "深层嵌套括号不崩溃");
}

// =================================================================
// 11. calcKey 状态机（刚求值后的按键行为）
// =================================================================
void testCalcKeyStateMachine() {
  printf("--- 11. calcKey 状态机 ---\n");

  // 11.1 求值后按数字：重新开始
  calcKey('C');
  calcKey('5'); calcKey('=');
  // 此时 calcJustEvaled=true, calcResult="5"
  check(strcmp(calcResult.c_str(), "5") == 0, "5= 结果为 5");
  // 按数字键 3：应重置为新表达式 "3"
  calcKey('3');
  check(calcExpr == "3", "求值后按 3 重置表达式为 '3'",
        calcExpr.c_str());

  // 11.2 求值后按运算符：接着结果继续算
  calcKey('C');
  calcKey('4'); calcKey('=');
  calcKey('+'); calcKey('6'); calcKey('=');
  check(strcmp(calcResult.c_str(), "10") == 0, "4= 后接 +6= 结果为 10");

  // 11.3 退格清掉最后一个字符
  calcKey('C');
  calcKey('1'); calcKey('2'); calcKey('3');
  calcKey('\b');
  check(calcExpr == "12", "12 退格后变 '12'", calcExpr.c_str());

  // 11.4 Ans 键：上次结果代入
  calcKey('C');
  calcKey('7'); calcKey('=');       // calcLastAns = "7"
  calcKey('a');                      // 按 Ans，重置并把 "7" 放进 calcExpr
  check(calcExpr == "7", "Ans 代入上次结果 '7'", calcExpr.c_str());

  // 11.5 Clear 键
  calcKey('C');
  calcKey('9'); calcKey('8');
  calcKey('C');
  check(calcExpr == "" && calcResult == "" && !calcError,
        "C 键清空全部状态");
}

// =================================================================
// 12. conv.cpp — 单位换算往返测试
// =================================================================

// 驱动 convKey 输入数字字符串（纯数字+小数点+负号）
static void convInputNum(const char* s) {
  convKey('C');   // 先清空
  for (const char* p = s; *p; ++p) convKey(*p);
}

// 读回 convResult 字符串并转 double
static double convResultNum() {
  return atof(convResult.c_str());
}

// 往返误差容限：浮点运算误差
static const double CONV_REL_TOL = 1e-5;  // 相对容差 0.001%
static bool convRoundTrip(double orig, double converted) {
  if (fabs(orig) < 1e-12) return fabs(converted) < 1e-9;
  return fabs((converted - orig) / orig) < CONV_REL_TOL;
}

void testConvLength() {
  printf("--- 12.1 长度换算 ---\n");

  // 类别切到 Length（索引 0）
  convCat = 0; convFrom = 0; convTo = 0;

  // 精确参考值：1 inch = 2.54 cm = 0.0254 m
  // FROM=in(索引4), TO=cm(索引1)
  convFrom = 4; convTo = 1;  // in -> cm
  convInputNum("1");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g cm", r);
    check(fabs(r - 2.54) < 1e-9, "1 inch = 2.54 cm（精确值）", det);
  }

  // 往返：1 m -> km -> m
  convFrom = 2; convTo = 3;  // m -> km
  convInputNum("1000");
  double km = convResultNum();
  {
    char det[64]; snprintf(det, sizeof(det), "got %.8g km", km);
    check(fabs(km - 1.0) < 1e-9, "1000 m = 1 km", det);
  }
  convFrom = 3; convTo = 2;  // km -> m
  convInputNum("1");
  {
    double m = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g m", m);
    check(fabs(m - 1000.0) < 1e-6, "1 km 回到 1000 m（往返）", det);
  }

  // 精确值：1 mile = 1609.344 m
  // 注意：recalc() 用 %.6g（6位有效数字），1609.344 被格式化为 "1609.34"；
  // 这是显示精度限制，不是换算系数有误。误差容限按 6 位有效数字放宽到 0.1 m。
  convFrom = 6; convTo = 2;  // mi -> m
  convInputNum("1");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g m", r);
    check(fabs(r - 1609.344) < 0.1, "1 mile ≈ 1609.34 m（%.6g精度内）", det);
  }

  // 1 nautical mile = 1852 m
  convFrom = 7; convTo = 2;  // nmi -> m
  convInputNum("1");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g m", r);
    check(fabs(r - 1852.0) < 1e-6, "1 nmi = 1852 m（精确值）", det);
  }
}

void testConvMass() {
  printf("--- 12.2 质量换算 ---\n");

  convCat = 1;

  // 1 kg = 1000 g（精确）
  convFrom = 2; convTo = 1;  // kg -> g
  convInputNum("1");
  {
    double r = convResultNum();
    check(fabs(r - 1000.0) < 1e-9, "1 kg = 1000 g", nullptr);
  }

  // 往返：1 oz -> g -> oz
  convFrom = 4; convTo = 1;  // oz -> g
  convInputNum("1");
  double g = convResultNum();
  {
    char det[64]; snprintf(det, sizeof(det), "got %.8g g", g);
    check(fabs(g - 28.3495) < 1e-4, "1 oz = 28.3495 g（参考值）", det);
  }
  // 回到 oz
  convFrom = 1; convTo = 4;  // g -> oz
  char gbuf[32]; snprintf(gbuf, sizeof(gbuf), "%.6g", g);
  convInputNum(gbuf);
  {
    double oz = convResultNum();
    check(convRoundTrip(1.0, oz), "g 再转回 oz 往返误差 < 0.001%");
  }

  // 1 lb = 453.592 g
  convFrom = 5; convTo = 1;  // lb -> g
  convInputNum("1");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g g", r);
    check(fabs(r - 453.592) < 1e-3, "1 lb = 453.592 g（参考值）", det);
  }
}

void testConvTemperature() {
  printf("--- 12.3 温度换算 ---\n");

  convCat = 2;

  // 参考值：-40°C = -40°F（两条温标的唯一交叉点）
  convFrom = 0; convTo = 1;  // C -> F
  convInputNum("-40");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g F", r);
    check(fabs(r - (-40.0)) < 1e-9, "-40 C = -40 F（交叉点精确值）", det);
  }

  // 0°C = 32°F
  convFrom = 0; convTo = 1;
  convInputNum("0");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g F", r);
    check(fabs(r - 32.0) < 1e-9, "0 C = 32 F", det);
  }

  // 100°C = 212°F
  convFrom = 0; convTo = 1;
  convInputNum("100");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g F", r);
    check(fabs(r - 212.0) < 1e-9, "100 C = 212 F", det);
  }

  // 0°C = 273.15 K
  convFrom = 0; convTo = 2;  // C -> K
  convInputNum("0");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g K", r);
    check(fabs(r - 273.15) < 1e-9, "0 C = 273.15 K", det);
  }

  // 往返 F -> C -> F
  convFrom = 1; convTo = 0;  // F -> C
  convInputNum("72");
  double c = convResultNum();
  convFrom = 0; convTo = 1;  // C -> F
  char cbuf[32]; snprintf(cbuf, sizeof(cbuf), "%.6g", c);
  convInputNum(cbuf);
  {
    double f = convResultNum();
    check(convRoundTrip(72.0, f), "72 F -> C -> F 往返", nullptr);
  }

  // K -> C -> K 往返
  convFrom = 2; convTo = 0;  // K -> C
  convInputNum("300");
  double cval = convResultNum();
  {
    char det[64]; snprintf(det, sizeof(det), "got %.8g C", cval);
    check(fabs(cval - (300.0 - 273.15)) < 1e-9, "300 K = 26.85 C", det);
  }
  convFrom = 0; convTo = 2;
  char cbuf2[32]; snprintf(cbuf2, sizeof(cbuf2), "%.6g", cval);
  convInputNum(cbuf2);
  {
    double k = convResultNum();
    check(convRoundTrip(300.0, k), "26.85 C -> K 往返", nullptr);
  }
}

void testConvVolume() {
  printf("--- 12.4 体积换算 ---\n");

  convCat = 3;

  // 1 L = 1000 mL（精确）
  convFrom = 1; convTo = 0;  // L -> mL
  convInputNum("1");
  {
    double r = convResultNum();
    check(fabs(r - 1000.0) < 1e-9, "1 L = 1000 mL", nullptr);
  }

  // 1 m3 = 1e6 mL（精确）
  convFrom = 2; convTo = 0;  // m3 -> mL
  convInputNum("1");
  {
    double r = convResultNum();
    check(fabs(r - 1e6) < 1.0, "1 m3 = 1e6 mL", nullptr);
  }

  // 往返：1 gal -> mL -> gal
  convFrom = 6; convTo = 0;  // gal -> mL
  convInputNum("1");
  double ml = convResultNum();
  {
    char det[64]; snprintf(det, sizeof(det), "got %.8g mL", ml);
    check(fabs(ml - 3785.41) < 0.01, "1 gal = 3785.41 mL（参考值）", det);
  }
  convFrom = 0; convTo = 6;
  char mlbuf[32]; snprintf(mlbuf, sizeof(mlbuf), "%.6g", ml);
  convInputNum(mlbuf);
  {
    double gal = convResultNum();
    check(convRoundTrip(1.0, gal), "mL 再转 gal 往返", nullptr);
  }
}

void testConvSpeed() {
  printf("--- 12.5 速度换算 ---\n");

  convCat = 4;

  // 1 m/s = 3.6 km/h（精确）
  convFrom = 0; convTo = 1;  // m/s -> km/h
  convInputNum("1");
  {
    double r = convResultNum();
    char det[64]; snprintf(det, sizeof(det), "got %.8g km/h", r);
    check(fabs(r - 3.6) < 1e-9, "1 m/s = 3.6 km/h（精确值）", det);
  }

  // 往返：36 km/h -> m/s -> km/h
  convFrom = 1; convTo = 0;  // km/h -> m/s
  convInputNum("36");
  double ms = convResultNum();
  {
    char det[64]; snprintf(det, sizeof(det), "got %.8g m/s", ms);
    check(fabs(ms - 10.0) < 1e-9, "36 km/h = 10 m/s", det);
  }
  convFrom = 0; convTo = 1;
  char msbuf[32]; snprintf(msbuf, sizeof(msbuf), "%.6g", ms);
  convInputNum(msbuf);
  {
    double kmh = convResultNum();
    check(convRoundTrip(36.0, kmh), "m/s 再转 km/h 往返", nullptr);
  }
}

void testConvTime() {
  printf("--- 12.6 时间换算 ---\n");

  convCat = 5;

  // 1 h = 3600 s（精确）
  convFrom = 3; convTo = 1;  // h -> s
  convInputNum("1");
  {
    double r = convResultNum();
    check(fabs(r - 3600.0) < 1e-9, "1 h = 3600 s（精确）", nullptr);
  }

  // 1 week = 7 days（精确）
  convFrom = 5; convTo = 4;  // wk -> day
  convInputNum("1");
  {
    double r = convResultNum();
    check(fabs(r - 7.0) < 1e-9, "1 wk = 7 day（精确）", nullptr);
  }

  // 往返：1000 ms -> s -> ms
  convFrom = 0; convTo = 1;  // ms -> s
  convInputNum("1000");
  double s = convResultNum();
  {
    char det[64]; snprintf(det, sizeof(det), "got %.8g s", s);
    check(fabs(s - 1.0) < 1e-9, "1000 ms = 1 s", det);
  }
  convFrom = 1; convTo = 0;
  convInputNum("1");
  {
    double ms2 = convResultNum();
    check(fabs(ms2 - 1000.0) < 1e-6, "1 s 回到 1000 ms（往返）", nullptr);
  }
}

void testConvArea() {
  printf("--- 12.7 面积换算 ---\n");

  convCat = 6;

  // 1 m2 = 10000 cm2（精确）
  convFrom = 2; convTo = 1;  // m2 -> cm2
  convInputNum("1");
  {
    double r = convResultNum();
    check(fabs(r - 10000.0) < 1e-6, "1 m2 = 10000 cm2", nullptr);
  }

  // 1 ha = 10000 m2（精确）
  convFrom = 7; convTo = 2;  // ha -> m2
  convInputNum("1");
  {
    double r = convResultNum();
    check(fabs(r - 10000.0) < 1e-6, "1 ha = 10000 m2", nullptr);
  }

  // 往返：1 acre -> m2 -> acre
  convFrom = 6; convTo = 2;  // acre -> m2
  convInputNum("1");
  double m2 = convResultNum();
  {
    char det[64]; snprintf(det, sizeof(det), "got %.8g m2", m2);
    check(fabs(m2 - 4046.86) < 0.01, "1 acre = 4046.86 m2（参考值）", det);
  }
  convFrom = 2; convTo = 6;
  char m2buf[32]; snprintf(m2buf, sizeof(m2buf), "%.6g", m2);
  convInputNum(m2buf);
  {
    double ac = convResultNum();
    check(convRoundTrip(1.0, ac), "m2 再转 acre 往返", nullptr);
  }
}

// =================================================================
// 13. convKey 状态机
// =================================================================
void testConvKeyStateMachine() {
  printf("--- 13. convKey 状态机 ---\n");

  convCat = 0; convFrom = 0; convTo = 1;

  // 13.1 小数点输入（第一个 '.' 起作用，第二个被忽略）
  convInputNum("3");
  convKey('.');
  convKey('1');
  convKey('4');
  {
    char det[64]; snprintf(det, sizeof(det), "input='%s'", convInput.c_str());
    check(convInput == "3.14", "3 . 1 4 → input '3.14'", det);
  }

  // 13.2 负号只能在输入框为空时添加
  convKey('C');
  convKey('-');
  check(convInput == "-", "空时按 - 得 '-'", convInput.c_str());

  // 13.3 退格
  convKey('C');
  convKey('1'); convKey('2'); convKey('3');
  convKey('\b');
  check(convInput == "12", "123 退格得 '12'", convInput.c_str());

  // 13.4 S（交换）键
  convCat = 0; convFrom = 2; convTo = 3;  // m / km
  convKey('S');
  check(convFrom == 3 && convTo == 2, "S 交换 FROM/TO", nullptr);

  // 13.5 切类别键
  convCat = 0;
  convKey('/');  // 下一类
  check(convCat == 1, "/ 切到类别 1（Mass）");

  convKey(',');  // 上一类
  check(convCat == 0, ", 切回类别 0（Length）");

  // 13.6 同源同目标时 convEnter 自动跳过（FROM != TO）
  convCat = 0; convFrom = 2; convTo = 2;
  convEnter();
  check(convFrom != convTo, "convEnter 处理 FROM==TO 时自动调整");

  // 13.7 负零归一：输入 -0、-0.0 等或换算结果为负零时不显示 '-0'
  convCat = 0; convFrom = 0; convTo = 1;
  convInputNum("-0");
  check(convResult == "0", "输入 -0 结果显示 '0' 不显示 '-0'");
  convInputNum("-0.0");
  check(convResult == "0", "输入 -0.0 结果显示 '0' 不显示 '-0'");
}

// =================================================================
// 14. 烟雾测试：drawCalc / drawConv 编译且不崩溃
// =================================================================
void testDrawNocrash() {
  printf("--- 14. draw 函数烟雾测试 ---\n");

  // 注意：drawCalc / drawConv 都依赖 globals 里的辅助，
  // 我们已在本文件顶部提供了空实现。只要不崩溃就是 PASS。
  calcKey('C');
  drawCalc();
  check(true, "drawCalc 不崩溃");

  convKey('C');
  drawConv();
  check(true, "drawConv 不崩溃");
}

// =================================================================
// main
// =================================================================
int main() {
  printf("==========================================\n");
  printf("  Cardputer ADV: Calc + Conv Host Tests\n");
  printf("==========================================\n");

  // ---- calc.cpp 测试 ----
  testBasicArithmetic();
  testPrecedenceAndAssociativity();
  testUnary();
  testDecimals();
  testErrors();
  testFactorial();
  testPower();
  testFormatting();
  testConsecutiveOperators();
  testDepthLimit();
  testCalcKeyStateMachine();

  // ---- conv.cpp 测试 ----
  testConvLength();
  testConvMass();
  testConvTemperature();
  testConvVolume();
  testConvSpeed();
  testConvTime();
  testConvArea();
  testConvKeyStateMachine();

  // ---- 烟雾 ----
  testDrawNocrash();

  printf("==========================================\n");
  printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
  printf("==========================================\n");

  return g_fail > 0 ? 1 : 0;
}

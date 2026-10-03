#include "calc.h"
#include "ui_common.h"
#include <cmath>
#include <cstring>
#include <cstdlib>

String calcExpr   = "";
String calcResult = "";
bool   calcError    = false;
bool   calcJustEvaled = false;

// ---- 递归下降求值：expr = term (('+'|'-') term)* ; term = unary (('*'|'/'|'%') unary)* ;
//      unary = ('+'|'-')* power ; power = postfix ('^' unary)? （右结合，-2^2 = -(2^2)）
//      postfix = primary '!'* ; primary = number | '(' expr ')' ----
static const char* gp;   // 当前解析位置
static bool gerr;        // 解析/求值中是否出错（如括号不匹配、除零）
static int  gdepth;      // 递归深度：防止 "((((((..." 这类深嵌套把 8KB 的 loop 栈递归爆掉
// 每层括号/幂/一元号都会经过 parseUnary，故在这里统一限深。真实算式几乎不会超过个位数层。
static const int GDEPTH_MAX = 24;

static void skipws() { while (*gp == ' ') gp++; }
static double parseExpr();
static double parseUnary();

// 阶乘只对 0..170 的非负整数定义（超出 double 精度/表示范围就报错）
static double factorial(double v) {
  if (v < 0 || v != floor(v) || v > 170) { gerr = true; return 0; }
  double r = 1;
  for (int i = 2; i <= (int)v; i++) r *= i;
  return r;
}

static double parsePrimary() {
  skipws();
  if (*gp == '(') {
    gp++;
    double v = parseExpr();
    skipws();
    if (*gp == ')') gp++; else gerr = true;
    return v;
  }
  char* end;
  double v = strtod(gp, &end);
  if (end == gp) { gerr = true; return 0; }   // 期望数字却没有
  gp = end;
  return v;
}

static double parsePostfix() {
  double v = parsePrimary();
  for (;;) {
    skipws();
    if (*gp != '!') break;
    gp++;
    v = factorial(v);
  }
  return v;
}

static double parsePower() {
  double v = parsePostfix();
  skipws();
  if (*gp == '^') { gp++; v = pow(v, parseUnary()); }
  return v;
}

static double parseUnary() {
  // 所有向下递归（'(' 括号、'^' 幂的右结合、连续一元符号）都会穿过这里，
  // 在入口统一限深即可挡住任意深度的递归爆栈。
  if (++gdepth > GDEPTH_MAX) { gerr = true; --gdepth; return 0; }
  skipws();
  double v;
  if      (*gp == '-') { gp++; v = -parseUnary(); }
  else if (*gp == '+') { gp++; v =  parseUnary(); }
  else                   v = parsePower();
  --gdepth;
  return v;
}

static double parseTerm() {
  double v = parseUnary();
  for (;;) {
    skipws();
    char c = *gp;
    if (c == '*') { gp++; v *= parseUnary(); }
    else if (c == '/') { gp++; double d = parseUnary(); if (d == 0) { gerr = true; return 0; } v /= d; }
    else if (c == '%') { gp++; double d = parseUnary(); if (d == 0) { gerr = true; return 0; } v = fmod(v, d); }
    else break;
  }
  return v;
}

static double parseExpr() {
  double v = parseTerm();
  for (;;) {
    skipws();
    char c = *gp;
    if (c == '+') { gp++; v += parseTerm(); }
    else if (c == '-') { gp++; v -= parseTerm(); }
    else break;
  }
  return v;
}

// 数字转字符串：整数不带小数点，否则最多 8 位有效数字、去掉多余尾零
static String fmtNum(double v) {
  if (isnan(v) || isinf(v)) return "Error";
  // 0*-1、0/-5 这类结果是 IEEE 负零，而 "%.0f" 会老实打出 "-0"。-0.0 == 0.0 为真，借此归一成正零。
  if (v == 0.0) v = 0.0;
  if (fabs(v) < 1e14 && v == floor(v)) {
    char b[32]; snprintf(b, sizeof(b), "%.0f", v); return String(b);
  }
  char b[32]; snprintf(b, sizeof(b), "%.8g", v); return String(b);
}

// 实时语法试算（输入中若语法合法直接返回当前值，否则返回 false）
static bool calcTryEval(double& out) {
  if (calcExpr.length() == 0) return false;
  char last = calcExpr[calcExpr.length() - 1];
  if (strchr("+-*/%^(", last)) return false;

  gp = calcExpr.c_str();
  gerr = false;
  gdepth = 0;
  double v = parseExpr();
  skipws();
  if (gerr || *gp != '\0' || isnan(v) || isinf(v)) return false;
  out = v;
  return true;
}

static void calcEval() {
  if (calcExpr.length() == 0) return;
  gp = calcExpr.c_str();
  gerr = false;
  gdepth = 0;
  double v = parseExpr();
  skipws();
  if (gerr || *gp != '\0' || isnan(v) || isinf(v)) {
    calcResult = "Error"; calcError = true;
  } else {
    calcResult = fmtNum(v); calcError = false;
  }
  calcJustEvaled = true;
}

static char lastKey = 0;
static uint32_t lastKeyMs = 0;
static String calcLastExpr = "";
static String calcLastAns = "";

void calcKey(char k) {
  lastKey = k;
  lastKeyMs = millis();

  if (k == '\n' || k == '=') {
    if (calcExpr.length()) {
      calcLastExpr = calcExpr;
      calcEval();
      if (!calcError) calcLastAns = calcResult;
    }
    dirty = true;
    return;
  }

  // Clear 键（C / c）清空全部
  if (k == 'c' || k == 'C') {
    calcExpr = "";
    calcResult = "";
    calcError = false;
    calcJustEvaled = false;
    dirty = true;
    return;
  }

  // 代入上次结果 Ans（A / a）
  if (k == 'a' || k == 'A') {
    if (calcLastAns.length() > 0) {
      if (calcJustEvaled) {
        calcExpr = calcLastAns;
        calcResult = "";
        calcError = false;
        calcJustEvaled = false;
      } else {
        if (calcExpr.length() + calcLastAns.length() < 48) calcExpr += calcLastAns;
      }
      dirty = true;
    }
    return;
  }

  if (k == '\b') { // 退格
    if (calcJustEvaled) {
      calcExpr = ""; calcResult = ""; calcError = false; calcJustEvaled = false;
    } else if (calcExpr.length()) {
      calcExpr.remove(calcExpr.length() - 1);
    } else {
      calcResult = ""; calcError = false;
    }
    dirty = true;
    return;
  }

  if (k && strchr("0123456789.+-*/%^!()", k)) {
    if (calcJustEvaled) {
      // 刚求过值：输运算符则接着上次结果算，输数字/括号则重新开始
      if (!calcError && strchr("+-*/%^!", k)) calcExpr = calcResult;
      else                                  calcExpr = "";
      calcResult = ""; calcError = false; calcJustEvaled = false;
    }
    if (calcExpr.length() < 48) calcExpr += k;
    dirty = true;
  }
}

// 右对齐显示，超长时截断保留尾部（跟计算器一样看最新输入的一端）
static String fitRight(const String& s, int maxCh) {
  if ((int)s.length() <= maxCh) return s;
  return "~" + s.substring(s.length() - (maxCh - 1));
}

void drawCalc() {
  cv.fillScreen(TFT_BLACK);
  const uint32_t now = millis();

  // 顶栏：标题 + 状态
  const char* st = calcError ? "ERR" : (calcJustEvaled ? "EVAL" : (calcExpr.length() ? "EDIT" : "READY"));
  const uint16_t stCol = calcError ? TFT_RED : (calcJustEvaled ? ACCENT : 0);
  drawPageHeader("Calculator", st, stCol);

  // ---- LCD 主显示屏 (y = 14..81, 高 68) ----
  const int px = 4, py = 14, pw = SW - 8, ph = 68;
  cv.fillRoundRect(px, py, pw, ph, 4, 0x0841); // 暗墨绿/OLED黑底色
  cv.drawRoundRect(px, py, pw, ph, 4, calcError ? TFT_RED : (calcJustEvaled ? ACCENT : 0x18C3));

  // 屏内顶栏：左侧模式/Ans 徽标，右侧历史算式
  cv.setTextDatum(top_left); cv.setTextSize(1);
  if (calcLastAns.length() > 0) {
    char ansBuf[24]; snprintf(ansBuf, sizeof(ansBuf), "Ans:%s", calcLastAns.c_str());
    cv.setTextColor(0x05E8, 0x0841);
    cv.drawString(trunc(String(ansBuf), 12), px + 6, py + 4);
  } else {
    cv.setTextColor(0x0320, 0x0841);
    cv.drawString("[MATH]", px + 6, py + 4);
  }

  // 历史/算式预览行 (右上角)
  if (calcJustEvaled && calcLastExpr.length()) {
    cv.setTextDatum(top_right); cv.setTextSize(1);
    cv.setTextColor(TFT_DARKGREY, 0x0841);
    cv.drawString(fitRight(calcLastExpr + " =", (pw - 80) / 6), px + pw - 6, py + 4);
  }

  // 算式输入行 (y = 26..42)
  String dispExpr = calcExpr;
  const bool blink = ((now / 450) % 2 == 0);
  if (!calcJustEvaled) {
    if (dispExpr.length() == 0) dispExpr = blink ? "_" : " ";
    else if (blink)             dispExpr += "_";
  }
  cv.setTextDatum(top_right); cv.setTextSize(2);
  cv.setTextColor(TFT_WHITE, 0x0841);
  cv.drawString(fitRight(dispExpr, (pw - 12) / 12), px + pw - 6, py + 16);

  // 屏内浅色分割线
  cv.drawFastHLine(px + 4, py + 38, pw - 8, 0x1082);

  // 结果行 (y = 42..66)
  cv.setTextDatum(bottom_right);
  if (calcError) {
    cv.setTextSize(3);
    cv.setTextColor(TFT_RED, 0x0841);
    cv.drawString("Error", px + pw - 6, py + ph - 3);
  } else if (calcResult.length() > 0) {
    const int tsize = (calcResult.length() > 10) ? 2 : 3;
    cv.setTextSize(tsize);
    cv.setTextColor(ACCENT, 0x0841);
    cv.drawString(fitRight(calcResult, (pw - 12) / (tsize * 6)), px + pw - 6, py + ph - 3);
  } else {
    // 实时计算预览
    double liveVal = 0;
    if (calcTryEval(liveVal)) {
      String s = "= " + fmtNum(liveVal);
      const int tsize = (s.length() > 10) ? 2 : 3;
      cv.setTextSize(tsize);
      cv.setTextColor(0x05E8, 0x0841); // 柔和绿预览
      cv.drawString(fitRight(s, (pw - 12) / (tsize * 6)), px + pw - 6, py + ph - 3);
    } else {
      cv.setTextSize(3);
      cv.setTextColor(0x18C3, 0x0841); // 占位
      cv.drawString("0", px + pw - 6, py + ph - 3);
    }
  }

  // ---- 极客风格运算符键盘矩阵 (y = 86..117, 高 32) ----
  // 8 列 x 2 行
  static const char* KEY_LABELS[2][8] = {
    { "+", "-", "*", "/", "^", "%", "(", ")" },
    { "7", "8", "9", ".", "!", "Ans", "C", "=" }
  };
  const int kw = 25, kh = 14, gapX = 4, gapY = 3;
  const int kStartX = 6;
  const bool keyActive = (now - lastKeyMs < 180);

  for (int r = 0; r < 2; r++) {
    const int ky = 86 + r * (kh + gapY);
    for (int c = 0; c < 8; c++) {
      const int kx = kStartX + c * (kw + gapX);
      const char* lbl = KEY_LABELS[r][c];

      // 检查当前按键是否点亮
      bool hit = false;
      if (keyActive) {
        if (strcmp(lbl, "Ans") == 0 && (lastKey == 'a' || lastKey == 'A')) hit = true;
        else if (strcmp(lbl, "C") == 0 && (lastKey == 'c' || lastKey == 'C')) hit = true;
        else if (strcmp(lbl, "=") == 0 && (lastKey == '=' || lastKey == '\n')) hit = true;
        else if (lbl[0] == lastKey && lbl[1] == 0) hit = true;
      }

      const uint16_t btnBg = hit ? ACCENT : 0x0841;
      const uint16_t btnBorder = hit ? TFT_WHITE : 0x18C3;
      cv.fillRoundRect(kx, ky, kw, kh, 3, btnBg);
      cv.drawRoundRect(kx, ky, kw, kh, 3, btnBorder);

      uint16_t txtCol = hit ? TFT_BLACK : TFT_LIGHTGREY;
      if (!hit) {
        if (strcmp(lbl, "=") == 0)        txtCol = ACCENT;
        else if (strcmp(lbl, "C") == 0)   txtCol = TFT_YELLOW;
        else if (strcmp(lbl, "Ans") == 0) txtCol = TFT_CYAN;
        else if (strchr("+-*/^%", lbl[0]))txtCol = TFT_CYAN;
      }
      cv.setTextDatum(middle_center); cv.setTextSize(1);
      cv.setTextColor(txtCol, btnBg);
      cv.drawString(lbl, kx + kw / 2, ky + kh / 2);
    }
  }

  // ---- 底部提示条 (y = 125..134) ----
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("Enter =   C clear   Del back   ` menu", 4, SH - 2);
}

#include "conv.h"
#include "ui_common.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>

// ---- 单位定义：factor = 到基础单位的换算系数；Temperature 用 index 特殊处理 ----
struct Unit { const char* name; double factor; };

static const Unit LEN[]  = {{"mm",1e-3},{"cm",1e-2},{"m",1},{"km",1e3},
                              {"in",0.0254},{"ft",0.3048},{"mi",1609.344},{"nmi",1852}};
static const Unit MASS[] = {{"mg",1e-3},{"g",1},{"kg",1e3},{"t",1e6},{"oz",28.3495},{"lb",453.592}};
static const Unit TEMP[] = {{"C",0},{"F",0},{"K",0}};   // factor=0 → 特殊换算
static const Unit VOL[]  = {{"mL",1},{"L",1e3},{"m3",1e6},
                              {"fl oz",29.5735},{"cup",236.588},{"pint",473.176},{"gal",3785.41}};
static const Unit SPD[]  = {{"m/s",1},{"km/h",1.0/3.6},{"mph",0.44704},{"knot",0.514444},{"ft/s",0.3048}};
static const Unit TIM[]  = {{"ms",1e-3},{"s",1},{"min",60},{"h",3600},{"day",86400},{"wk",604800}};
static const Unit AREA[] = {{"mm2",1e-6},{"cm2",1e-4},{"m2",1},{"km2",1e6},
                              {"in2",6.4516e-4},{"ft2",0.092903},{"acre",4046.86},{"ha",1e4}};

struct Cat { const char* name; const Unit* units; int count; };
static const Cat CATS[] = {
  {"Length", LEN, 8}, {"Mass", MASS, 6}, {"Temp", TEMP, 3},
  {"Volume", VOL, 7}, {"Speed", SPD, 5}, {"Time", TIM, 6}, {"Area", AREA, 8}
};
static const int CAT_COUNT = 7;

int    convCat   = 0;
int    convFrom  = 0;
int    convTo    = 1;
String convInput = "";
String convResult = "";

// 温度：先转到 °C，再从 °C 转到目标
static double toC(double v, int fromIdx) {
  if (fromIdx == 1) return (v - 32.0) * 5.0 / 9.0;
  if (fromIdx == 2) return v - 273.15;
  return v;
}
static double fromC(double c, int toIdx) {
  if (toIdx == 1) return c * 9.0 / 5.0 + 32.0;
  if (toIdx == 2) return c + 273.15;
  return c;
}

static void recalc() {
  if (convInput.length() == 0 || convInput == "-" || convInput == ".") {
    convResult = "";
    return;
  }
  double v = atof(convInput.c_str());
  double result;
  if (convCat == 2) {
    result = fromC(toC(v, convFrom), convTo);
  } else {
    const Cat& cat = CATS[convCat];
    result = v * cat.units[convFrom].factor / cat.units[convTo].factor;
  }
  // 输入 "-0" 或换算结果为 IEEE 负零时，"%.6g" 会打出 "-0"。-0.0 == 0.0 为真，借此归一成正零。
  if (result == 0.0) result = 0.0;
  char buf[28];
  double absR = fabs(result);
  if (absR == 0.0 || (absR >= 0.001 && absR < 1e7)) {
    snprintf(buf, sizeof(buf), "%.6g", result);
  } else {
    snprintf(buf, sizeof(buf), "%.4e", result);
  }
  convResult = String(buf);
}

void convEnter() {
  convInput  = "";
  convResult = "";
  if (convFrom == convTo) convTo = (convFrom + 1) % CATS[convCat].count;
}

void convKey(char k) {
  const Cat& cat = CATS[convCat];

  if (k == ',') {   // 上一类别
    convCat  = (convCat - 1 + CAT_COUNT) % CAT_COUNT;
    convFrom = 0; convTo = 1; convInput = ""; convResult = "";
    dirty = true; return;
  }
  if (k == '/') {   // 下一类别
    convCat  = (convCat + 1) % CAT_COUNT;
    convFrom = 0; convTo = 1; convInput = ""; convResult = "";
    dirty = true; return;
  }
  if (k == ';') {   // FROM 上一单位
    convFrom = (convFrom - 1 + cat.count) % cat.count;
    if (convFrom == convTo) convFrom = (convFrom - 1 + cat.count) % cat.count;
    recalc(); dirty = true; return;
  }
  if (k == '.') {   // FROM 下一单位（注意 '.' 还可能是小数点，下面分流）
    // 如果输入框非空且 '.' 用于小数点，交给数字输入处理
    // 这里的 '.' 在没有输入时才做"切单位"——有输入时做小数点
    if (convInput.length() == 0) {
      convFrom = (convFrom + 1) % cat.count;
      if (convFrom == convTo) convFrom = (convFrom + 1) % cat.count;
      recalc(); dirty = true; return;
    }
    // 否则：当作小数点处理
    if (convInput.indexOf('.') < 0 && convInput.length() < 16) {
      if (convInput.length() == 0 || convInput == "-") convInput += "0";
      convInput += '.';
      recalc(); dirty = true;
    }
    return;
  }
  if (k == '[') {   // TO 上一单位
    convTo = (convTo - 1 + cat.count) % cat.count;
    if (convTo == convFrom) convTo = (convTo - 1 + cat.count) % cat.count;
    recalc(); dirty = true; return;
  }
  if (k == ']') {   // TO 下一单位
    convTo = (convTo + 1) % cat.count;
    if (convTo == convFrom) convTo = (convTo + 1) % cat.count;
    recalc(); dirty = true; return;
  }
  if (k == 's' || k == 'S') {   // 交换 FROM 和 TO 单位
    std::swap(convFrom, convTo);
    recalc(); dirty = true; return;
  }
  if (k == 'c' || k == 'C' || k == '\n') {
    convInput = ""; convResult = ""; dirty = true; return;
  }
  if (k == '\b') {
    if (convInput.length()) convInput.remove(convInput.length() - 1);
    recalc(); dirty = true; return;
  }
  // 数字与小数点
  if (k >= '0' && k <= '9') {
    if (convInput.length() < 16) { convInput += k; recalc(); dirty = true; }
    return;
  }
  if (k == '-' && convInput.length() == 0) {
    convInput = "-"; dirty = true; return;
  }
}

// 超长字符串右侧截断显示
static String trimRight(const String& s, int maxCh) {
  if ((int)s.length() <= maxCh) return s;
  return "~" + s.substring(s.length() - maxCh + 1);
}

void drawConv() {
  cv.fillScreen(TFT_BLACK);
  const Cat& cat = CATS[convCat];

  // 1. 顶栏 (Converter + 类别序号)
  drawPageHeader("Converter", (String(convCat + 1) + "/" + String(CAT_COUNT)).c_str(), ACCENT);

  // 2. 类别轮播胶囊栏
  int prevCat = (convCat - 1 + CAT_COUNT) % CAT_COUNT;
  int nextCat = (convCat + 1) % CAT_COUNT;
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  int cw = cv.textWidth(cat.name) + 16;
  cv.fillRoundRect(SW / 2 - cw / 2, 14, cw, 12, 3, 0x18C3);
  cv.drawRoundRect(SW / 2 - cw / 2, 14, cw, 12, 3, 0x07FF);
  cv.setTextColor(TFT_WHITE, 0x18C3);
  cv.drawString(cat.name, SW / 2, 20);

  cv.setTextDatum(middle_right);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString(String("< ") + CATS[prevCat].name, SW / 2 - cw / 2 - 6, 20);

  cv.setTextDatum(middle_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString(String(CATS[nextCat].name) + " >", SW / 2 + cw / 2 + 6, 20);

  // 3. FROM 输入卡片 (上半)
  const int cardY1 = 28, cardH = 43;
  cv.fillRoundRect(4, cardY1, SW - 8, cardH, 4, 0x0821);
  cv.drawRoundRect(4, cardY1, SW - 8, cardH, 4, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("FROM", 10, cardY1 + 4);
  cv.setTextColor(ICON_DIM, 0x0821);
  cv.drawString("; .", 42, cardY1 + 4);

  // FROM 单位徽章
  int uw = cv.textWidth(cat.units[convFrom].name) + 10;
  cv.fillRoundRect(SW - 10 - uw, cardY1 + 3, uw, 11, 2, 0x1082);
  cv.drawRoundRect(SW - 10 - uw, cardY1 + 3, uw, 11, 2, 0x2145);
  cv.setTextDatum(middle_center);
  cv.setTextColor(0x07FF, 0x1082);
  cv.drawString(cat.units[convFrom].name, SW - 10 - uw / 2, cardY1 + 8);

  // 输入数值 (白，右对齐)
  cv.setTextDatum(middle_right); cv.setTextSize(2);
  cv.setTextColor(TFT_WHITE, 0x0821);
  String disp = convInput.length() ? convInput : "0";
  cv.drawString(trimRight(disp, 15), SW - 12, cardY1 + 27);

  // 4. 卡片间转换指示三角
  cv.fillTriangle(SW / 2 - 4, 71, SW / 2 + 4, 71, SW / 2, 74, 0x18C3);

  // 5. TO 换算结果卡片 (下半)
  const int cardY2 = 74;
  cv.fillRoundRect(4, cardY2, SW - 8, cardH, 4, 0x0821);
  cv.drawRoundRect(4, cardY2, SW - 8, cardH, 4, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x57EA, 0x0821);
  cv.drawString("TO", 10, cardY2 + 4);
  cv.setTextColor(ICON_DIM, 0x0821);
  cv.drawString("[ ]", 28, cardY2 + 4);

  // TO 单位徽章
  uw = cv.textWidth(cat.units[convTo].name) + 10;
  cv.fillRoundRect(SW - 10 - uw, cardY2 + 3, uw, 11, 2, 0x1082);
  cv.drawRoundRect(SW - 10 - uw, cardY2 + 3, uw, 11, 2, 0x2145);
  cv.setTextDatum(middle_center);
  cv.setTextColor(0x57EA, 0x1082);
  cv.drawString(cat.units[convTo].name, SW - 10 - uw / 2, cardY2 + 8);

  // 结果数值 (绿，右对齐)
  cv.setTextDatum(middle_right); cv.setTextSize(2);
  cv.setTextColor(cv.color565(80, 230, 110), 0x0821);
  String res = convResult.length() ? convResult : (convInput.length() ? "..." : "0");
  cv.drawString(trimRight(res, 15), SW - 12, cardY2 + 27);

  // 6. 底部操作指引栏
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("S", 6, 126);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK); cv.drawString("swap", 15, 126);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("C", 46, 126);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK); cv.drawString("clear", 55, 126);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(",/", 92, 126);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK); cv.drawString("cat", 106, 126);

  cv.setTextDatum(middle_right);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(";.[ ]", SW - 36, 126);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK); cv.drawString("unit", SW - 6, 126);
}

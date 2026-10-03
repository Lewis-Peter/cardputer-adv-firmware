#include "fx.h"
#include "bg_fetch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <cmath>
#include <ctime>
#include <cstring>
#include "tls_ca.h"
#include "ui_common.h"
#include "net_job.h"
#include "wifi_net.h"
#include "http_json.h"
#include "list_sel.h"

// 30 个日历日 ≈ 22 个交易日，90 个 ≈ 64 个。留 70 够两档都装下。
// 内存：70 × (float + 6 字节日期) ≈ 700 字节静态，可以接受。
static const int FX_MAX_PTS = 70;
static const uint32_t AUTO_MS = 60UL * 60 * 1000;   // 央行一天才更一次，一小时重拉绰绰有余
static const int ROWS_VISIBLE = 6;

// 两档窗口。天数是**日历天**，实际拿到的交易日点数会少三成左右（周末没有）。
static const struct { int days; const char* label; } WINDOWS[] = {
  {30, "30d"}, {90, "90d"},
};
static const int WINDOW_COUNT = (int)(sizeof(WINDOWS) / sizeof(WINDOWS[0]));
static int winIdx = 0;

struct FxPt { char date[11]; float rate; };   // "2026-09-02"
static FxPt* pts = nullptr;
static int  ptN = 0;
static int  selIdx = 0, scrollTop = 0;

static String   errMsg = "";
static bool     everFetched = false;
static uint32_t lastFetchMs = 0;

// ---- 涨跌配色：红涨绿跌（国内行情软件的习惯）----
// ⚠️ 这跟欧美是**反的**，也跟这个固件其余部分"红=报警"的用法不一样，所以颜色只当辅助：
// 数值一律带 ▲/▼ 和 ± 号，读法不依赖颜色。改这里的话记得两页一起改。
static uint16_t upDownColor(float d) {
  if (d > 0)  return cv.color565(242, 72, 72);     // 涨
  if (d < 0)  return ACCENT;                        // 跌
  return ICON_DIM;
}
// ▲/▼ 是**画**出来的，不是字体字符。原来用 Font0 的 0x1E/0x1F（经典 GLCD 字模里的
// 上下三角），实测在这块屏上渲出来是**空白**——而箭头恰恰是这一页读法的主要信号
// （颜色只是辅助，红涨绿跌跟欧美是反的），主要信号不能指望字体里有没有那个码位。
// cy 是三角形的垂直中心，h 是半高。
static void upDownArrow(int cx, int cy, int h, float d, uint16_t col) {
  if (d > 0)      cv.fillTriangle(cx, cy - h, cx - h, cy + h, cx + h, cy + h, col);
  else if (d < 0) cv.fillTriangle(cx, cy + h, cx - h, cy - h, cx + h, cy - h, col);
  else            cv.drawFastHLine(cx - h, cy, h * 2 + 1, col);
}

// ---- 取数 ----

// 把 t 格式化成 YYYY-MM-DD
static void ymd(char* out, size_t n, time_t t) {
  struct tm g{};
  if (gmtime_r(&t, &g)) strftime(out, n, "%Y-%m-%d", &g);
  else                  snprintf(out, n, "1970-01-01");
}

// ⚠️⚠️ 只有这个函数依赖响应的具体形状。接口文档给的是：
//   {"amount":1.0,"base":"USD","start_date":"...","end_date":"...",
//    "rates":{"2026-08-04":{"CNY":7.1523},"2026-08-05":{"CNY":7.1610}, ...}}
// 也就是 rates 是一个"日期 -> {币种: 值}"的对象。**还没对过真实响应**，
// 换源或者字段对不上时，改这一个函数就够了，上面的绘制一行都不用动。
static bool fxParse(JsonDocument& doc) {
  JsonObject rates = doc["rates"];
  if (rates.isNull()) { errMsg = "no 'rates' in reply"; return false; }

  if (!pts) pts = (FxPt*)malloc(sizeof(FxPt) * FX_MAX_PTS);
  if (!pts) { errMsg = "out of memory"; return false; }

  ptN = 0;
  for (JsonPair kv : rates) {
    if (ptN >= FX_MAX_PTS) break;
    // ⚠️ 两种形状都收：
    //   {"2026-08-04":{"CNY":7.15}}   文档里时间序列的样子
    //   {"2026-08-04":7.15}           只问一个币种时有些接口会把那层对象省掉
    // 这不是过度设计——解析器还没对过真实响应（见文件顶上），而这两种是最可能的分歧。
    // 多认一种的代价是三行；认错了的代价是整页空白，而且从屏幕上看不出是形状问题。
    float v = 0.0f;
    JsonVariant val = kv.value();
    if (val.is<JsonObject>()) v = val["CNY"] | 0.0f;
    else                      v = val | 0.0f;
    if (v <= 0) continue;                       // 缺这一天就跳过，不要画成 0
    strncpy(pts[ptN].date, kv.key().c_str(), sizeof(pts[ptN].date) - 1);
    pts[ptN].date[sizeof(pts[ptN].date) - 1] = 0;
    pts[ptN].rate = v;
    ptN++;
  }
  // 走到这儿说明 rates 在、但一个点都没解出来 —— 十有八九是形状跟预期不一样。
  // 屏幕上直接把下一步写出来，别让人对着一句"没有数据"猜是接口挂了还是解析错了。
  if (ptN == 0) { errMsg = "rates present but empty - try FXDUMP on serial"; return false; }

  // 按日期升序排一遍。**不假设接口按顺序返回**——JSON 对象本来就没有顺序保证，
  // 而这里顺序错了不会报错，只会画出一条乱七八糟的折线，很难看出是数据的锅。
  // YYYY-MM-DD 定长补零，直接字符串比较就是日期序。点数最多 70，插入排序足够。
  for (int i = 1; i < ptN; i++) {
    FxPt k = pts[i];
    int j = i - 1;
    while (j >= 0 && strcmp(pts[j].date, k.date) > 0) { pts[j + 1] = pts[j]; j--; }
    pts[j + 1] = k;
  }
  return true;
}

// URL 单独拿出来：串口的 FXDUMP 要打的必须是**同一条** URL，否则"我照着 dump 的形状写的
// 解析器却还是不工作"就成了新的谜题。
void fxBuildUrl(char* out, size_t n) {
  time_t now = time(nullptr);
  // 多往前要 3 天：万一今天是周末/假日，尾部就没有点了，多要几天保证窗口里一定有收盘价。
  char from[12], to[12];
  ymd(to, sizeof(to), now);
  ymd(from, sizeof(from), now - (time_t)(WINDOWS[winIdx].days + 3) * 86400);
  // ⚠️ 2026-09-03：api.frankfurter.app 已 301 到 api.frankfurter.dev/v1/，而 HTTPClient
  // 默认不跟重定向，于是这一页只会报 "http 301"。响应结构没变，改 URL 就够了。
  snprintf(out, n, "https://api.frankfurter.dev/v1/%s..%s?from=USD&to=CNY", from, to);
}

static void fetchFx() {
  // 错误字符串先备好容量：下面 TLS 那段借走了画布，期间再给它长内存会落进画布腾出的洞里
  errMsg.reserve(96);
  errMsg = "";
  lastFetchMs = millis();

  // 接收缓冲必须在进 lease 之前备好：pts 占 ~700B，若在 lease 里 malloc 会钉死画布的洞
  if (!pts) pts = (FxPt*)malloc(sizeof(FxPt) * FX_MAX_PTS);
  if (!pts) { errMsg = "out of memory"; dirty = true; return; }

  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }
  // 证书校验要比对有效期，时钟没对上必然握手失败——分开报，别混成网络错。
  // 这一页还有第二个理由必须等时钟：请求的起止日期就是拿本机时间算的。
  if (!tlsClockReady()) { errMsg = "waiting for clock (NTP)"; dirty = true; return; }

  char url[128];
  fxBuildUrl(url, sizeof(url));

  // TLS 握手要两块 ~16.7KB 缓冲，抓取期间把画布借出去，出作用域自动还。
  // client 和证书包在 lease 之前建好；lease 声明在 filter/doc 之前（逆序析构保证它们先释放）。
  WiFiClientSecure client;
  tlsUseCaBundle(client);                  // 真校验，不是 setInsecure（见 tls_ca.h）
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免汇率接口握手丢包卡死主循环
  client.setHandshakeTimeout(8);

  CanvasLease lease;
  // ⚠️ 这一页可以走 fetchJsonHttp（不像 GitHub / Quake 要逐元素流式解析）：
  // 一个交易日只有 `"2026-08-04":{"CNY":7.1523},` 二十来个字节，90 天窗口整包也就
  // 1.7KB 左右，配上过滤器进堆的更少。TLS 那 40KB 峰值躲不掉，但至少没有再叠一份大 JSON。
  JsonDocument filter;
  filter["rates"] = true;

  JsonDocument doc;
  HttpJsonOptions options(8000, 12000);
  options.stream = false;                  // 响应小，getString() 顺带把 chunked 解掉
  if (!fetchJsonHttp(client, url, doc, &filter, options, errMsg)) { dirty = true; return; }

  if (!fxParse(doc)) { ptN = 0; dirty = true; return; }

  selIdx = 0; scrollTop = 0;
  everFetched = true;
  dirty = true;
}

// ---- 生命周期 ----

static DeferredFetch job;

void fxEnter() {
  if (!pts) pts = (FxPt*)malloc(sizeof(FxPt) * FX_MAX_PTS);
  winIdx = loadUChar("fx", "win", 0);
  if (winIdx >= WINDOW_COUNT) winIdx = 0;   // 存的是 uint8_t，只可能大不可能负
  ptN = 0; selIdx = 0; scrollTop = 0;
  errMsg = ""; everFetched = false;
  job.request();
  lastFetchMs = millis();
  dirty = true;
}

void fxExit() {
  if (pts) {
    free(pts);
    pts = nullptr;
    ptN = 0;
  }
}

void fxUpdate() {
  // 拉取交给后台工人（bg_fetch.h）；lastFetchMs 先在主线程记上，工人还没开跑时别被重复触发
  if (job.due()) { lastFetchMs = millis(); bgFetchRun(fetchFx); return; }
  if (millis() - lastFetchMs >= AUTO_MS) { lastFetchMs = millis(); bgFetchRun(fetchFx); }
}

void fxKey(char k) {
  if (k == 'r' || k == 'R') job.request();            // 走延后路径，别在按键回调里同步拉
  else if (k == 'm' || k == 'M') {                    // 换窗口，选择记进 NVS
    winIdx = (winIdx + 1) % WINDOW_COUNT;
    saveUChar("fx", "win", (uint8_t)winIdx);
    ptN = 0; everFetched = false;
    job.request();
  } else if (ptN > 0 && (k == ';' || k == ',')) {
    listMove(selIdx, scrollTop, ptN, ROWS_VISIBLE, -1);
  } else if (ptN > 0 && (k == '.' || k == '/')) {
    listMove(selIdx, scrollTop, ptN, ROWS_VISIBLE, 1);
  }
  dirty = true;
}

// ---- 绘制 ----

int fxPointCount() { return ptN; }

static float latest()    { return ptN ? pts[ptN - 1].rate : 0.0f; }
static float prevClose() { return ptN >= 2 ? pts[ptN - 2].rate : latest(); }

// 没数据/出错时两页共用的占位，返回 true 表示已经画完、调用方直接 return
static bool drawEmpty() {
  if (ptN > 0) return false;
  cv.setTextDatum(top_center); cv.setTextSize(1);
  if (errMsg.length()) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString(trunc(errMsg, 38), SW / 2, 54);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("r  retry     m  switch window", SW / 2, 70);
  } else if (!everFetched) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("loading...", SW / 2, 60);
  } else {
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("no quotes in this window", SW / 2, 60);
  }
  drawPageDots();
  return true;
}

static void header(const char* title) {
  char right[24];
  snprintf(right, sizeof(right), "USD/CNY %s", WINDOWS[winIdx].label);
  drawPageHeader(title, right, ICON_DIM);
}

void drawFx() {
  job.markShown();                 // ⚠️ 必须第一行，见 net_job.h
  cv.fillScreen(TFT_BLACK);
  header("FX");
  if (drawEmpty()) return;

  const float now = latest(), prev = prevClose();
  const float d = now - prev;
  const uint16_t dc = upDownColor(d);
  char b[32];

  // 现价：这一页唯一该被一眼看到的东西
  cv.setTextDatum(top_left); cv.setTextSize(3);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(b, sizeof(b), "%.4f", now);
  cv.drawString(b, 6, 16);

  // 日变化：绝对值 + 百分比，各占一行摆在现价右边。
  // 箭头和 ± 号是主要信号，颜色只是辅助（红涨绿跌跟欧美习惯相反，见 upDownColor）。
  upDownArrow(126, 23, 5, d, dc);
  cv.setTextSize(2); cv.setTextDatum(top_left);
  cv.setTextColor(dc, TFT_BLACK);
  snprintf(b, sizeof(b), "%+.4f", d);
  cv.drawString(b, 136, 16);
  cv.setTextSize(1);
  snprintf(b, sizeof(b), "%+.2f%%", prev > 0 ? d / prev * 100.0f : 0.0f);
  cv.drawString(b, 136, 34);

  // 反向报价：换汇时真正想知道的往往是这一个方向
  cv.setTextSize(1); cv.setTextDatum(top_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  snprintf(b, sizeof(b), "1 CNY = %.4f USD", now > 0 ? 1.0f / now : 0.0f);
  cv.drawString(b, 6, 46);

  cv.setTextDatum(top_right);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  snprintf(b, sizeof(b), "as of %s", pts[ptN - 1].date + 5);   // 只留 MM-DD
  cv.drawString(b, SW - 6, 46);

  // ---- 走势图 ----
  // ⚠️ 横轴是"第几个交易日"，不是"第几天"：欧洲央行周末不公布，等距画出来周末是被压掉的。
  // 想按真实日期等距画就得留空档，而 240 像素宽塞不下 90 天还看得清，权衡下来按点等距。
  const int gx = 6, gy = 60, gw = SW - 12, gh = 46, baseY = gy + gh;
  float lo = pts[0].rate, hi = pts[0].rate;
  for (int i = 1; i < ptN; i++) {
    if (pts[i].rate < lo) lo = pts[i].rate;
    if (pts[i].rate > hi) hi = pts[i].rate;
  }
  if (hi - lo < 0.0005f) hi = lo + 0.0005f;      // 一整段几乎没动时别把噪声放大成锯齿
  const float span = hi - lo;

  cv.drawRect(gx, gy, gw, gh, DIM_BORDER);
  // 前一个收盘价那条水平参考线：折线在它上面就是涨、下面就是跌，一眼能看出来
  {
    int y = baseY - (int)((prev - lo) / span * (gh - 4)) - 2;
    for (int x = gx + 1; x < gx + gw - 1; x += 4) cv.drawPixel(x, y, DIM_BORDER);
  }
  int px = -1, py = 0;
  for (int i = 0; i < ptN; i++) {
    int x = gx + (ptN > 1 ? i * (gw - 1) / (ptN - 1) : gw / 2);
    int y = baseY - (int)((pts[i].rate - lo) / span * (gh - 4)) - 2;
    if (px >= 0) cv.drawLine(px, py, x, y, dc);
    px = x; py = y;
  }
  if (px >= 0) cv.fillCircle(px, py, 2, dc);     // 最新那一点点出来

  // 区间标在图**内**的右上/右下角，不占左边留白——weather 那两页就是被左边留白坑过
  // （标签比留白宽，右对齐之后首字符被推到屏幕外）。放里面就没有这个问题。
  cv.setTextSize(1);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(top_right);
  snprintf(b, sizeof(b), "hi %.4f", hi);
  cv.drawString(b, gx + gw - 3, gy + 2);
  cv.setTextDatum(bottom_right);
  snprintf(b, sizeof(b), "lo %.4f", lo);
  cv.drawString(b, gx + gw - 3, baseY - 2);

  // ⚠️ 居中的底部文字必须整个待在 123 行以上，否则会压在页码点上（点占 129..133，
  // 也是居中的）。top 基准 + 8px 字高 => y 最大 SH-20；写 SH-12 就正好压上去了，
  // 第一版就是这么错的。见 ui_common.h 里 drawPageDots 旁边那条规矩。
  cv.setTextDatum(top_center);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  snprintf(b, sizeof(b), "%d pts   m window   r refresh", ptN);
  cv.drawString(b, SW / 2, SH - 20);

  drawPageDots();
}

void drawFxDays() {
  job.markShown();
  cv.fillScreen(TFT_BLACK);
  header("FX days");
  if (drawEmpty()) return;

  // 最新的排在最上面：想看的多半是"最近几天怎么走的"
  const int rowH = 14, y0 = 16;
  for (int r = 0; r < ROWS_VISIBLE; r++) {
    int i = scrollTop + r;
    if (i >= ptN) break;
    const FxPt& p = pts[ptN - 1 - i];                       // 倒序
    const float prev = (ptN - 2 - i >= 0) ? pts[ptN - 2 - i].rate : p.rate;
    const float d = p.rate - prev;
    int y = y0 + r * rowH;
    bool sel = (i == selIdx);
    if (sel) {
      cv.fillRoundRect(2, y - 1, SW - 4, rowH - 1, 3, CARD_BG);
      cv.fillRect(2, y, 2, rowH - 3, ACCENT);
    }
    char b[24];
    cv.setTextSize(1); cv.setTextDatum(top_left);
    cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? CARD_BG : TFT_BLACK);
    cv.drawString(p.date, 8, y + 3);                        // 完整 YYYY-MM-DD

    cv.setTextDatum(top_right);
    cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? CARD_BG : TFT_BLACK);
    snprintf(b, sizeof(b), "%.4f", p.rate);
    cv.drawString(b, SW - 72, y + 3);

    cv.setTextColor(upDownColor(d), sel ? CARD_BG : TFT_BLACK);
    snprintf(b, sizeof(b), "%+.4f", d);
    cv.drawString(b, SW - 8, y + 3);
    upDownArrow(SW - 15 - cv.textWidth(b), y + 7, 3, d, upDownColor(d));   // 15 而不是 12：三角要跟 +/- 号留出空隙
  }
  drawScrollBar(SW - 4, y0, ROWS_VISIBLE * rowH, scrollTop, ptN, ROWS_VISIBLE);

  cv.drawFastHLine(6, 102, SW - 12, DIM_BORDER);
  char b[40];
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  snprintf(b, sizeof(b), "%d/%d", selIdx + 1, ptN);
  cv.drawString(b, 6, 106);
  cv.setTextDatum(top_right);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("ECB reference rate", SW - 6, 106);

  cv.setTextDatum(top_center);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString(";/. scroll   m window   r refresh", SW / 2, SH - 20);

  drawPageDots();
}

#include "github.h"
#include "bg_fetch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ctime>
#include <HTTPClient.h>
#include "tls_ca.h"
#include "ui_common.h"
#include "net_job.h"
#include "wifi_net.h"
#include "secrets.h"
#include "http_json.h"

// 接口最多回一年多一点（实测 369 天），留点余量
static const int MAX_DAYS = 380;
static const uint32_t STALE_MS = 30 * 60 * 1000;   // 半小时之内不重复拉

static uint8_t  dayLevel[MAX_DAYS];   // 0~4，GitHub 自己分好的档
static uint16_t dayCount[MAX_DAYS];   // 当天提交数
static int      nDays = 0;
static int      totalYear = 0;
static int      curStreak = 0, bestStreak = 0, bestDay = 0;
static String   errMsg = "";
static bool     haveData = false;
static uint32_t fetchedAtMs = 0;
static DeferredFetch job;   // 延后首拉，别在按键/绘制路径里同步拉网络（见 net_job.h）

// GitHub 深色主题那五档绿，直接抄官方色值——自己调的话很难像
// GitHub 深色主题配色规范
static const uint16_t GH_BG_DARK    = cv.color565(13, 17, 23);     // #0d1117 热力面板暗底
static const uint16_t GH_BORDER     = cv.color565(48, 54, 61);     // #30363d 边框线
static const uint16_t GH_CARD_BG    = cv.color565(22, 27, 34);     // #161b22 卡片底色
static const uint16_t GH_TEXT_MUTED = cv.color565(139, 148, 158); // #8b949e 次级灰字
static const uint16_t GH_GREEN_L1   = cv.color565(14, 68, 41);     // #0e4429
static const uint16_t GH_GREEN_L2   = cv.color565(0, 109, 50);     // #006d32
static const uint16_t GH_GREEN_L3   = cv.color565(38, 166, 65);    // #26a641
static const uint16_t GH_GREEN_L4   = cv.color565(57, 211, 83);    // #39d353
static const uint16_t GH_GREEN_EMPTY= cv.color565(22, 27, 34);     // #161b22 空格子

static uint16_t levelColor(uint8_t lv) {
  switch (lv) {
    case 1:  return GH_GREEN_L1;
    case 2:  return GH_GREEN_L2;
    case 3:  return GH_GREEN_L3;
    case 4:  return GH_GREEN_L4;
    default: return GH_GREEN_EMPTY;
  }
}

static void computeStats() {
  curStreak = bestStreak = bestDay = totalYear = 0;
  int run = 0;
  for (int i = 0; i < nDays; i++) {
    totalYear += dayCount[i];
    if (dayCount[i] > bestDay) bestDay = dayCount[i];
    if (dayCount[i] > 0) { run++; if (run > bestStreak) bestStreak = run; }
    else run = 0;
  }
  // 当前连续：从最后一天往回数。今天还没提交不算断——GitHub 自己也是这么显示的，
  // 所以最后一天是 0 就从倒数第二天开始数
  int i = nDays - 1;
  if (i >= 0 && dayCount[i] == 0) i--;
  for (; i >= 0 && dayCount[i] > 0; i--) curStreak++;
}

static void fetchContrib() {
  // 错误字符串先备好容量：下面 TLS 那段借走了画布，期间再给它长内存会落进画布腾出的洞里
  errMsg.reserve(96);
  errMsg = "";

  if (strlen(GITHUB_USER) == 0) { errMsg = "no GITHUB_USER in secrets.h"; dirty = true; return; }
  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }
  // 证书校验要比有效期，时钟没对上必然失败——分开报，别混成网络错
  if (!tlsClockReady()) { errMsg = "waiting for clock (NTP)"; dirty = true; return; }

  char url[128];
  snprintf(url, sizeof(url),
           "https://github-contributions-api.jogruber.de/v4/%s?y=last", GITHUB_USER);

  // TLS 握手要两块 ~16.7KB 缓冲外加证书链，流式抓取期间把画布借出去，出作用域自动还。
  // client 和证书包在 lease 之前建好；lease 声明在流式抓取之前。
  // 统计数组均为静态分配，errMsg 已提前 reserve，作用域内仅有用完即放的分配。
  WiFiClientSecure client;
  tlsUseCaBundle(client);
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免 GitHub 贡献接口握手丢包卡死主循环
  client.setHandshakeTimeout(10);

  static uint8_t  tmpLevel[MAX_DAYS];
  static uint16_t tmpCount[MAX_DAYS];
  static int      tmpDays = 0;
  tmpDays = 0;

  bool ok = false;
  {
    CanvasLease lease;
    HttpJsonOptions options(10000, 20000);
    ok = fetchJsonStreamArray(client, url, "\"contributions\":[", nullptr, options,
      [](JsonDocument& elem, int) -> bool {
        tmpCount[tmpDays] = (uint16_t)(elem["count"] | 0);
        tmpLevel[tmpDays] = (uint8_t)(elem["level"] | 0);
        tmpDays++;
        return tmpDays < MAX_DAYS;
      }, errMsg, MAX_DAYS);
  } // lease 析构，画布在这里恢复

  if (!ok || tmpDays == 0) {
    if (errMsg == "http 404" || (ok && tmpDays == 0)) {
      errMsg = "no data (user exists?)";
    }
    dirty = true;
    return;
  }

  nDays = tmpDays;
  memcpy(dayCount, tmpCount, nDays * sizeof(dayCount[0]));
  memcpy(dayLevel, tmpLevel, nDays * sizeof(dayLevel[0]));
  computeStats();
  haveData = true;
  // 只在拉成功时记"新鲜时刻"（跟 weather.cpp 一样）：记在开头的话，一次失败或者被用户取消的拉取
  // 也会把旧数据的新鲜度续上 30 分钟，重进这一页不会再拉
  fetchedAtMs = millis();
  errMsg = "";
  dirty = true;
}

void githubEnter() {
  if (!haveData || millis() - fetchedAtMs > STALE_MS) job.request();
  dirty = true;
}

void githubUpdate() {
  if (job.due()) bgFetchRun(fetchContrib);   // 交给后台工人（bg_fetch.h）
}

void githubKey(char k) {
  if (k == 'r' || k == 'R') job.request();   // 走同一条延后路径，别在按键回调里同步拉
  dirty = true;
}

void drawGithub() {
  job.markShown();   // ⚠️ 必须在最前面，下面的空状态会提前 return（见 net_job.h）
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("GitHub", GITHUB_USER, ICON_DIM);

  if (!haveData) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    if (errMsg.length()) {
      cv.fillRoundRect(16, 42, SW - 32, 52, 4, GH_CARD_BG);
      cv.drawRoundRect(16, 42, SW - 32, 52, 4, cv.color565(180, 50, 50));
      cv.setTextColor(TFT_RED, GH_CARD_BG);
      cv.drawString(trunc(errMsg, 34), SW / 2, 58);
      cv.setTextColor(GH_TEXT_MUTED, GH_CARD_BG);
      cv.drawString("press 'r' to retry", SW / 2, 78);
    } else {
      cv.fillRoundRect(24, 46, SW - 48, 44, 4, GH_BG_DARK);
      cv.drawRoundRect(24, 46, SW - 48, 44, 4, GH_BORDER);
      cv.setTextColor(GH_GREEN_L4, GH_BG_DARK);
      int dots = (millis() / 300) % 4;
      char loadStr[32];
      snprintf(loadStr, sizeof(loadStr), "loading contributions%.*s", dots, "...");
      cv.drawString(loadStr, SW / 2, 68);
    }
    return;
  }

  // 计算本地今天的对应索引
  int idx = -1;
  if (timeSynced && nDays > 0) {
    time_t now = time(nullptr);
    struct tm u{}, l{};
    if (gmtime_r(&now, &u) && localtime_r(&now, &l)) {
      int delta;
      if (l.tm_year == u.tm_year) delta = l.tm_yday - u.tm_yday;
      else                        delta = (l.tm_year > u.tm_year) ? 1 : -1;
      if      (delta ==  0)               idx = nDays - 1;
      else if (delta == -1 && nDays >= 2) idx = nDays - 2;
    }
  }

  // ---- 1. 热力图面板（固定全宽卡片槽，GitHub #0d1117 深色背景） ----
  const int CELL = 3, PITCH = 4, ROWS = 7;
  const int panelX = 4, panelY = 18, panelW = SW - 8, panelH = 33;

  cv.fillRoundRect(panelX, panelY, panelW, panelH, 3, GH_BG_DARK);
  cv.drawRoundRect(panelX, panelY, panelW, panelH, 3, GH_BORDER);

  int todayWd = 0;
  if (timeSynced) {
    time_t now = time(nullptr);
    struct tm ti{};
    if (localtime_r(&now, &ti)) todayWd = ti.tm_wday;
  }
  int firstWd = ((todayWd - (nDays - 1)) % 7 + 7) % 7;
  const int gx = panelX + 5;
  const int gy = panelY + 3;

  int todayX = -1, todayY = -1;
  for (int i = 0; i < nDays; i++) {
    int slot = firstWd + i;
    int col = slot / ROWS, row = slot % ROWS;
    int x = gx + col * PITCH, y = gy + row * PITCH;
    if (x + CELL > panelX + panelW - 4) break;
    cv.fillRect(x, y, CELL, CELL, levelColor(dayLevel[i]));
    if (i == idx) { todayX = x; todayY = y; }
  }

  // 今日高亮焦点光标框（Cursor Highlight）：一眼锁定今日位置
  if (todayX >= 0) {
    uint16_t focusCol = (idx >= 0 && dayLevel[idx] > 0) ? GH_GREEN_L4 : cv.color565(100, 190, 255);
    cv.drawRect(todayX - 1, todayY - 1, CELL + 2, CELL + 2, focusCol);
  }

  // ---- 2. 热力图下方：左侧刷新提示 + 右侧图例 (Less/More) 两侧对齐 ----
  const int legY = panelY + panelH + 2;
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(cv.color565(90, 100, 110), TFT_BLACK);
  cv.drawString("r: refresh", panelX + 2, legY);

  // 图例：Less [■■■■■] More，右对齐
  const int legBlockW = 5 * 4 - 1;   // 19px
  const int legX = panelX + panelW - 74;
  cv.drawString("Less", legX, legY);
  for (int i = 0; i < 5; i++) {
    cv.fillRect(legX + 26 + i * 4, legY + 2, 3, 3, levelColor(i));
  }
  cv.drawString("More", legX + 26 + legBlockW + 4, legY);

  // ---- 3. 年度总提交数大字区（平滑 Font4 + 徽标） ----
  const int sy = 65;
  char b[16];
  snprintf(b, sizeof(b), "%d", totalYear);

  cv.setFont(&fonts::Font4);
  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.drawString(b, 8, sy);
  int tw = cv.textWidth(b);
  cv.setFont(&fonts::Font0); // 恢复默认字库

  cv.setTextSize(1);
  cv.setTextColor(GH_GREEN_L4, TFT_BLACK);
  cv.drawString("contributions", 8 + tw + 8, sy + 2);
  cv.setTextColor(GH_TEXT_MUTED, TFT_BLACK);
  cv.drawString("in the last year", 8 + tw + 8, sy + 13);

  // 右上侧 Badge 标签
  const int badgeW = 56, badgeH = 13, badgeX = SW - badgeW - 6, badgeY = sy + 3;
  cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, GH_CARD_BG);
  cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, GH_BORDER);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(GH_TEXT_MUTED, GH_CARD_BG);
  cv.drawString("365 DAYS", badgeX + badgeW / 2, badgeY + badgeH / 2 + 1);

  // ---- 4. 底部三联微型暗底胶囊卡片（TODAY / STREAK / BEST） ----
  const int cardY = 91, cardH = 32;
  const int gap = 5, margin = 4;
  const int cardW = (SW - 2 * margin - 2 * gap) / 3; // (240 - 8 - 10) / 3 = 74

  auto drawStatCard = [&](int cx, const char* title, const char* val, uint16_t valCol, const char* unit) {
    cv.fillRoundRect(cx, cardY, cardW, cardH, 3, GH_CARD_BG);
    cv.drawRoundRect(cx, cardY, cardW, cardH, 3, GH_BORDER);

    // 标题标签
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(GH_TEXT_MUTED, GH_CARD_BG);
    cv.drawString(title, cx + cardW / 2, cardY + 3);

    // 核心数值 + 单位计算居中
    int numY = cardY + 13;
    if (unit && unit[0]) {
      char fullVal[16]; snprintf(fullVal, sizeof(fullVal), "%s", val);
      int nw = (int)strlen(fullVal) * 12; // size 2 单字宽约 12px
      int totalW = nw + 2 + (int)strlen(unit) * 6;
      int startX = cx + (cardW - totalW) / 2;

      cv.setTextDatum(top_left); cv.setTextSize(2);
      cv.setTextColor(valCol, GH_CARD_BG);
      cv.drawString(fullVal, startX, numY);

      cv.setTextSize(1);
      cv.setTextColor(GH_TEXT_MUTED, GH_CARD_BG);
      cv.drawString(unit, startX + nw + 2, numY + 5);
    } else {
      cv.setTextDatum(top_center); cv.setTextSize(2);
      cv.setTextColor(valCol, GH_CARD_BG);
      cv.drawString(val, cx + cardW / 2, numY);
    }
  };

  // Card 0: TODAY
  int c0 = margin;
  if (idx >= 0) {
    char tv[12]; snprintf(tv, sizeof(tv), "%d", dayCount[idx]);
    drawStatCard(c0, "TODAY", tv, dayCount[idx] > 0 ? GH_GREEN_L4 : TFT_DARKGREY, "");
  } else {
    drawStatCard(c0, "TODAY", "--", TFT_DARKGREY, "");
  }

  // Card 1: STREAK
  int c1 = c0 + cardW + gap;
  char sv[12]; snprintf(sv, sizeof(sv), "%d", curStreak);
  drawStatCard(c1, "STREAK", sv, curStreak > 0 ? GH_GREEN_L3 : TFT_DARKGREY, "d");

  // Card 2: BEST
  int c2 = c1 + cardW + gap;
  char bv[12]; snprintf(bv, sizeof(bv), "%d", bestStreak);
  drawStatCard(c2, "BEST", bv, TFT_WHITE, "d");

  // 有旧数据但刷新失败时，在底部显示错误提示
  if (errMsg.length()) {
    cv.setTextDatum(bottom_center);
    cv.setTextSize(1);
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString(trunc(errMsg, 38), SW / 2, SH - 2);
  }
}

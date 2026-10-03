#include "okx.h"
#include "bg_fetch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <cstdlib>
#include <ctime>
#include "tls_ca.h"
#include "ui_common.h"
#include "icons.h"
#include "net_job.h"
#include "wifi_net.h"
#include "http_json.h"

// 这是个慢变量（OKX 自己也不是每秒动），5 分钟重拉一次足够。
// ⚠️ 别调太短：每次都是一次 TLS 握手，而握手要抢 40KB 连续堆（见 README 的内存那节）。
static const uint32_t AUTO_MS = 5UL * 60 * 1000;

enum OkxSource {
  SOURCE_NONE = 0,
  SOURCE_OKX_C2C,
  SOURCE_COINGECKO
};

static float     rate = 0.0f;          // 1 USDT 折多少人民币
static char      rateAt[6] = "";       // 取到这个数的本机时间 HH:MM
static String    errMsg = "";
static bool      everFetched = false;
static uint32_t  lastFetchMs = 0;
static OkxSource activeSource = SOURCE_NONE;

void okxBuildUrl(char* out, size_t n) {
  snprintf(out, n, "https://www.okx.com/v3/c2c/otc-ticker/quotedPrice?side=buy&quoteCurrency=CNY&baseCurrency=USDT");
}

void okxBuildFallbackUrl(char* out, size_t n) {
  snprintf(out, n, "https://api.coingecko.com/api/v3/simple/price?ids=tether&vs_currencies=cny");
}

// 解析 OKX C2C 行情接口：
// {"code":0,"data":[{"bestOption":true,"depositName":"","payment":"wxPay","price":"7.28"}],...}
static bool parseOkxC2C(JsonDocument& doc, float& outRate, String& err) {
  bool codeOk = false;
  if (doc["code"].is<int>() && doc["code"].as<int>() == 0) {
    codeOk = true;
  } else if (doc["code"].is<const char*>() && strcmp(doc["code"].as<const char*>(), "0") == 0) {
    codeOk = true;
  }
  if (!codeOk) {
    const char* code = doc["code"].is<const char*>() ? doc["code"].as<const char*>() : "";
    const char* msg = doc["msg"] | doc["detailMsg"] | "";
    err = String("okx ") + (code[0] ? code : "err") + (msg[0] ? String(": ") + msg : String(""));
    return false;
  }

  JsonArrayConst data = doc["data"].as<JsonArrayConst>();
  if (data.isNull() || data.size() == 0) {
    err = "no 'data' in OKX reply";
    return false;
  }

  float r = 0.0f;
  for (JsonObjectConst item : data) {
    bool best = item["bestOption"] | false;
    JsonVariantConst p = item["price"];
    float val = 0.0f;
    if (p.is<const char*>())     val = atof(p.as<const char*>());
    else if (p.is<float>())      val = p.as<float>();
    else if (p.is<double>())     val = (float)p.as<double>();

    if (val > 0.0f) {
      if (best || r <= 0.0f) {
        r = val;
        if (best) break;
      }
    }
  }

  if (r <= 0.0f) {
    err = "no valid 'price' in OKX";
    return false;
  }

  outRate = r;
  return true;
}

// 解析 CoinGecko 接口：
// {"tether":{"cny":7.27}}
static bool parseCoinGecko(JsonDocument& doc, float& outRate, String& err) {
  JsonVariantConst cny = doc["tether"]["cny"];
  float val = 0.0f;
  if (cny.is<float>())        val = cny.as<float>();
  else if (cny.is<double>())  val = (float)cny.as<double>();
  else if (cny.is<const char*>()) val = atof(cny.as<const char*>());

  if (val <= 0.0f) {
    err = "no 'tether.cny' in CoinGecko";
    return false;
  }

  outRate = val;
  return true;
}

static void fetchOkx() {
  // 错误字符串先备好容量：下面 TLS 那段借走了画布，期间再给它们长内存会落进画布腾出的洞里
  errMsg.reserve(96);
  errMsg = "";
  lastFetchMs = millis();

  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }
  // 证书校验要比对有效期，时钟没对上必然握手失败——分开报，别混成网络错
  if (!tlsClockReady()) { errMsg = "waiting for clock (NTP)"; dirty = true; return; }

  float newRate = 0.0f;
  OkxSource newSource = SOURCE_NONE;
  String okxErr = "";
  String fbErr = "";
  okxErr.reserve(64);
  fbErr.reserve(64);

  // client 和证书包要在 lease 之前建：tlsUseCaBundle() 每次都会重新 calloc 一块永不释放的
  // 证书索引（~484B），放进 lease 里它会落在画布腾出的洞里把画布钉死。
  WiFiClientSecure client;
  tlsUseCaBundle(client);
  // 框架默认 TLS 握手超时 120s 且不受 HTTPClient 超时管控；握手包被墙静默丢弃时会把主循环卡死两分钟，
  // 这里收紧到 5s 以便快速切到 fallback（fallback 前会改成 7s）。
  client.setHandshakeTimeout(5);

  // 为什么一个 lease 覆盖整个过程（OKX + Fallback 两次握手）：
  // 1. fetchOkx() 在后台工人上一口气跑完（bg_fetch.h），期间主线程不画 cv，
  //    中间把画布还回来又立马借出去没有任何视觉意义；
  // 2. 两次借还凭空多一次 64.8KB 的 free/malloc，如果 OKX 失败留在堆上的瞬态碎片未清理，
  //    中间恢复画布甚至可能失败；
  // 3. 保持 lease 覆盖整个过程，正好把 64.8KB 大洞持续让给回退的 CoinGecko 握手（也需 ~33KB 缓冲）；
  // 4. client 复用同一个实例并在回退前 client.stop()，避免两次 tlsUseCaBundle() 重复分配证书索引。
  {
    CanvasLease lease;

    // 1. 优先尝试 OKX C2C 实时行情（带 OTC 实时溢价）
    {
      char url[128];
      okxBuildUrl(url, sizeof(url));

      JsonDocument doc;
      // 紧凑超时设置：若遇 GFW 拦截丢包，快速转入 Fallback，避免阻塞整机 UI
      HttpJsonOptions options(4500, 6000);
      options.stream = false;
      if (fetchJsonHttp(client, url, doc, nullptr, options, okxErr)) {
        if (parseOkxC2C(doc, newRate, okxErr)) {
          newSource = SOURCE_OKX_C2C;
        }
      }
    }

    // 2. OKX 失败（超时/GFW封锁/解析失败）时自动 Fallback 到 CoinGecko
    if (newSource == SOURCE_NONE) {
      client.stop(); // 彻底关闭上一次失败连接并释放 mbedTLS 瞬态缓冲
      char url[96];
      okxBuildFallbackUrl(url, sizeof(url));
      client.setHandshakeTimeout(7); // 同上：fallback 也不能再挂 120s

      JsonDocument doc;
      HttpJsonOptions options(5000, 8000);
      options.stream = false;
      if (fetchJsonHttp(client, url, doc, nullptr, options, fbErr)) {
        if (parseCoinGecko(doc, newRate, fbErr)) {
          newSource = SOURCE_COINGECKO;
        } else {
          errMsg = fbErr;
        }
      } else {
        // 两个源均失败，展示更有帮助的错误提示（利用已 reserve 的空间，避免 + 临时 String）
        if (okxErr.length()) {
          errMsg = "OKX:";
          errMsg += okxErr;
        } else {
          errMsg = fbErr;
        }
      }
    }
  } // lease 析构，画布在这里恢复

  if (newSource != SOURCE_NONE && newRate > 0.0f) {
    rate = newRate;
    activeSource = newSource;
    int h, m, s;
    if (nowHM(h, m, s)) snprintf(rateAt, sizeof(rateAt), "%02d:%02d", h, m);
    else                snprintf(rateAt, sizeof(rateAt), "--:--");
    everFetched = true;
    errMsg = "";
  } else if (!everFetched) {
    rate = 0.0f;
  }
  dirty = true;
}

// ---- 生命周期 ----

static DeferredFetch job;

void okxEnter() {
  if (!everFetched) {
    rate = 0.0f; rateAt[0] = 0;
    activeSource = SOURCE_NONE;
  }
  errMsg = "";
  job.request();
  lastFetchMs = millis();
  dirty = true;
}

void okxUpdate() {
  // 拉取交给后台工人（bg_fetch.h）；lastFetchMs 先在主线程记上，工人还没开跑时别被重复触发
  if (job.due()) { lastFetchMs = millis(); bgFetchRun(fetchOkx); return; }
  if (millis() - lastFetchMs >= AUTO_MS) { lastFetchMs = millis(); bgFetchRun(fetchOkx); }
}

void okxKey(char k) {
  if (k == 'r' || k == 'R') job.request();   // 走延后路径，别在按键回调里同步拉
  dirty = true;
}

// ---- 绘制 ----

void drawOkx() {
  job.markShown();                 // ⚠️ 必须第一行，下面的空状态会提前 return（见 net_job.h）
  cv.fillScreen(TFT_BLACK);

  // 顶部状态栏：标明当前数据源状态（OKX C2C 显青色，Fallback 显黄色）
  const char* rightHdr = (activeSource == SOURCE_COINGECKO) ? "CoinGecko" : "OKX C2C";
  uint16_t rightCol = (activeSource == SOURCE_COINGECKO) ? TFT_YELLOW : ACCENT;
  drawPageHeader("OKX", rate > 0 ? rightHdr : "USDT/CNY", rate > 0 ? rightCol : ICON_DIM);

  if (rate <= 0) {
    cv.setTextDatum(top_center); cv.setTextSize(1);
    if (errMsg.length()) {
      cv.setTextColor(TFT_RED, TFT_BLACK);
      cv.drawString(trunc(errMsg, 38), SW / 2, 54);
      cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
      cv.drawString("r  retry", SW / 2, 70);
    } else {
      cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
      cv.drawString(everFetched ? "no rate" : "loading...", SW / 2, 60);
    }
    return;
  }

  // 左边放 OKX 的标记（备选源时使用低调浅灰），右边放数
  icoOkx(28, 56, 18, (activeSource == SOURCE_COINGECKO) ? 0x9CD3 : TFT_WHITE);

  char b[32];
  // 主角：1 USDT 值多少人民币
  const int numX = 60, numY = 42;
  cv.setTextDatum(top_left); cv.setTextSize(3);
  cv.setTextColor(ACCENT, TFT_BLACK);
  snprintf(b, sizeof(b), "%.4f", rate);
  cv.drawString(b, numX, numY);
  const int numW = cv.textWidth(b);

  cv.setTextSize(1);
  cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  cv.drawString("1 USDT =", numX, 30);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("CNY", numX + numW + 8, numY + 14);

  // 反向：换一万块能拿多少 U
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  snprintf(b, sizeof(b), "1 CNY = %.4f USDT", 1.0f / rate);
  cv.drawString(b, numX, 76);

  // 数据源清晰标注行
  cv.setTextDatum(top_left);
  if (activeSource == SOURCE_COINGECKO) {
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("CoinGecko (OKX fallback)", 6, 96);
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("OKX C2C real-time market price", 6, 96);
  }

  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  snprintf(b, sizeof(b), "updated %s", rateAt);
  cv.drawString(b, 6, SH - 20);
  cv.setTextDatum(top_right);
  cv.drawString("r  refresh", SW - 6, SH - 20);
}


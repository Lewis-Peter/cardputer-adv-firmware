// ⚠️ 已知问题：HTTPS 握手在这块板子上是**临界的**，多数时候会失败：
//     E (xxxxx) esp-sha: Failed to allocate buf memory
//     [E][ssl_client.cpp] start_ssl_client(): (-1) ERROR
//
// 2026-08-09 实测定位（串口 STAT / MEMCAP 两条指令，minEver 是关键）：
//     握手前   heap=74524 largest=63476 minEver=70284
//     握手失败 heap=71300 largest=42996 minEver=4236   ← 掉到 4.2KB
// 也就是说**整个堆在握手期间被抽干**，不是某个特殊内存池不够。
// 曾经怀疑过是 DMA 可用内存单独紧张（esp-sha 走 DMA 路径），实测排除了：
//     刚复位  DMA free=66740 largest=63476，跟 MALLOC_CAP_8BIT 基本同一个池
// 63KB 的连续块申请不到 128 字节是不可能的——问题在于那一刻只剩 4KB。
//
// 元凶最可能是 mbedTLS 的 CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384：
// 两个 16KB 的 I/O buffer 就是 32KB，加上证书链解析和签名校验的临时对象，
// 峰值接近 70KB，而我们总共只有 74KB（主画布那 64KB 已经扣掉了）。
// 所以它是"刚好差一点"——偶尔能成功，多数失败。
//
// ⚠️ 想改那个配置不容易：Arduino framework 的 libmbedcrypto/libmbedtls 是**预编译**的，
// 光在 platformio.ini 里加 -D 改不动实现，得重编 framework。
// 真要治本，方向是减少请求时刻的常驻内存，而不是调 TLS 参数。
#include "chat.h"
#include <vector>
#include <atomic>
#include <cstring>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "tls_ca.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ui_common.h"
#include "wifi_net.h"
#include "secrets.h"

String chatInput;
String chatError;
int chatPage = 0;

// 正文从顶栏下面 14 起画，7 行 * 16 = 末行顶部 110、底部 124，正好停在页码指示器
// （SH-4）上方。写成 8 行的话末行会越过 135 的屏底被裁掉——而分页也是按这个数算的，
// 那样每页第 8 行的内容会永远读不到。改这里记得同步核对 drawChatReply() 的起始 y。
static const int CHAT_MAX_LINES = 7;
static const int CHAT_LINE_H = 16;
static String chatReply;

// 请求体控制在约 8KB 内：足够保留三轮短对话，又不会在 TLS 已经打散连续堆后再申请大块。
static const int CHAT_HISTORY_TURNS = 3;
static const int CHAT_HISTORY_REPLY_MAX = 1200;
static const int CHAT_REPLY_MAX = 6000;
static String chatUsers[CHAT_HISTORY_TURNS];
static String chatReplies[CHAT_HISTORY_TURNS];
static int chatHistoryCount = 0;

// 回复的总页数。只在回复变化时重算一次——算它要把整段回复重新排版一遍，
// 而回复页每帧都在画，现算等于把最贵的操作放进最热的路径。
static int chatPagesCached = 1;

static int utf8CharLen(const String& s, int pos) {
  uint8_t c = (uint8_t)s[pos];
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1; // 键盘目前只产生 ASCII；坏 UTF-8 也必须能被退格清掉
}

void chatBackspace() {
  if (!chatInput.length()) return;
  int p = (int)chatInput.length() - 1;
  while (p > 0 && ((uint8_t)chatInput[p] & 0xC0) == 0x80) p--;
  chatInput.remove(p);
}

static bool busyStaticDrawn = false;
static int busyLastDot = -1;

void chatReset() {
  chatInput = "";
  chatError = "";
  chatReply = "";
  chatPagesCached = 1;
  chatPage = 0;
  chatHistoryCount = 0;
  busyStaticDrawn = false;
  busyLastDot = -1;
  for (int i = 0; i < CHAT_HISTORY_TURNS; i++) { chatUsers[i] = ""; chatReplies[i] = ""; }
}

// UTF-8 安全按屏宽换行，逻辑跟 reader.cpp 的分页换行一致（中英文混排都能处理）
static std::vector<String> wrapText(const String& text, int maxW) {
  std::vector<String> lines;
  LovyanGFX& gfx = canvasAvailable() ? static_cast<LovyanGFX&>(cv) : static_cast<LovyanGFX&>(M5.Display);
  gfx.setFont(&fonts::efontCN_14); gfx.setTextSize(1);
  String line;
  size_t i = 0;
  while (i < text.length()) {
    uint8_t c0 = (uint8_t)text[i];
    int n = (c0 & 0x80) == 0 ? 1 : (c0 & 0xE0) == 0xC0 ? 2 : (c0 & 0xF0) == 0xE0 ? 3 : (c0 & 0xF8) == 0xF0 ? 4 : 1;
    if (i + n > text.length()) n = 1;
    String ch = text.substring(i, i + n);
    i += n;
    if (ch == "\n") { lines.push_back(line); line = ""; continue; }
    if (ch == "\r") continue;
    String test = line + ch;
    if (gfx.textWidth(test.c_str()) > maxW && line.length() > 0) {
      lines.push_back(line);
      line = ch;
    } else {
      line = test;
    }
  }
  if (line.length() > 0) lines.push_back(line);
  gfx.setFont(&fonts::Font0);
  return lines;
}

// 折行扫描：按屏宽把回复折成一行行，每折出一行就回调 emit(text)。
// 计数和绘制**共用这一份**——两处各写一遍必然漂移，而且以前就是那样：
// drawChatReply 自己扫一遍、末尾再调 chatPageCount 又扫一遍，每帧两次全量排版。
//
// ⚠️ 热路径上不要 `String test = line + ch`：那是**每个字符一次堆分配**，
// 6000 字节的回复每帧就是上万次 malloc/free。这块板子没 PSRAM，对碎片极敏感
// （见 README 的"踩过的坑"）。改成累加宽度 + `line += ch`（摊销扩容），
// 这几个点阵字体没有字距调整，逐字宽度之和等于整串宽度。
template <typename F>
static void chatWrapScan(F emit) {
  int lineW = 0;
  String line;
  for (size_t i = 0; i < chatReply.length();) {
    int n = utf8CharLen(chatReply, i);
    if (i + n > (int)chatReply.length()) n = 1;
    String ch = chatReply.substring(i, i + n);
    i += n;
    if (ch == "\r") continue;
    if (ch == "\n") { emit(line); line = ""; lineW = 0; continue; }
    const int cw = cv.textWidth(ch.c_str());
    if (lineW + cw > SW - 12 && line.length()) { emit(line); line = ch; lineW = cw; }
    else { line += ch; lineW += cw; }
  }
  if (line.length()) emit(line);
}

static void chatRecountPages() {
  if (!chatReply.length()) { chatPagesCached = 1; return; }
  cv.setFont(&fonts::efontCN_14); cv.setTextSize(1);
  int lines = 0;
  chatWrapScan([&](const String&) { lines++; });
  cv.setFont(&fonts::Font0);
  chatPagesCached = (lines + CHAT_MAX_LINES - 1) / CHAT_MAX_LINES;
  if (chatPagesCached < 1) chatPagesCached = 1;
}

int chatPageCount() {
  return chatPagesCached;
}

// ---- 后台请求任务 ----
// 以前 chatSend() 是在 handleKey() 里同步跑的，一条请求最长 60s，期间键盘轮询不到、
// BtnA 没反应、自动熄屏也不会触发——整机看着像死了。现在挪到单独的 FreeRTOS 任务里。
//
// 线程边界（要紧）：任务里只碰堆（HTTP + JSON），绝不碰 cv。
// wrapText() 要用 cv.textWidth() 量字宽，跟主循环的 render() 是同一块画布，
// 所以排版必须留在主线程的 chatUpdate() 里做。
// 跨线程握手用 std::atomic 而不是 volatile：volatile 只挡编译器对*这一个*变量的优化，
// 既不阻止它把下面几个 String 的写重排到标志之后，也不产生跨核的内存屏障。
// release/acquire 才是这里要的语义——任务侧 release 之前的所有写，主线程 acquire 之后必然可见。
static std::atomic<bool> workerDone{false};
static std::atomic<bool> workerBusy{false};   // chatBusy() 读它，不读 TaskHandle
static String         workerReply;          // 成功时的正文
static String         workerErr;            // 失败原因
static String         workerBody;           // 请求体，主线程组装好再交给任务（别让任务读 chatInput）
static String         workerSsid, workerPw; // Wi-Fi 凭据同理，主线程读 NVS 再交给任务
static bool           replyReady = false;   // 排版完成、等主循环切到回复页
// 用户在请求还没回来时就退出了这一页。任务杀不得（它正卡在 mbedTLS 里，强杀会把握手
// 期间那几十 KB 连同 socket 一起漏掉），所以只标记一下，让 chatUpdate() 把结果丢掉。
static std::atomic<bool> workerAbandoned{false};

// ⚠️ 正文单独拎出来，唯一目的是**让它正常 return**。
// vTaskDelete(nullptr) 不跑 C++ 析构：它把整块栈回收掉，栈上那些对象的析构函数
// 一个都不会执行。而这个函数的栈上恰好有三样带堆的东西：
//   WiFiClientSecure —— 析构里才 stop()，mbedTLS 的收发缓冲是 16KB+4KB 那个量级
//   HTTPClient       —— http.end() 顶不上：disconnect() 里 `if (_reuse && _canReuse)`
//                       那条分支根本不关 socket（原文抄在 router.cpp trafficClose()），
//                       而 _reuse 默认 true、OpenAI 那类端点必然回 keep-alive
//   String reply/err —— 回复最长 CHAT_REPLY_MAX，也是一块堆
// 三样加起来，每问一次就走掉几十 KB，而这块板子总共才 70KB 出头的空闲堆。
//
// 所以规矩是：**正文里一律用 return，绝不在正文里 vTaskDelete**。
// 这样以后往里加提前返回的分支也不会重新引入这个坑——不是靠记住补一句 stop()，
// 是让语言替我们保证。
static void chatWorkerBody() {
  String reply, err;

  // 配置错误不必先连 Wi-Fi；否则 key 漏填时会先白等 15 秒，用户只看到像网络坏了。
  if (!CHAT_BASE_URL || !CHAT_BASE_URL[0]) err = "no CHAT_BASE_URL configured";
  else if (!CHAT_API_KEY || !CHAT_API_KEY[0] || !strcmp(CHAT_API_KEY, "sk-REPLACE-ME"))
    err = "no CHAT_API_KEY configured";

  // Wi-Fi 也在这儿连。以前这步走的是 connectWith()，它会往屏上刷握手日志，只能留在
  // 主线程，于是没连网时按下回车照样卡 15s。这里用静默版：不碰 cv，只轮询状态。
  if (!err.length() && WiFi.status() != WL_CONNECTED) {
    if (workerSsid.length() == 0) {
      err = "no saved wifi";
    } else {
      WiFi.mode(WIFI_STA);
      WiFi.begin(workerSsid.c_str(), workerPw.c_str());
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
        vTaskDelay(pdMS_TO_TICKS(100));
      if (WiFi.status() != WL_CONNECTED) err = "wifi connect failed";
    }
  }
  if (err.length()) {
    workerReply = "";
    workerErr   = err;
    workerBody  = "";
    workerSsid  = "";
    workerPw    = "";
    return;
  }
  workerSsid = "";
  workerPw   = "";

  WiFiClientSecure client;
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免大模型 API 握手丢包卡死主循环
  client.setHandshakeTimeout(15);
  // 证书策略，从严到宽三档。这个请求的 Authorization 头里带着 API key，
  // 发到一条没验过身份的连接上等于交给中间人，所以默认必须是校验过的。
  //   1) 配了 CHAT_ROOT_CA -> 证书钉扎，最严（只认这一张根，连 bundle 里的别的 CA 都不认）
  //   2) 否则用根证书 bundle -> 真校验。**这是现在的默认**，不用再手贴证书了
  //   3) CHAT_ALLOW_INSECURE = true -> 显式降级，每次都往串口打一行警告
  //
  // ⚠️ 原来没有第 2 档，所以"不贴证书"只能失败关闭，Chat 页开箱即不可用。而 README 当时
  //    不肯硬编证书的理由是"根证书会过期，到期后设备就连不上"——bundle 恰好解决它：
  //    121 张根证书，过期一张还有其余的，换 CDN 换 CA 也照样能验。
  if (CHAT_ROOT_CA && CHAT_ROOT_CA[0]) {
    client.setCACert(CHAT_ROOT_CA);
  } else if (CHAT_ALLOW_INSECURE) {
    client.setInsecure();
    Serial.println("[chat] ⚠ CHAT_ALLOW_INSECURE 打开着，本次请求不校验证书，API key 可被中间人窃取");
  } else if (!tlsClockReady()) {
    // 用 bundle 校验就要比证书有效期，时钟没对上必然失败。说清楚是等时钟，
    // 别让人以为是网络或 key 的问题（开机后 NTP 是后台跑的，有几秒窗口）。
    workerReply = "";
    workerErr   = "waiting for clock (NTP)";
    workerBody  = "";
    return;
  } else {
    tlsUseCaBundle(client);
  }

  HTTPClient http;
  http.setConnectTimeout(15000);   // TCP 连接超时，默认只有 5000ms，实机路由到这个域名经常不够
  http.setTimeout(60000);          // 这个是收数据的超时，HTTPClient::setTimeout 是 uint16_t，最大只能到 ~65s
  // 走 HTTP/1.0：服务端（尤其反代/网关）就不能回 chunked 分块编码，
  // 下面直接对 getStream() 流式解析才不会读到混在数据里的块长度行
  http.useHTTP10(true);
  String url = String(CHAT_BASE_URL) + "/v1/chat/completions";
  if (!http.begin(client, url)) {
    err = "http begin failed";
  } else {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + CHAT_API_KEY);

    int code = http.POST(workerBody);
    workerBody = ""; // 发完请求体立即释放，给后续 TLS 响应流与 JsonDocument 腾出堆空间
    if (code != 200) {
      if (code == 401 || code == 403) err = "auth failed: check CHAT_API_KEY";
      else if (code == 408 || code == 429) err = code == 429 ? "rate limited: try later" : "request timeout";
      else if (code >= 500) err = "server error: try later";
      else err = String("http error ") + code;
      if (code < 0) {                        // 连接层面失败（DNS/TCP/TLS），把 mbedTLS 的原始错误也带上
        char detail[100];
        client.lastError(detail, sizeof(detail));
        err = (code == HTTPC_ERROR_READ_TIMEOUT) ? "request timeout" : "network error";
        if (detail[0]) { err += ": "; err += detail; }
      }
    } else {
      JsonDocument respDoc;
      DeserializationError e;
      // 不先 getString()：回复异常大时，那份整包副本会和 JsonDocument 同时占堆。
      int bodySize = http.getSize();
      if (bodySize > 12000) {
        err = "reply too large";
        http.end();
        workerReply = ""; workerErr = err;
        return;
      }
      e = deserializeJson(respDoc, http.getStream());
      // ⚠️ 别把 NoMemory 也报成"JSON 坏了"——那会把人往"服务器返回有问题"的方向带，
      // 而真实原因是 TLS 握手之后堆已经被打散、装不下解析树了。这两种要分开说，
      // 一个该重试/换短问题，另一个该去查服务端。
      if (e == DeserializationError::NoMemory) err = "reply too big for free heap";
      else if (e) err = "bad json reply";
      else {
        const char* content = respDoc["choices"][0]["message"]["content"] | (const char*)nullptr;
        if (!content) err = "no content in reply";
        else {
          reply = content;
          if ((int)reply.length() > CHAT_REPLY_MAX) reply.remove(CHAT_REPLY_MAX);
        }
      }
    }
    http.end();
  }

  workerReply = reply;
  workerErr   = err;
}

static void chatWorker(void*) {
  chatWorkerBody();          // 这里返回 = 栈上的 client / http / String 真的被析构了
  workerBusy.store(false, std::memory_order_relaxed);
  // release 必须是最后一步：主线程 acquire 到它之后，workerReply/workerErr 的写才保证可见。
  // 挪到正文外面之后这条更强了：连析构都排在 release 之前。
  workerDone.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool chatBusy() { return workerBusy.load(std::memory_order_relaxed); }

bool chatTakeReply() {
  if (!replyReady) return false;
  replyReady = false;
  return true;
}

// 离开 Chat 页时调（cleanupApp 挂的）。在途请求没法取消，只能让它自己跑完然后丢掉结果。
void chatExit() {
  busyStaticDrawn = false;
  busyLastDot = -1;
  if (chatBusy()) {
    // 握手还占着堆，这时抢 64KB 画布多半失败；画布留给 chatUpdate() 收尾时恢复
    workerAbandoned.store(true, std::memory_order_relaxed);
  } else {
    workerBody = "";
    workerSsid = "";
    workerPw   = "";
    canvasRestore();
  }
}

void chatSend() {
  if (chatBusy()) return;
  workerAbandoned.store(false, std::memory_order_relaxed);   // 新请求，之前的放弃标记作废
  chatError = "";
  chatReply = "";
  chatPagesCached = 1;
  chatPage = 0;

  // 凭据在主线程读（Preferences 那个句柄是全局共享的，别让任务也去动它），
  // 连接动作本身交给任务——它最长要 15s，放在按键路径里就是白卡。
  workerSsid = ""; workerPw = "";
  if (WiFi.status() != WL_CONNECTED) {
    String ss, pw;
    if (loadCreds(ss, pw)) { workerSsid = ss; workerPw = pw; }
  }

  // 请求体在主线程组装好——不让任务去读 chatInput，免得跟键盘输入撞上
  JsonDocument reqDoc;
  reqDoc["model"] = CHAT_MODEL;
  JsonArray msgs = reqDoc["messages"].to<JsonArray>();
  for (int i = 0; i < chatHistoryCount; i++) {
    JsonObject u = msgs.add<JsonObject>(); u["role"] = "user"; u["content"] = chatUsers[i];
    JsonObject a = msgs.add<JsonObject>(); a["role"] = "assistant"; a["content"] = chatReplies[i];
  }
  JsonObject m = msgs.add<JsonObject>();
  m["role"] = "user";
  m["content"] = chatInput;
  workerBody = "";
  serializeJson(reqDoc, workerBody);

  workerDone.store(false, std::memory_order_relaxed);
  workerBusy.store(true, std::memory_order_relaxed);   // 建任务之前先置位，免得任务先跑起来把它清掉
  busyStaticDrawn = false;
  busyLastDot = -1;

  // 16KB 栈：mbedTLS 握手很吃栈，8KB 会溢出。跑在 core 0（Arduino 的 loop 在 core 1），
  // 任务干完自己 vTaskDelete，栈随之还给堆，不是常驻开销。
  if (xTaskCreatePinnedToCore(chatWorker, "chat", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    workerBusy.store(false, std::memory_order_relaxed);
    chatError = "no memory for request";
    workerBody = "";
    workerSsid = "";
    workerPw   = "";
  } else {
    // 成功创建 worker 任务之后释放全局画布 cv，腾出 ~64.8KB 连续堆给 mbedTLS 握手
    canvasRelease();
  }
  dirty = true;
}

void chatUpdate() {
  if (!workerDone.load(std::memory_order_acquire)) return;
  workerDone.store(false, std::memory_order_relaxed);

  busyStaticDrawn = false;
  busyLastDot = -1;

  // 这一趟是用户已经离开页面之后才回来的：结果整个丢掉。
  // 不丢的话 chatInput 早被重进时的 chatReset() 清空了，下面会把 {问题:"" , 回答:上一题的}
  // 配成一对写进历史，之后每次请求都带着这个错位上下文发出去；replyReady 也会把正在
  // 打字的用户一把拽到回复页，显示的还是他已经放弃的那个问题的答案。
  if (workerAbandoned.exchange(false, std::memory_order_relaxed)) {
    workerReply = "";
    workerErr   = "";
    workerBody  = "";
    workerSsid  = "";
    workerPw    = "";
    canvasRestore();
    dirty = true;   // 人已经在别的页面了，画布回来后要重画一次
    return;
  }

  // 请求完成，恢复全局画布用于后续排版与页面渲染
  canvasRestore();

  if (workerErr.length()) {
    chatError = workerErr;
  } else {
    chatReply = workerReply;
    if ((int)chatReply.length() > CHAT_REPLY_MAX) chatReply.remove(CHAT_REPLY_MAX);
    chatRecountPages();          // 页数只在这里算一次，绘制时直接用缓存
    // 请求成功后才写入历史；网络失败不能污染下一次重试的上下文。
    if (chatHistoryCount == CHAT_HISTORY_TURNS) {
      for (int i = 1; i < CHAT_HISTORY_TURNS; i++) { chatUsers[i - 1] = chatUsers[i]; chatReplies[i - 1] = chatReplies[i]; }
      chatHistoryCount--;
    }
    chatUsers[chatHistoryCount] = chatInput;
    chatReplies[chatHistoryCount] = chatReply;
    if ((int)chatReplies[chatHistoryCount].length() > CHAT_HISTORY_REPLY_MAX)
      chatReplies[chatHistoryCount].remove(CHAT_HISTORY_REPLY_MAX);
    chatHistoryCount++;
    replyReady = true;
  }
  workerReply = "";
  workerErr = "";
  dirty = true;
}

// 请求进行中直接在 M5.Display 上画"询问中 + 动态省略号"（cv 已释放，不能走 cv.pushSprite 路径）
void drawChatBusyDirect() {
  const int boxY = 28, boxH = 66;
  const int ey = boxY + boxH + 8;

  if (!busyStaticDrawn) {
    M5.Display.fillScreen(TFT_BLACK);

    // 1. 顶栏标题
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_left);
    M5.Display.fillRoundRect(1, 3, 2, 7, 1, ACCENT);
    M5.Display.setTextColor(ACCENT, TFT_BLACK);
    M5.Display.drawString("ChatGPT", 7, 3);
    M5.Display.drawFastHLine(0, PAGE_HDR_BOTTOM, SW, DIM_BORDER);

    // 2. 输入框线框
    M5.Display.drawRoundRect(8, boxY, SW - 16, boxH, 4, TFT_DARKGREY);

    // 3. 正在提问的问题（带光标）
    std::vector<String> lines = wrapText(chatInput + "_", SW - 28);
    M5.Display.setFont(&fonts::efontCN_14);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.setTextDatum(top_left);
    int maxLines = boxH / CHAT_LINE_H;
    int start = (int)lines.size() > maxLines ? (int)lines.size() - maxLines : 0;
    int ly = boxY + 4;
    for (int i = start; i < (int)lines.size(); i++) {
      M5.Display.drawString(lines[i].c_str(), 14, ly);
      ly += CHAT_LINE_H;
    }
    M5.Display.setFont(&fonts::Font0);

    busyStaticDrawn = true;
    busyLastDot = -1;
  }

  // 4. 动态省略号（仅在每 400ms 状态变化时局部重绘，避免整屏闪烁及高频刷屏占总线）
  int dot = (int)((millis() / 400) % 4);
  if (dot != busyLastDot) {
    busyLastDot = dot;
    M5.Display.fillRect(0, ey - 2, SW, 16, TFT_BLACK);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.setTextDatum(top_center);
    char b[16];
    snprintf(b, sizeof(b), "asking%.*s", dot, "...");
    M5.Display.drawString(b, SW / 2, ey);
  }
}

void drawChatInput() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("ChatGPT");

  const int boxY = 28, boxH = 66;
  cv.drawRoundRect(8, boxY, SW - 16, boxH, 4, TFT_DARKGREY);

  std::vector<String> lines = wrapText(chatInput + "_", SW - 28);
  cv.setFont(&fonts::efontCN_14); cv.setTextSize(1);
  cv.setTextColor(TFT_GREEN, TFT_BLACK);
  cv.setTextDatum(top_left);
  int maxLines = boxH / CHAT_LINE_H;
  int start = (int)lines.size() > maxLines ? (int)lines.size() - maxLines : 0;
  int ly = boxY + 4;
  for (int i = start; i < (int)lines.size(); i++) { cv.drawString(lines[i].c_str(), 14, ly); ly += CHAT_LINE_H; }
  cv.setFont(&fonts::Font0);

  if (chatBusy()) {
    // 请求在后台跑，主循环还活着——所以这里能画个会动的省略号，
    // 而不是像以前那样整机冻在一帧 "asking..." 上
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.setTextDatum(top_center); cv.setTextSize(1);
    char b[16];
    snprintf(b, sizeof(b), "asking%.*s", (int)((millis() / 400) % 4), "...");
    cv.drawString(b, SW / 2, boxY + boxH + 8);
  } else if (chatError.length() > 0) {
    std::vector<String> errLines = wrapText(chatError, SW - 16);
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.setTextDatum(top_left); cv.setTextSize(1);
    int ey = boxY + boxH + 6;
    for (int i = 0; i < (int)errLines.size() && i < 2; i++) { cv.drawString(errLines[i].c_str(), 8, ey); ey += 12; }
  }

}

void drawChatReply() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("Chat reply", ";/. page", ICON_DIM);
  cv.setFont(&fonts::efontCN_14); cv.setTextSize(1);
  cv.setTextColor(TFT_WHITE, TFT_BLACK); cv.setTextDatum(top_left);
  int y = PAGE_HDR_BOTTOM + 2;
  int first = chatPage * CHAT_MAX_LINES, lineNo = 0;
  chatWrapScan([&](const String& s) {
    if (lineNo >= first && lineNo < first + CHAT_MAX_LINES) { cv.drawString(s.c_str(), 6, y); y += CHAT_LINE_H; }
    lineNo++;
  });
  cv.setFont(&fonts::Font0);

  drawPageDots(chatPage, chatPageCount());
}

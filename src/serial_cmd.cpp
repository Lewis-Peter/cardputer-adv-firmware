#include "serial_cmd.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <SD.h>
#include <esp_heap_caps.h>

#include "globals.h"
#include "tls_ca.h"
#include "http_json.h"     // HTTP_UA：FXDUMP 要跟正常取数发一样的 User-Agent
#include "led.h"
#include "ram_profile.h"
#include "wifi_net.h"
#include "gbk_table.h"   // ensureUtf8：有的路由器 SSID 是 GBK 编码
#include "gnss.h"
#include "netprobe.h"
#include "adsb.h"
#include "lanscan.h"
#include "typhoon.h"
#include "quake.h"
#include "fx.h"
#include "okx.h"
#include "sats.h"
#include "router.h"
#include "github.h"
#include "weather.h"
#include "sd_files.h"
#include "wsniff.h"
#include "wardrive.h"
#include "wifi_chan.h"
#include "ridapp.h"
#include "bt.h"
#include "bctrail.h"
#include "power_util.h"
#include "battlog.h"
#include "reader.h"
#include "imu.h"
#include "astro.h"
#include "lora.h"
#include "radio.h"
#include "hotspot.h"
#include "ir.h"
#include "ducky.h"
#include "ssh_app.h"
#include "hash_oven.h"
#include "pcmode.h"
#include "ui_common.h"

// 裸跳转（GOTO / GNSSMAP）故意**不**调 cleanupApp：这是修 MENU 搁浅内存那次特意留下的
// 逃生口（见 docs/serial-sweep.md）——正在 LoRa 抓包 / BLE 扫描时想跳去看一眼别的页面，
// 清理会把采集直接掐断。代价是从占资源的 app 跳走时那块内存会搁浅到重启，而这个坑最容易
// 在串口巡检里把堆的读数带偏。所以不清，但一定要吼一声，别让它悄悄发生。
static void warnRawJump(Screen from) {
  Serial.printf("[jump] 裸跳转，不 cleanup：离开 screen=%d。若它占着资源（地图 48KB / "
                "LoRa SPI / BLE 协议栈）会搁浅到重启；要正常退出请用 MENU 或 BACK\n",
                (int)from);
}

// 把某个 HTTPS 接口的原始响应原样打到串口。
//
// 存在理由：fx / okx 两页的解析器都是**照接口文档写的、没对过真实响应**——写它们的
// 机器上出口代理把那几个域名全拦了。形状要是有出入，页面只会显示一句"解不出来"，
// 而从屏幕上根本看不出差在哪。有了这个，一条命令就能看见真实形状，
// 顺带还能把它存成模拟器的假数据（跟 tools/uisim/data/ 那几份 JMA 报文一个待遇）。
static void serialDumpJson(const char* tag, const char* url) {
  Serial.printf("[%s] GET %s\n", tag, url);
  if (WiFi.status() != WL_CONNECTED) { Serial.printf("[%s] wifi not connected\n", tag); return; }
  if (!tlsClockReady()) { Serial.printf("[%s] 时钟还没对上(NTP)，证书校验必然失败\n", tag); return; }

  WiFiClientSecure client;
  tlsUseCaBundle(client);
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免串口 dump 时握手丢包卡死主循环
  client.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.setUserAgent(HTTP_UA);
  if (!http.begin(client, url)) { Serial.printf("[%s] http begin failed\n", tag); return; }

  int code = http.GET();
  Serial.printf("[%s] http %d, len=%d\n", tag, code, http.getSize());
  if (code == 200) {
    // ⚠️ 别 getString()：那会在堆上多留一份整包副本，而这会儿刚做完 TLS 握手、
    // 正是堆最紧的时候。逐字节转发到串口就够了，占用恒定。
    Stream& st = http.getStream();
    int n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 8000 && n < 4096) {
      if (!st.available()) {
        // Stream 上没有 connected()，问底下的 socket——对端收完就关，这里靠它退出。
        if (!client.connected()) break;
        delay(2);
        continue;
      }
      Serial.write((char)st.read());
      n++;
    }
    Serial.printf("\n[%s] dumped %d bytes\n", tag, n);
  }
  http.end();
  client.stop();   // ⚠️ end() 在 keep-alive 时不关 socket，见 chat.cpp 顶上那段
}

// 最近一条串口指令到达时，屏幕是不是灭的。见下面唤醒那段的注释。
static bool lastCmdSawScreenOff = false;

// WIFISCAN：扫描周边 AP，按 JSON 行流式吐到串口，给 PC 端（cardputer-bridge）解析。
// 每行以固定前缀 "WSCAN " 起头，PC 据此从其它串口日志里筛出扫描数据（跟 SPEC/SHOT 一个套路）。
static const char* wifiAuthName(int enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "?";
  }
}

// 只有 SSID 可能含 " \ 和控制字符，转义它就够（BSSID/enc 都是安全字符）。就地写进 out。
static void jsonEscapeInto(const String& s, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < s.length() && o + 7 < cap; i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
    else if ((uint8_t)c < 0x20)  { o += snprintf(out + o, cap - o, "\\u%04x", (uint8_t)c); }
    else out[o++] = c;
  }
  out[o] = '\0';
}

static void wifiScanToSerial() {
  Serial.println("WSCAN {\"t\":\"start\"}");
  doScan();   // 阻塞 2~4s：复用 wifi_net 里带热点共存+重试的扫描路径
  if (scanCount < 0) {
    Serial.println("WSCAN {\"t\":\"error\",\"msg\":\"scan failed\"}");
    return;
  }
  pcmodeRecordWifiScan(scanCount);
  char esc[112];   // 32 字节 GBK 转 UTF-8 最多膨胀到 48 字节，再算上转义
  uint32_t scanTs = millis();
  for (int i = 0; i < scanCount; i++) {
    jsonEscapeInto(ensureUtf8(WiFi.SSID(i)), esc, sizeof(esc));   // GBK 的 SSID 先转成 UTF-8，跟 WiFi 页面显示一致
    Serial.printf("WSCAN {\"i\":%d,\"ts\":%lu,\"ssid\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"enc\":\"%s\"}\n",
                  i, (unsigned long)scanTs, esc, WiFi.BSSIDstr(i).c_str(), (int)WiFi.RSSI(i),
                  (int)WiFi.channel(i), wifiAuthName(WiFi.encryptionType(i)));
  }
  Serial.printf("WSCAN {\"t\":\"end\",\"n\":%d}\n", scanCount);
  WiFi.scanDelete();   // 结果读完立即释放驱动里的扫描缓冲
}

static void handleSerialCmd(const String& raw) {
  String cmd = raw;
  cmd.trim();
  if (cmd.length() == 0) return;
  // 熄屏时物理按键第一下只负责唤醒、不触发动作（防误触）——但串口指令是主动发出的，不存在
  // "不小心碰到"这回事，在这里先唤醒再往下处理，避免每条远程指令都要先发一次空的把屏幕叫醒。
  //
  // 记下"这条指令到达时屏是不是灭的"，因为下面这一唤醒会立刻把 screenOff 抹平——
  // STAT 想报熄屏状态就只能报这个快照，直接读 screenOff 永远是 0（观测行为本身改变了
  // 被观测的状态，2026-08-26 为此绕了半天）。
  lastCmdSawScreenOff = screenOff;
  if (screenOff) {
    M5.Display.setBrightness(brightVal());
    screenOff = false;
    ledApply();
    dirty = true;
    // ⚠️ 必须一起把活动时间刷新，否则这次唤醒撑不过一帧：loop() 末尾的熄屏判据看的是
    //    millis()-lastActivityMs，不刷新的话下一轮立刻又满足条件、马上灭回去，
    //    屏幕只亮约 20ms。表现就是"发了指令屏幕还是黑的"，而这条唤醒看着明明执行了。
    lastActivityMs = millis();
  }
  if (cmd.length() == 1) {
    if (screen == SCREEN_PCMODE && cmd[0] != '`') {
      // 在 PC MODE 下，除 ` 退出键外，单字符进入后续指令分支（如 ? 显示 HELP，其余报 unknown）
    } else {
      handleKey(cmd[0]);
      return;
    }
  }
  String up = cmd; up.toUpperCase();
  if (screen == SCREEN_PCMODE) {
    if      (up == "ENTER") { handleKey('\n'); return; }
    else if (up == "BACK")  { handleKey('`'); return; }
  } else {
    bool navAlias = true;
    if      (up == "ENTER")    handleKey('\n');
    else if (up == "BACK")     handleKey('`');
    else if (up == "UP")       handleKey(';');
    else if (up == "DOWN")     handleKey('.');
    else if (up == "SPACE")    handleKey(' ');
    else navAlias = false;
    // 导航别名到此为止，必须 return：以前会穿透下去——
    //  · ENTER 在 Settings 选中 "PC MODE" 时，handleKey 已把 screen 切成 PCMODE，
    //    接着撞上 PCMODE 白名单，多吐一条 PCM {"t":"err","cmd":"ENTER"}；
    //  · 其余情况掉进最后的 else，把 "ENTER"/"UP" 这几个字母又逐字符喂一遍给当前界面。
    if (navAlias) {
      Serial.printf("[remote] cmd=\"%s\" -> screen=%d\n", cmd.c_str(), (int)screen);
      return;
    }
  }

  if (screen == SCREEN_PCMODE) {
    bool pcmValid = (up == "PCMODE" || up == "PCEXIT" || up == "MENU" ||
                     up == "CAPS" || up == "SCAN WIFI" || up == "WIFISCAN" ||
                     up == "GNSS ON" || up.startsWith("GNSS ON ") || up == "GNSS OFF" ||
                     up == "IMU ON" || up.startsWith("IMU ON ") || up == "IMU OFF" ||
                     up == "LORA SNIFF" || up.startsWith("LORA SNIFF ") || up == "LORA OFF" ||
                     up == "RID ON" || up.startsWith("RID ON ") || up == "RID OFF" ||
                     up == "BLE ON" || up == "BLE OFF" ||
                     up == "GPS" || up == "NMEA" || up.startsWith("NMEA ") ||
                     up == "STAT" || up == "HELP" || up == "?" || up == "REBOOT" ||
                     up == "BRIGHT" || up.startsWith("BRIGHT ") ||
                     up == "VOL" || up.startsWith("VOL ") || up == "MUTE" || up == "UNMUTE" ||
                     up == "SLEEP" || up.startsWith("SLEEP ") ||
                     up == "SHOT" || up == "SHOTRAW" ||
                     up == "BATT" || up.startsWith("BATT ") || up.startsWith("BATTLOG") ||
                     up == "WIFI" || up == "WIFIOFF" || up == "RANDMAC" || up.startsWith("SETMAC ") ||
                     up == "TRAIL" || up.startsWith("TRAIL ") || up == "RAMLOG");
    if (!pcmValid) {
      char esc[128];
      jsonEscapeInto(cmd, esc, sizeof(esc));
      Serial.printf("PCM {\"t\":\"err\",\"cmd\":\"%s\",\"msg\":\"unknown\"}\n", esc);
      return;
    }
  }

  if (screen == SCREEN_SSH && sshIsTerminalActive()) {
    // 处于 SSH 终端中：除基本系统控制指令（STAT/MENU/REBOOT/SLEEP/BRIGHT/VOL/MUTE）外，
    // 其余指令（如 ls, fastfetch, cat, pwd 等）均当作终端命令直接打入远程 Shell！
    if (up != "STAT" && up != "MENU" && up != "REBOOT" &&
        up != "BRIGHT" && !up.startsWith("BRIGHT ") &&
        up != "SLEEP" && !up.startsWith("SLEEP ") &&
        up != "VOL" && !up.startsWith("VOL ") &&
        up != "MUTE" && up != "UNMUTE") {
      for (size_t i = 0; i < cmd.length(); i++) handleKey(cmd[i]);
      handleKey('\n');
      return;
    }
  }
  // 回主菜单前先走一遍 app 的释放：MENU 归在"导航键别名"这一组（和 ENTER/BACK 并列），
  // 语义是模拟用户回菜单，就该和按 ` 一样清理。裸跳转会把当前 app 占的内存搁浅到重启——
  // GNSS 地图那 48KB 底图缓存最明显，而且拿串口调试堆余量时正好会被这条自己污染。
  // 想要不清理的裸跳转用 GOTO（它的语义本来就是"仅供看 UI"）。
  //
  // ⚠️ 这里必须是 if 而不是 else if：以前是 else if，整条分发链被接到了上面 SSH 那个 if
  //    的 else 上——SSH 终端里发 STAT/MENU/REBOOT/BRIGHT… 这些"放行"的指令会跳过整条链，
  //    什么都不做。
  if (up == "MENU")     { cleanupApp(screen); screen = SCREEN_MENU; dirty = true; }
  else if (up == "PCMODE") {
    pcmodeEnter();
  }
  else if (up == "PCEXIT") {
    if (screen == SCREEN_PCMODE) {
      pcmodeExit();
    } else {
      // 协议规矩：每条命令恰好一个回执。不在 PC MODE 时也回 exit（幂等，跟 LORA OFF 已关也回 end
      // 一个道理）——bridge 重启后不知道设备状态，发 PCEXIT 兜底时要能拿到一个确定答复。
      Serial.println("PCM {\"t\":\"exit\"}");
    }
  }
  else if (up == "CAPS") {
    pcmodePrintCaps();
  }
  else if (up == "NETPROBE") { screen = SCREEN_NETPROBE; netprobeEnter(NETPROBE_MODE_PROBE); dirty = true; }
  else if (up == "DNSFUZZ")  { screen = SCREEN_NETPROBE; netprobeEnter(NETPROBE_MODE_DNS); dirty = true; }
  else if (up == "SSH")      { screen = SCREEN_SSH; sshEnter(); dirty = true; }
  else if (up == "HASHOVEN" || up == "OVEN") { screen = SCREEN_HASHOVEN; hashOvenEnter(); dirty = true; }
  else if (up == "ROUTER")   { screen = SCREEN_ROUTER; routerEnter(); dirty = true; }
  else if (up == "WSNIFF" || up == "SNIFFER") { screen = SCREEN_WSNIFF; wsniffEnter(); dirty = true; }
  else if (up == "WARDRIVE") { screen = SCREEN_WARDRIVE; wardriveEnter(); dirty = true; }
  else if (up == "ASTRO" || up == "DAYLIGHT" || up == "SUN") { screen = SCREEN_ASTRO_SUN; astroEnter(); dirty = true; }
  else if (up == "MOON")     { screen = SCREEN_MOON; astroEnter(); dirty = true; }
  else if (up == "TERM" || up == "TERMINATOR") { screen = SCREEN_ASTRO_TERM; astroEnter(); dirty = true; }
  else if (up == "IMU")      { screen = SCREEN_COMPASS; imuTare(); dirty = true; }
  else if (up == "IMUDATA")  { screen = SCREEN_COMPASS_DETAIL; dirty = true; }
  else if (up == "LORA SNIFF" || up.startsWith("LORA SNIFF ")) {
    const char* args = nullptr;
    if (up.startsWith("LORA SNIFF ")) {
      args = cmd.c_str() + 11;
      while (*args == ' ') args++;
    }
    LoraSniffConfig cfg;
    const char* errReason = nullptr;
    if (!loraSniffParseArgs(args, cfg, &errReason)) {
      char errBuf[64];
      loraSniffFormatErr(errBuf, sizeof(errBuf), "bad args");
      Serial.println(errBuf);
    } else {
      loraSniffStart(cfg);
    }
  }
  else if (up == "LORA OFF") {
    loraSniffStop();
  }
  else if (up == "RID ON" || up.startsWith("RID ON ")) {
    // Remote ID 无屏流（PC 主导模式）：RID ON [信道]，给了信道就蹲死不跳频（1..13），越界 → err
    int chan = 0;
    bool bad = false;
    if (up.startsWith("RID ON ")) {
      String a = cmd.substring(7); a.trim();
      if (a.length()) {
        chan = a.toInt();
        if (chan < 1 || chan > 13) bad = true;
      }
    }
    if (bad) Serial.println("RID {\"t\":\"err\",\"msg\":\"bad args\"}");
    else     ridStreamStart(chan);
  }
  else if (up == "RID OFF") {
    ridStreamStop();
  }
  else if (up == "BLE ON")  { btStreamStart(); }
  else if (up == "BLE OFF") { btStreamStop(); }
  else if (up == "LORA")     { warnRawJump(screen); screen = SCREEN_LORA; loraEnter(); dirty = true; }
  else if (up == "WIFICHAN") { warnRawJump(screen); screen = SCREEN_WIFI_CHAN; wifiChanScan(); dirty = true; }
  else if (up == "WIFICHAN_DETAIL" || up == "APDETAIL") { warnRawJump(screen); screen = SCREEN_WIFI_CHAN_DETAIL; dirty = true; }
  else if (up == "RADIO") { warnRawJump(screen); screen = SCREEN_RADIO; radioEnter(); dirty = true; }
  else if (up == "RADIOPLAY") { warnRawJump(screen); screen = SCREEN_RADIO_PLAY; dirty = true; }
  else if (up == "RADIOURL") { warnRawJump(screen); screen = SCREEN_RADIO_URL; dirty = true; }
  else if (up == "BTSCAN") { warnRawJump(screen); screen = SCREEN_BT_SCAN; btScan(); dirty = true; }
  else if (up == "BTDEV" || up == "BTDETAIL") { warnRawJump(screen); screen = SCREEN_BT_DEVICE; dirty = true; }
  else if (up == "BTRADAR") {
    warnRawJump(screen);
    if (btDevCount == 0) { btScan(); }
    if (btDevCount > 0) { btRadarStart(btDevIdx); }
    screen = SCREEN_BT_RADAR;
    dirty = true;
  }
  else if (up == "HOTSPOT") { warnRawJump(screen); screen = SCREEN_HOTSPOT; dirty = true; }
  else if (up == "HOTSPOT_QR" || up == "HOTSPOTQR") { warnRawJump(screen); hotspotEnsureOn(); screen = SCREEN_HOTSPOT_QR; dirty = true; }
  else if (up == "HOTSPOT_PW" || up == "HOTSPOTPW") { warnRawJump(screen); hotspotPwInput = hotspotPassword(); screen = SCREEN_HOTSPOT_PW; dirty = true; }
  else if (up == "IR") { warnRawJump(screen); screen = SCREEN_IR; irEnter(); dirty = true; }
  else if (up == "DUCKY") { warnRawJump(screen); screen = SCREEN_DUCKY; duckyEnter(); dirty = true; }
  else if (up.startsWith("PROBE ")) {
    // 任意目标一次性探测，不用改代码重刷机，格式 "PROBE host[:port]"。屏幕/状态都不切，
    // 结果只走 Serial，可以在 NetProbe 巡检、DnsFuzz 等任何界面下随时插一条查询。
    netprobeAdhoc(cmd.substring(6));
  }
  else if (up == "STAT") {
    // 只读状态快照：WiFi 状态/堆余量/最大连续块。查内存问题时最常用的三行，
    // 而且不像 WIFI 指令那样会顺手触发一次连接，可以用来观察"看门狗有没有自己拉回来"
    const char* ws = WiFi.status() == WL_CONNECTED ? "connected" : "down";
    Serial.printf("[stat] wifi=%s mode=%d rssi=%d ip=%s ssid=%s\n",
                  ws, (int)WiFi.getMode(), (int)WiFi.RSSI(),
                  WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
    // 熄屏状态从外面完全看不见，而它会改变按键语义（熄屏时第一键只负责唤醒、
    // 被 handleKey 吞掉），串口驱 UI 时这是个隐形的坑。所以报出来。
    Serial.printf("[stat] screenOffOnArrival=%d idle=%us sleepAt=%ds\n",
                  lastCmdSawScreenOff ? 1 : 0,
                  (unsigned)((millis() - lastActivityMs) / 1000),
                  SLEEP_OPTS[sleepOptIdx]);
    // 菜单索引从外面完全看不见，而它是跨进出保留的——串口驱 UI 时"我以为我在索引 0"
    // 是最容易栽的坑（docs/serial-sweep.md 记了三次）。报出来就不用再猜。
    Serial.printf("[stat] menu=%d/%d (%s) group=%s\n",
                  menuIndex, APP_COUNT, APPS[menuIndex].name,
                  GROUPS[menuGroupOf(menuIndex)].name);
    Serial.printf("[stat] heap=%u largest=%u minEver=%u screen=%d\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)ESP.getMinFreeHeap(), (int)screen);
    // loop 任务的栈剩余水位。为什么要看它：mbedTLS 握手很吃栈——chat.cpp 就是因为 8KB 会溢出
    // 才专门开了个 16KB 的任务——而 Sats/GitHub 的 HTTPS 握手是**直接在 loop 任务上**跑的。
    // 想给它们加上真正的证书校验（setCACert 替代 setInsecure）之前，先在那一页拉完一次再敲
    // STAT 看这个数：证书链验证的大数运算还要再吃几 KB，余量太小就得先把 loop 栈提上去
    // （SET_LOOP_TASK_STACK_SIZE(16*1024)）或者把取数挪进后台任务。
    // ⚠️ 单位是字节不是字：ESP-IDF 的 StackType_t 是 uint8_t，所以这个返回值本来就以字节计
    //    （xTaskCreate 的栈大小参数同理，chat.cpp 传的 16384 就是 16KB），别再乘 4。
    Serial.printf("[stat] loopStackFree=%u bytes\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    Serial.printf("[stat] vol=%d%% (effective=%d)%s bootsnd=%s debug=%s\n",
                  volPct, volVal(), debugOn ? " [DEBUG MUTED]" : "",
                  bootSoundOn ? "ON" : "OFF", debugOn ? "ON" : "OFF");
  }
  else if (up.startsWith("TRAIL MARK")) {
    // 手动埋一个字节。用途是**自证工具是好的**：埋一个 → 按复位 → 看开机回放里还在不在。
    // 面包屑平时读出来全是零（bcMark 只在查问题时才现撒），所以没有这条指令的话，
    // "RTC 内容能扛过软复位"这个前提永远没机会验证，等真出事了才发现工具是坏的就晚了。
    // 接受十进制和 0x 十六进制（轨迹是按十六进制打的，能对上眼更省事）。
    const char* arg = cmd.c_str() + 10;     // "TRAIL MARK" 正好 10 个字符
    char* end = nullptr;
    long v = strtol(arg, &end, 0);
    // end == arg 表示一个数字都没读到（比如只发了裸的 TRAIL MARK）。不拦的话 strtol
    // 会返回 0，于是埋下一个 00——而 00 恰好跟"空槽"长得一模一样，等于埋了个看不见的。
    if (end == arg)          { Serial.println("[bctrail] 用法：TRAIL MARK n   (n = 0..255 或 0x..)"); }
    else if (v < 0 || v > 255) { Serial.println("[bctrail] mark 取值要在 0..255"); }
    else {
      bcMark((uint8_t)v);
      Serial.printf("[bctrail] marked %02X\n", (unsigned)v);
    }
  }
  else if (up == "TRAIL") {
    // 面包屑轨迹：查"卡死了但看不见现场"用。开机时也会自动回放上一轮的。
    // 判读看**哪个编号没出现**——最后出现的那个之后就是卡住的地方。
    bcTrailDump(Serial, "command");
  }
  else if (up == "BATT" || up.startsWith("BATT ")) {
    // 电量/充电这块没有电流传感（TP4057 的 CHRG 脚没接到主控），只能从一个被负载污染的
    // 电压反推。想改进先得看清噪声本身长什么样——这条就是干这个的。
    powerDiag(up.length() > 5 ? cmd.substring(5).toInt() : 64);
  }
  else if (up == "LS" || up.startsWith("LS ")) {
    // 串口通道一直有 CAT 没有 LS——文件名猜不出来的时候（比如 /battlog 里到底有几个
    // 日志、哪个覆盖了那次放电）就卡死了。列目录：名字 + 字节数，目录标 <DIR>。
    String path = (cmd.length() > 3) ? cmd.substring(3) : String("/");
    path.trim();
    if (!path.startsWith("/")) path = "/" + path;
    if (!sdMounted) { Serial.println("[ls] 没有 SD 卡"); }
    else {
      File d = SD.open(path);
      if (!d)              Serial.printf("[ls] 打不开 %s\n", path.c_str());
      else if (!d.isDirectory()) Serial.printf("[ls] %s 不是目录（用 CAT 读文件）\n", path.c_str());
      else {
        Serial.printf("[ls] %s\n", path.c_str());
        int n = 0;
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
          if (f.isDirectory()) Serial.printf("  <DIR> %s\n", f.name());
          else                 Serial.printf("  %7u %s\n", (unsigned)f.size(), f.name());
          f.close();
          if (++n >= 200) { Serial.println("  ...（超过 200 条，截断）"); break; }
        }
        Serial.printf("[ls] 共 %d 项\n", n);
      }
      if (d) d.close();
    }
  }
  else if (up.startsWith("BATTLOG")) {
    // ⚠️ 必须在**拔线之前**打开。开关存 NVS、重启也保持——因为这个日志存在的全部意义
    // 就是记录"串口断开之后"发生了什么，那时候你没法再发指令。
    if      (up.endsWith("ON"))  { battLogSet(true);  Serial.println("[batt] 日志已开：每 10 秒一行写到 SD /battlog/。现在可以拔线了"); }
    else if (up.endsWith("OFF")) { battLogSet(false); Serial.println("[batt] 日志已关"); }
    else Serial.printf("[batt] 日志当前 %s（用 BATTLOG ON / BATTLOG OFF 切换）\n",
                       battLogEnabled() ? "开着" : "关着");
  }
  else if (up == "FXDUMP") {
    // 汇率那一页的解析器是照接口文档写的、**没对过真实响应**（写这行的机器上，出口代理
    // 把所有免 key 汇率接口都拦了）。所以留这条：直接把原始响应打出来，形状一眼可见。
    //
    // 用途有两个：一是页面报 "rates present but empty" 时确认到底差在哪；
    // 二是把这段 JSON 存下来当模拟器的假数据（tools/uisim/data/frankfurter_usdcny.json），
    // 跟那三份 JMA 报文一个待遇。
    char url[128];
    fxBuildUrl(url, sizeof(url));
    serialDumpJson("fx", url);
  }
  else if (up == "OKXDUMP" || up.startsWith("OKXDUMP ")) {
    // OKXDUMP 打主源 OKX C2C 原始响应；OKXDUMP FB 打备用源 CoinGecko 原始响应
    char url[128];
    if (up.indexOf("FB") >= 0 || up.indexOf("FALLBACK") >= 0 || up.indexOf("CG") >= 0) {
      okxBuildFallbackUrl(url, sizeof(url));
      serialDumpJson("okx-fallback", url);
    } else {
      okxBuildUrl(url, sizeof(url));
      serialDumpJson("okx", url);
    }
  }
  else if (up == "RAMLOG") {
    // MEMCAP/STAT 报的是"此刻还剩多少"，这条报的是"**是哪一步**吃掉的"。
    // 查"这版固件比上版多吃了内存，是谁多吃的"只能靠它。
    ramDump(Serial);
  }
  else if (up == "MEMCAP") {
    // 各类内存池分开看。调试条上那个 largest 是 MALLOC_CAP_8BIT 的，而 DMA/内部内存
    // 是另一个池子——TLS 握手里 esp-sha 报 "Failed to allocate buf memory" 时，
    // 光看 8BIT 那个数会被误导成"内存还很宽裕"。
    struct { const char* name; uint32_t caps; } pools[] = {
      {"8BIT    ", MALLOC_CAP_8BIT},
      {"INTERNAL", MALLOC_CAP_INTERNAL},
      {"DMA     ", MALLOC_CAP_DMA},
      {"32BIT   ", MALLOC_CAP_32BIT},
    };
    for (auto& p : pools)
      Serial.printf("[memcap] %s free=%7u largest=%7u\n", p.name,
                    (unsigned)heap_caps_get_free_size(p.caps),
                    (unsigned)heap_caps_get_largest_free_block(p.caps));
    // 碎片形态：free_blocks 才是关键——同样是 50KB 空闲，散成 200 块和聚成 2 块
    // 是完全不同的两回事，而 free/largest 两个数都看不出这个区别。
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    Serial.printf("[memcap] 空闲 %u 字节散在 %u 个块里，已分配 %u 块；平均每个空闲块 %u 字节\n",
                  (unsigned)info.total_free_bytes, (unsigned)info.free_blocks,
                  (unsigned)info.allocated_blocks,
                  info.free_blocks ? (unsigned)(info.total_free_bytes / info.free_blocks) : 0);
  }
  else if (up == "GPS")  gnssPrintDiag();
  else if (up == "GNSS ON") {
    gnssStreamSet(true, 1);
  }
  else if (up.startsWith("GNSS ON ")) {
    int hz = cmd.substring(8).toInt();
    if (hz < 1) hz = 1;
    if (hz > 5) hz = 5;
    gnssStreamSet(true, hz);
  }
  else if (up == "GNSS OFF") {
    gnssStreamSet(false, 0);
  }
  else if (up == "IMU ON") {
    imuStreamSet(true, 50);
  }
  else if (up.startsWith("IMU ON ")) {
    String arg = cmd.substring(7);
    arg.trim();
    int hz = arg.toInt();
    if (hz < 10) hz = 10;
    if (hz > 100) hz = 100;
    imuStreamSet(true, hz);
  }
  else if (up == "IMU OFF") {
    imuStreamSet(false);
  }
  else if (up == "NMEA" || up.startsWith("NMEA ")) {
    int sec = (up == "NMEA") ? 3 : cmd.substring(5).toInt();
    if (sec < 1) sec = 1;
    if (sec > 30) sec = 30;
    gnssNmeaEcho((uint32_t)sec * 1000);
  }
  else if (up == "SCAN WIFI" || up == "WIFISCAN") {
    if (ridStreamIsActive()) {
      // Remote ID 流占着混杂模式和信道，这时候扫描会把它的信道/STA 状态搅乱。
      // 回数据面的 error 就是这条命令的回执，不再另发 ack（一条命令恰好一个回执）
      Serial.println("WSCAN {\"t\":\"error\",\"msg\":\"busy: RID stream on\"}");
    } else if (btStreamIsActive()) {
      // BLE 流开着时 Wi-Fi 是关的（BLE 控制器独占射频），同样用 error 作回执
      Serial.println("WSCAN {\"t\":\"error\",\"msg\":\"busy: BLE stream on\"}");
    } else {
      if (btHoldsHeap()) btReleaseForOtherApps();   // BLE 流停掉后只是挂起，Wi-Fi 要先拿回内存
      if (screen == SCREEN_PCMODE && up == "SCAN WIFI") {
        Serial.println("PCM {\"t\":\"ack\",\"cmd\":\"SCAN WIFI\"}");
      }
      wifiScanToSerial();   // PC 主导模式：扫描并把 AP 列表流式吐给 PC
    }
  }
  // 直跳，免得靠方向键数格子。跟 GOTO 一样是裸跳转、不 cleanup，理由见下面那段
  else if (up == "GNSSMAP") { warnRawJump(screen); screen = SCREEN_GNSS_MAP; gnssMapEnter(); dirty = true; }
  else if (up.startsWith("GOTO ")) {
    // 按 Screen 枚举序号直跳任意界面，调 UI 时配 SHOT 用（菜单索引会保留，靠方向键数格子不可靠）。
    // ⚠️ 只有下面那张白名单里的页面会调 enter()，其余的跳过去可能是个没初始化的空壳。
    // 这个口子是给调试用的，不是正经导航。
    int n = cmd.substring(5).toInt();
    if (n >= 0 && n < (int)SCREEN__COUNT) {
      warnRawJump(screen);
      screen = (Screen)n;
      // 顺手把"纯取数"那几页的 enter 也调上，否则跳过去是个没初始化的空壳，
      // 测不出真实的进入行为（之前排查 Planes 进入卡顿就卡在这儿）。
      // 会抢硬件的（LoRa 占 SPI、Spectrum 开 Mic、BLE/Ducky 起协议栈）故意不调，
      // 免得一条调试指令把外设状态搅乱。
      switch (screen) {
        case SCREEN_ADSB:    adsbEnter(); break;
        case SCREEN_SATS:    satsEnter(); break;
        case SCREEN_ROUTER:  routerEnter(); break;
        case SCREEN_GITHUB:  githubEnter(); break;
        case SCREEN_WEATHER: weatherEnter(); break;
        case SCREEN_LANSCAN: lanscanEnter(); break;
        case SCREEN_GNSS_MAP: gnssMapEnter(); break;
        case SCREEN_TYPHOON:
        case SCREEN_TYPHOON_TRACK: typhoonEnter(); break;
        case SCREEN_QUAKE:
        case SCREEN_QUAKE_MAP: quakeEnter(); break;
        case SCREEN_FX:
        case SCREEN_FX_DAYS: fxEnter(); break;
        case SCREEN_OKX: okxEnter(); break;
        case SCREEN_FILES:   curPath = "/"; loadDir(curPath); break;
        case SCREEN_READER_SHELF: readerEnter(); break;
        case SCREEN_WIFI_CHAN: wifiChanScan(); break;
        case SCREEN_NETPROBE: netprobeEnter(); break;
        case SCREEN_SSH:      sshEnter(); break;
        case SCREEN_WSNIFF:   wsniffEnter(); break;
        case SCREEN_WARDRIVE: wardriveEnter(); break;
        case SCREEN_ASTRO_SUN:
        case SCREEN_MOON:
        case SCREEN_ASTRO_TERM: astroEnter(); break;
        case SCREEN_COMPASS:
        case SCREEN_COMPASS_DETAIL: imuTare(); break;
        case SCREEN_SETTINGS: settingsIndex = 0; break;
        case SCREEN_HOTSPOT:
        case SCREEN_HOTSPOT_PW:
        case SCREEN_HOTSPOT_QR: break;
        case SCREEN_IR:      irEnter(); break;
        case SCREEN_DUCKY:   duckyEnter(); break;
        default: break;
      }
      dirty = true;
    }
    Serial.printf("[remote] GOTO %d\n", n);
  }
  else if (up == "SHOT") {
    // 把当前画布按 RGB565 原样吐出来（每行一条 hex），Mac 那头 tools/shot.py 拼成 PNG。
    // 屏幕才 240x135，整屏 64KB、hex 化 130KB，115200 下十几秒——用来核对布局够了。
    if (!canvasAvailable()) {
      Serial.println("SHOT unavailable: canvas released");
      return;
    }
    render();                                    // 先按当前状态画一帧，保证吐的是最新画面
    Serial.printf("[shot] %dx%d rgb565\n", SW, SH);
    for (int y = 0; y < SH; y++) {
      String line;
      line.reserve(SW * 4 + 8);
      char hx[5];
      for (int x = 0; x < SW; x++) {
        snprintf(hx, sizeof(hx), "%04X", cv.readPixel(x, y));
        line += hx;
      }
      Serial.printf("[shotrow] %d %s\n", y, line.c_str());
      delay(1);                                  // 别把 CDC 缓冲一次性灌爆
    }
    Serial.println("[shot] end");
  }
  else if (up == "SHOTRAW") {
    if (!canvasAvailable()) {
      Serial.println("SHOT unavailable: canvas released");
      return;
    }
    Serial.printf("[shot] %dx%d rgb565\n", SW, SH);
    for (int y = 0; y < SH; y++) {
      String line;
      line.reserve(SW * 4 + 8);
      char hx[5];
      for (int x = 0; x < SW; x++) {
        snprintf(hx, sizeof(hx), "%04X", cv.readPixel(x, y));
        line += hx;
      }
      Serial.printf("[shotrow] %d %s\n", y, line.c_str());
      delay(1);
    }
    Serial.println("[shot] end");
  }
  else if (up == "BOOTSND") {
    Serial.printf("[bootsnd] %s\n", bootSoundOn ? "ON" : "OFF");
  }
  else if (up == "BOOTSND ON") {
    bootSoundSet(true);
    Serial.println("[bootsnd] ON");
  }
  else if (up == "BOOTSND OFF") {
    bootSoundSet(false);
    Serial.println("[bootsnd] OFF");
  }
  else if (up == "MUTE") {
    volSet(0);
    dirty = true;
    Serial.println("[vol] 0% (MUTED)");
  }
  else if (up == "UNMUTE") {
    if (volPct == 0) volSet(60);
    else M5.Speaker.setVolume(volVal());
    dirty = true;
    Serial.printf("[vol] %d%%%s\n", volPct, debugOn ? " [DEBUG MUTED]" : "");
  }
  else if (up == "VOL") {
    Serial.printf("[vol] %d%% (hardware=%d)%s\n", volPct, volVal(), debugOn ? " [DEBUG MUTED]" : "");
  }
  else if (up.startsWith("VOL ")) {
    int v = cmd.substring(4).toInt();
    volSet(v);
    dirty = true;
    Serial.printf("[vol] set to %d%%%s\n", volPct, debugOn ? " [DEBUG MUTED]" : "");
  }
  else if (up == "BRIGHT") {
    Serial.printf("[bright] %d%% (raw=%d)\n", brightPct, brightVal());
  }
  else if (up.startsWith("BRIGHT ")) {
    int b = cmd.substring(7).toInt();
    brightSet(b);
    dirty = true;
    Serial.printf("[bright] set to %d%% (raw=%d)\n", brightPct, brightVal());
  }
  else if (up == "SLEEP") {
    Serial.printf("[sleep] idx=%d sec=%d (0=Never)\n", sleepOptIdx, SLEEP_OPTS[sleepOptIdx]);
  }
  else if (up.startsWith("SLEEP ")) {
    int val = cmd.substring(6).toInt();
    int targetIdx = -1;
    // 优先按秒数匹配（例如 SLEEP 0 对应 SLEEP_OPTS[4]==0，避免 0 被当作索引 0 导致设成 5 秒）
    for (int i = 0; i < SLEEP_OPT_COUNT; i++) {
      if (SLEEP_OPTS[i] == val) { targetIdx = i; break; }
    }
    if (targetIdx < 0 && val >= 0 && val < SLEEP_OPT_COUNT) {
      targetIdx = val;
    }
    if (targetIdx >= 0) {
      sleepSet(targetIdx);
      dirty = true;
      Serial.printf("[sleep] set to idx=%d sec=%d (0=Never)\n", sleepOptIdx, SLEEP_OPTS[sleepOptIdx]);
    } else {
      Serial.println("[sleep] invalid value (opts: 5, 15, 30, 60, 0 or idx 0..4)");
    }
  }
  else if (up == "DEBUG") {
    Serial.printf("[debug] %s%s\n", debugOn ? "ON" : "OFF", debugOn ? " (DEVICE MUTED)" : "");
  }
  else if (up == "DEBUG ON" || up == "DEBUG 1") {
    debugSet(true);
    bcTrailStartWatchdog();
    dirty = true;
    Serial.println("[debug] ON (DEBUG BAR ON, DEVICE MUTED)");
  }
  else if (up == "DEBUG OFF" || up == "DEBUG 0") {
    debugSet(false);
    dirty = true;
    Serial.println("[debug] OFF (AUDIO RESTORED)");
  }
  else if (up.startsWith("BOOTFRAME ")) {
    if (!canvasAvailable()) {
      Serial.println("SHOT unavailable: canvas released");
      return;
    }
    int t = up.substring(10).toInt();
    drawBootFrame(t);
    Serial.printf("[shot] %dx%d rgb565\n", SW, SH);
    for (int y = 0; y < SH; y++) {
      String line;
      line.reserve(SW * 4 + 8);
      char hx[5];
      for (int x = 0; x < SW; x++) {
        snprintf(hx, sizeof(hx), "%04X", cv.readPixel(x, y));
        line += hx;
      }
      Serial.printf("[shotrow] %d %s\n", y, line.c_str());
      delay(1);
    }
    Serial.println("[shot] end");
  }
  else if (up == "BOOTANIM") {
    if (!canvasAvailable()) {
      Serial.println("SHOT unavailable: canvas released");
      return;
    }
    bootAnim();
    Serial.printf("[shot] %dx%d rgb565\n", SW, SH);
    for (int y = 0; y < SH; y++) {
      String line;
      line.reserve(SW * 4 + 8);
      char hx[5];
      for (int x = 0; x < SW; x++) {
        snprintf(hx, sizeof(hx), "%04X", cv.readPixel(x, y));
        line += hx;
      }
      Serial.printf("[shotrow] %d %s\n", y, line.c_str());
      delay(1);
    }
    Serial.println("[shot] end");
  }
  else if (up == "BOOTPOST") {
    if (!canvasAvailable()) {
      Serial.println("SHOT unavailable: canvas released");
      return;
    }
    bootSelfTest();
    Serial.printf("[shot] %dx%d rgb565\n", SW, SH);
    for (int y = 0; y < SH; y++) {
      String line;
      line.reserve(SW * 4 + 8);
      char hx[5];
      for (int x = 0; x < SW; x++) {
        snprintf(hx, sizeof(hx), "%04X", cv.readPixel(x, y));
        line += hx;
      }
      Serial.printf("[shotrow] %d %s\n", y, line.c_str());
      delay(1);
    }
    Serial.println("[shot] end");
  }
  else if (up == "WIFI") {
    // 若 BLE 还在挂起状态并占着堆，必须先彻底释放，否则 Wi-Fi 与 BLE 并存会导致堆仅剩 ~1KB 且严重碎片化
    if (btHoldsHeap()) {
      btReleaseForOtherApps();
    }
    wifiKeeperResume();
    // 平时 Wi-Fi 是常开+自动重连的（见 wifi_net.cpp 的 bootWifiStart），用不着这条指令；
    // 它是给 WIFIOFF 之后做重连对照测试用的——WIFIOFF 会挂起看门狗，这条负责恢复看门狗
    // 并立刻用 NVS 里记住的凭据连一次，不用在 UI 里翻 Settings->WiFi->选网 三层菜单。
    String s, p;
    bool haveCreds = loadCreds(s, p);
    Serial.printf("[remote] WIFI: haveCreds=%d\n", haveCreds);
    if (haveCreds) connectWith(s.c_str(), p.c_str(), true);
    Serial.printf("[remote] WIFI: mac=%s ip=%s\n", WiFi.macAddress().c_str(), WiFi.localIP().toString().c_str());
    dirty = true;
  }
  else if (up == "WIFIOFF") {   // 配合 WIFI 指令做干净的断连/重连对照测试
    wifiKeeperSuspend();        // 否则看门狗下一帧就把它拉回来，等于关不掉
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[remote] WIFIOFF: 看门狗已挂起，发 WIFI 恢复");
    dirty = true;
  }
  else if (up == "RANDMAC") {
    // 排查"是不是按MAC地址白名单判定认证状态"用——手动给STA换一个随机MAC（本地管理位置1，
    // 避免跟真实厂商MAC冲突），下次WIFI重连会以这个新MAC入网，配合WIFIOFF看DHCP给不给新IP、
    // 443状态跟不跟着变。
    if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
    uint8_t mac[6];
    mac[0] = 0x02;   // 本地管理位(bit1)=1，组播位(bit0)=0，避免撞真实厂商前缀
    for (int i = 1; i < 6; i++) mac[i] = (uint8_t)esp_random();
    esp_wifi_set_mac(WIFI_IF_STA, mac);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[remote] RANDMAC: set to %s\n", buf);
  }
  else if (up.startsWith("RIDSCAN")) {
    // Remote ID：混杂模式跳信道 1-13，解码标准 ODID（解析器见 odid.cpp），
    // 同时把管理帧里的 vendor IE(tag 221) 原始字节也全打出来——解不出来时得能看见原文。
    //   FA:0B:BC = ASTM F3411 / ASD-STAN 标准 Remote ID（oui_type 应为 0x0D）
    //   26:37:12 = DJI 私有 DroneID
    // ⚠️ ESP32-S3 只有 2.4GHz。无人机图传要是跑在 5.8，这里物理上收不到。
    // RIDSCAN [秒] [信道]，给了信道就蹲死不跳频（查 NAN 必须蹲 ch6）
    int secs = 20, chan = 0;
    String rest = cmd.substring(7); rest.trim();
    if (rest.length()) {
      int sp = rest.indexOf(' ');
      int v = (sp < 0 ? rest : rest.substring(0, sp)).toInt();
      if (v > 0 && v <= 300) secs = v;
      if (sp >= 0) { int c = rest.substring(sp + 1).toInt(); if (c >= 1 && c <= 13) chan = c; }
    }
    ridScanRun(secs, chan);
  }
  else if (up.startsWith("RFSCAN")) {
    int secs = 20;
    String rest = cmd.substring(6); rest.trim();
    if (rest.length()) { int v = rest.toInt(); if (v > 0 && v <= 300) secs = v; }   // 跟 RIDSCAN 一样封顶：整段是阻塞的
    rfScanRun(secs);
  }
  else if (up == "RIDFAKE") {
    // 把真机抓到的两帧灌进 Drone ID 页，用来在没无人机时核对排版（见 ridapp.h）
    ridAppInjectSample();
    Serial.println("[rid] injected 2 real sample frames");
    dirty = true;
  }
  else if (up == "RIDREC") {
    ridAppToggleRecord();
    Serial.printf("[rid] recording %s\n", ridAppIsRecording() ? "STARTED" : "STOPPED");
    dirty = true;
  }
  else if (up.startsWith("BTEXT")) {
    // BLE 扩展广播（BT5 Long Range / Coded PHY）取样。BTDUMP 那套 API 看不见这一类。
    int secs = 15;
    if (cmd.length() > 6) { int v = cmd.substring(6).toInt(); if (v > 0 && v <= 120) secs = v; }
    btExtScanRun(secs);
  }
  else if (up.startsWith("BTDUMP")) {
    // BLE 取样：扫一遍，把每个设备的 Service Data / Manufacturer Data 原样打到串口。
    // 屏幕上一次只看得到一个设备，要横扫一片时串口才够用。
    int secs = 6;
    if (cmd.length() > 7) { int v = cmd.substring(7).toInt(); if (v > 0 && v <= 60) secs = v; }
    btDumpRun(secs);
  }
  else if (up.startsWith("CAT ")) {
    // 远程读 SD 卡文件内容——主要给 netprobe.log/dnsfuzz.log 用，不用拔卡就能看自动巡检进展
    String path = cmd.substring(4);
    path.trim();
    if (!path.startsWith("/")) path = "/" + path;
    if (!sdMounted) {
      Serial.println("[cat] SD not mounted");
    } else {
      File f = SD.open(path, FILE_READ);
      if (!f) {
        Serial.printf("[cat] %s: open failed\n", path.c_str());
      } else {
        Serial.printf("[cat] %s (%u bytes)\n", path.c_str(), (unsigned)f.size());
        while (f.available()) Serial.write(f.read());
        Serial.println();
        f.close();
      }
    }
  }
  else if (up.startsWith("PUT ")) {
    String rest = cmd.substring(4);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp > 0) {
      String path = rest.substring(0, sp);
      if (!path.startsWith("/")) path = "/" + path;
      String content = rest.substring(sp + 1);
      if (!sdMounted) {
        Serial.println("[put] SD not mounted");
      } else {
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash > 0) {
          String dir = path.substring(0, lastSlash);
          if (!SD.exists(dir)) SD.mkdir(dir);
        }
        File f = SD.open(path, FILE_WRITE);
        if (f) {
          f.print(content);
          f.close();
          Serial.printf("[put] wrote %d bytes to %s\n", (int)content.length(), path.c_str());
        } else {
          Serial.printf("[put] open %s failed\n", path.c_str());
        }
      }
    }
  }
  else if (up.startsWith("MKDIR ")) {
    String dir = cmd.substring(6); dir.trim();
    if (!dir.startsWith("/")) dir = "/" + dir;
    if (!sdMounted) Serial.println("[mkdir] SD not mounted");
    else { bool ok = SD.mkdir(dir); Serial.printf("[mkdir] %s: %s\n", dir.c_str(), ok ? "ok" : "fail"); }
  }
  else if (up.startsWith("SETMAC ")) {
    // 设指定STA MAC(MAC欺骗/授权任意MAC测试用): SETMAC aa:bb:cc:dd:ee:ff ；设完发 WIFI 重连生效
    if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
    String a = cmd.substring(7); a.trim();
    unsigned v[6];
    if (sscanf(a.c_str(), "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) == 6) {
      uint8_t mac[6]; for (int i=0;i<6;i++) mac[i]=(uint8_t)v[i];
      esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
      Serial.printf("[remote] SETMAC: %02X:%02X:%02X:%02X:%02X:%02X rc=%d (发 WIFI 重连生效)\n",
                    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], (int)e);
    } else {
      Serial.println("[remote] SETMAC: bad mac, use aa:bb:cc:dd:ee:ff");
    }
  }
  else if (up.startsWith("HGET ")) {
    // 原始HTTP GET(不跟随重定向,便于看302授权响应): HGET <host> <path>
    // 关键: 请求的L2源MAC=本机当前STA MAC, 而path里可塞任意terminalMac, 用于判定授权按哪个MAC生效
    String rest = cmd.substring(5); rest.trim();
    int sp = rest.indexOf(' ');
    String host = sp < 0 ? rest : rest.substring(0, sp);
    String path = sp < 0 ? String("/") : rest.substring(sp + 1);
    WiFiClient c;
    Serial.printf("[hget] GET %s%s  src-mac=%s\n", host.c_str(), path.c_str(), WiFi.macAddress().c_str());
    if (!c.connect(host.c_str(), 80)) { Serial.println("[hget] connect fail"); }
    else {
      c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path.c_str(), host.c_str());
      uint32_t t0 = millis(); int total = 0;
      Serial.println("[hget] <<<RESP");
      while ((c.connected() || c.available()) && millis() - t0 < 10000 && total < 6000) {
        if (!c.available()) { delay(5); continue; }
        int b = c.read();
        if (b < 0) break;
        Serial.write((uint8_t)b);
        total++;
      }
      c.stop();
      Serial.printf("\n[hget] RESP>>> %d bytes\n", total);
      Serial.println("[hget] done");
    }
  }
  else if (up.startsWith("HPOST ")) {
    // 原始HTTP POST(带表单body,带Referer过ZTE的CSRF/referer检查): HPOST <host> <path> <body>
    String rest = cmd.substring(6); rest.trim();
    int s1 = rest.indexOf(' ');
    String host = s1 < 0 ? rest : rest.substring(0, s1);
    String r2   = s1 < 0 ? String("") : rest.substring(s1 + 1);
    int s2 = r2.indexOf(' ');
    String path = s2 < 0 ? r2 : r2.substring(0, s2);
    String body = s2 < 0 ? String("") : r2.substring(s2 + 1);
    WiFiClient c;
    Serial.printf("[hpost] POST %s%s body=%s src-mac=%s\n", host.c_str(), path.c_str(), body.c_str(), WiFi.macAddress().c_str());
    if (!c.connect(host.c_str(), 80)) { Serial.println("[hpost] connect fail"); }
    else {
      c.printf("POST %s HTTP/1.1\r\nHost: %s\r\nReferer: http://%s/index.html\r\n"
               "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
               path.c_str(), host.c_str(), host.c_str(), body.length(), body.c_str());
      uint32_t t0 = millis(); int total = 0;
      Serial.println("[hpost] <<<RESP");
      while ((c.connected() || c.available()) && millis() - t0 < 10000 && total < 6000) {
        if (!c.available()) { delay(5); continue; }
        int b = c.read(); if (b < 0) break;
        Serial.write((uint8_t)b); total++;
      }
      c.stop();
      Serial.printf("\n[hpost] RESP>>> %d bytes\n[hpost] done\n", total);
    }
  }
  else if (up.startsWith("TCPHEX ")) {
    // 原始TCP探测(打未知二进制服务如9000): TCPHEX <host> <port> [hexpayload]
    // 连接->(可选)发送hex解码后的字节->读回显(hex),用于摸私有协议有没有banner/对某magic有无响应
    String rest = cmd.substring(7); rest.trim();
    int s1 = rest.indexOf(' ');
    if (s1 < 0) { Serial.println("[tcp] usage: TCPHEX <host> <port> [hexpayload]"); return; }
    String host = rest.substring(0, s1);
    String r2 = rest.substring(s1 + 1);
    int s2 = r2.indexOf(' ');
    int port = (s2 < 0 ? r2 : r2.substring(0, s2)).toInt();
    if (port <= 0 || port > 65535) { Serial.println("[tcp] usage: TCPHEX <host> <port> [hexpayload]"); return; }
    String hex = s2 < 0 ? String("") : r2.substring(s2 + 1);
    hex.replace(" ", "");
    WiFiClient c;
    Serial.printf("[tcp] connect %s:%d payload=%s\n", host.c_str(), port, hex.c_str());
    if (!c.connect(host.c_str(), port)) { Serial.println("[tcp] connect fail"); }
    else {
      for (int i = 0; i + 1 < (int)hex.length(); i += 2) {
        uint8_t b = (uint8_t) strtol(hex.substring(i, i + 2).c_str(), nullptr, 16);
        c.write(b);
      }
      uint32_t t0 = millis(); int n = 0;
      String asc = "";
      Serial.print("[tcp] resp hex:");
      while ((c.connected() || c.available()) && millis() - t0 < 4000 && n < 256) {
        if (!c.available()) { delay(5); continue; }
        int b = c.read(); if (b < 0) break;
        Serial.printf(" %02X", b);
        asc += (b >= 32 && b < 127) ? (char)b : '.';
        n++;
      }
      Serial.printf("\n[tcp] resp asc: %s\n[tcp] %d bytes\n", asc.c_str(), n);
      c.stop();
    }
  }
  else if (up.startsWith("FONT")) {
    String arg = cmd.substring(4); arg.trim();
    if (arg == "12") readerSetFontSize(FONT_SIZE_12);
    else if (arg == "14") readerSetFontSize(FONT_SIZE_14);
    else if (arg == "16") readerSetFontSize(FONT_SIZE_16);
    else readerCycleFontSize();
    Serial.printf("[font] current size: %s\n", FONT_CONFIGS[readerFontSize].label);
    dirty = true;
  }
  else if (up == "HELP" || up == "?") {
    Serial.println("[remote] serial debug channel commands:");
    Serial.println("  单字符      -> 原样当按键喂给UI (; . ` 回车等导航键)");
    Serial.println("  ENTER/BACK/UP/DOWN/MENU -> 对应导航键别名");
    Serial.println("  BOOTANIM                -> 播放赛博朋克开机动画并输出截图");
    Serial.println("  BOOTPOST                -> 运行硬件自检(POST AUDIT)并输出截图");
    Serial.println("  BOOTFRAME [t]           -> 步进绘制开机动画第t毫秒画面并截屏");
    Serial.println("  BOOTSND [ON|OFF]        -> 查询或配置开机音效偏好(存NVS)");
    Serial.println("  SHOT / SHOTRAW          -> 串口截屏(RGB565 hex,配 tools/shot.py 存PNG)");
    Serial.println("  LORA / WIFICHAN / RADIO -> 直跳对应 App");
    Serial.println("  BTSCAN / BTDEV / BTRADAR-> 直跳蓝牙扫描 / 详情 / 寻物雷达");
    Serial.println("  NETPROBE / DNSFUZZ      -> 跳转到网络巡检/DNS Fuzz模式并开始跑");
    Serial.println("  PROBE host[:port]       -> 一次性TCP连通性探测,端口默认443,结果只走Serial");
    Serial.println("  STAT                    -> 只读状态快照: WiFi/堆余量/最大连续块/loop栈余量(不会触发连接)");
    Serial.println("  RAMLOG                  -> 打开机各阶段内存轨迹(带逐步Δ,查'静态RAM涨了是谁涨的')");
    Serial.println("  TRAIL                   -> 打印面包屑轨迹(查卡死;开机也会自动回放上一轮)");
    Serial.println("  TRAIL MARK n            -> 手动埋一个字节(0..255或0x..),用来自证'软复位不丢'");
    Serial.println("  BATT [n]                -> 连采n次电池电压(默认64),打出分布+内部状态");
    Serial.println("  BATTLOG ON|OFF          -> 电池曲线写SD(存NVS,重启保持;拔线前打开)");
    Serial.println("  GPS                     -> 打GNSS收数诊断(字节数/校验和/静默时长/定位)");
    Serial.println("  GNSS ON [hz]|OFF        -> GNSS串口流(PC主导模式: 1..5Hz定位与卫星列表,GNSS 前缀)");
    Serial.println("  IMU ON [hz]|OFF         -> IMU串口流(PC主导模式: 10..100Hz六轴加速度与角速度,IMU 前缀)");
    Serial.println("  LORA SNIFF <MHz> [...]  -> LoRa嗅探流(PC主导模式: 原始帧hex与信号质量,LORA 前缀)");
    Serial.println("  LORA OFF                -> 关闭LoRa嗅探流");
    Serial.println("  BLE ON|OFF              -> BLE 广播扫描流(PC主导模式: 1M+Coded PHY, 含 Remote ID 解码, BLE 前缀)");
    Serial.println("  RID ON [信道]|OFF       -> Remote ID 无人机嗅探流(PC主导模式: 2.4G beacon/NAN, RID 前缀; 给信道则蹲守)");
    Serial.println("  NMEA [sec]              -> 原样回显GNSS模块的NMEA语句(默认3秒,最多30秒,NMEA 前缀)");
    Serial.println("  PCMODE                  -> 进入PC主导模式(释放画布,极简状态屏,PCM 握手,也可在Settings手动进入)");
    Serial.println("  PCEXIT                  -> 退出PC主导模式(恢复画布,按来源返回Settings或主菜单)");
    Serial.println("  CAPS                    -> 查询PC主导模式能力列表(PCM 前缀)");
    Serial.println("  SCAN WIFI               -> 扫描周边AP(WIFISCAN别名,PCMODE内先回ack再出WSCAN)");
    Serial.println("  WIFISCAN                -> 扫描周边AP,按JSON行(WSCAN 前缀)流式吐出,配PC端bridge用");
    Serial.println("  GNSSMAP                 -> 直接跳到GNSS地图页");
    Serial.println("  GOTO n                  -> 按Screen枚举序号直跳任意界面(调试用,不cleanup)");
    Serial.println("  WIFI                    -> 用NVS里记住的凭据直连(不进UI)");
    Serial.println("  WIFIOFF                 -> 断开并关闭WiFi(配合WIFI做干净的重连测试)");
    Serial.println("  RANDMAC                 -> STA MAC换成随机值(本地管理位),配合WIFI重连生效");
    Serial.println("  SETMAC aa:bb:cc:dd:ee:ff -> STA MAC设为指定值,配合WIFI重连生效");
    Serial.println("  LS [path]               -> 列SD目录(名字+字节数),默认根目录");
    Serial.println("  PUT path content        -> 写内容到SD卡文件(目录自动建)");
    Serial.println("  MKDIR path              -> 在SD卡上创建目录");
    Serial.println("  CAT path                -> 读SD卡文件内容(不带开头/会自动补上)");
    Serial.println("  FXDUMP                  -> 打印汇率接口的原始响应(解析器没对过真响应时用这个看形状)");
    Serial.println("  HGET host path          -> 原始HTTP GET,不跟随重定向");
    Serial.println("  HPOST host path body    -> 原始HTTP POST(表单body,带Referer)");
    Serial.println("  TCPHEX host port [hex]  -> 连接后可选发送hex解码字节,读回显(hex+ascii)");
    Serial.println("  DEBUG [ON|OFF]          -> 开关底部调试条与全机静音(调试模式下全机静音)");
    Serial.println("  VOL [0-100]             -> 查询或设置系统音量(存NVS)");
    Serial.println("  MUTE / UNMUTE           -> 一键静音 / 恢复音量(存NVS)");
    Serial.println("  BRIGHT [5-100]          -> 查询或设置屏幕亮度(存NVS)");
    Serial.println("  SLEEP [sec|idx]         -> 查询或设置休眠超时(5/15/30/60/0, 存NVS)");
    Serial.println("  FONT [12|14|16]         -> 切换小说阅读器字号(12/14/16px)");
    Serial.println("  HELP / ?                -> 显示本帮助");
    Serial.println("  其余原样文本            -> 逐字符当键盘输入喂给当前界面");
  }
  else {
    for (size_t i = 0; i < cmd.length(); i++) handleKey(cmd[i]);   // 其余当逐字符文本输入
    if (screen == SCREEN_SSH && sshIsTerminalActive()) handleKey('\n');
  }
  if (!(screen == SCREEN_SSH && sshIsTerminalActive())) {
    Serial.printf("[remote] cmd=\"%s\" -> screen=%d\n", cmd.c_str(), (int)screen);
  }
}

void serialCmdPoll() {
  // 逐字节攒，收到换行才执行。以前用的是 readStringUntil('\n')——它在只收到半行时
  // 会一直等到默认 1s 超时，交互式手敲指令的话每敲一个字符就卡主循环 1s。
  static char buf[192];
  static size_t len = 0;
  static bool  discard = false;       // 本行已超长，丢到行尾为止
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      bool skip = discard;
      discard = false;
      size_t n = len; len = 0;
      if (!skip && n) { buf[n] = 0; handleSerialCmd(String(buf)); }
      return;                         // 一次只执行一条，剩下的下一轮 loop 再取
    }
    if (discard) continue;
    // ⚠️ 这里以前只是 len = 0，于是超长行的后半截会从头累积、被当成一条新指令执行。
    // 必须置 discard 一直丢到换行为止，光清 len 是不够的。
    // 已知代价：一次性灌 300+ 字节会把 USB-CDC 的接收环冲掉一部分，要是丢的正好是换行，
    // 就会连带吃掉紧跟其后的那一条指令（实测就是这样）。这是分隔符丢失的固有后果，
    // 不是解析器的问题——再发一次即可。
    if (len < sizeof(buf) - 1) buf[len++] = c;
    else { discard = true; len = 0; }
  }
}

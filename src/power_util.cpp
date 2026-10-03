#include "power_util.h"
#include <M5Unified.h>
#include "globals.h"   // loadUChar/saveUChar（NVS 读写 helper）
#include "battlog.h"   // 曲线日志（板级依赖都在那边，别搬回来）

// 采样/判定参数（都比较保守，避免 ADC 噪声导致抖动）：
static const uint32_t SAMPLE_MS   = 2000;   // 每 2s 采一次电压
static const uint32_t WINDOW_MS   = 12000;  // 每 12s 结算一次斜率
static const float    RISE_MV     = 8.0f;   // 窗口内涨超 +8mV → 判为充电
static const float    FALL_MV     = 8.0f;   // 窗口内跌超 -8mV → 判为放电
static const float    EMA_ALPHA   = 0.2f;   // 电压指数平滑系数
// 满电涓流阶段电压本来就快走平了，拔掉充电器那一下的回落幅度可能远小于 FALL_MV，
// 于是"走平保持原状态"的迟滞会让 charging 卡在 true 上不再退出（实测过的 bug：明明
// 没插着还显示在充电）。加一道超时兜底：充电态太久没再出现上升沿就强制退出，见 .cpp。
static const uint32_t STILL_CHARGING_TIMEOUT_MS = 90000;
// 斜率法有个天生测不出来的情况：**开机前就已经插着**。那时电压一直平在恒压平台上，
// 从来不出现上升沿，charging 会一直是 false（实测：插着 USB 的板子显示 "on battery"）。
// 补一条绝对判据：TP4057 恒压档把电池节点顶在 4.2V，而一节在放电的电芯扛着 ESP32
// （Wi-Fi 常开，上百 mA）根本不可能长时间停在 4.18V 以上。所以"高位走平"=插着。
// 「插没插」的判据。⚠️ 原来是 4180，来自"TP4057 恒压档把电池节点顶在 4.2V"这个推理——
// 但实测插着电静止时只有 4116~4150 mV，**比 4180 低**，所以这条规则从来没触发过。
//
// 实测数据给了一条干净得多的分界线：
//     离电全程最高  3634 mV
//     插电观测范围  4116 ~ 4424 mV
// 中间 480mV 是空的。**但别急着在中间画线**——那条间隔是重负载下的：那次放电 94.6%
// 的时间停在 Router 页（Wi-Fi 全程连着 + 持续轮询），压降大，所以离电电压被压到 3.63V。
// 负载轻的时候（熄屏、Wi-Fi 关掉）同一块满电池会读到接近开路电压的值，间隔可能整个闭合。
// 我一度把阈值设成 3800，powertest 立刻抓到反例："拔掉回到 3.90V"被判成充电。
//
// 所以只把它当**高置信度的兜底**，取在刚好能覆盖实测插电静止值(4116~4150)的下沿：
// 4050 会在插着时可靠触发（原来的 4180 比实测值还高，从来没触发过），而离电要读到 4050
// 以上得是"满电 + 极轻负载"，那种情况下报成充电的代价也不大（不会显示成快没电）。
//
// ⚠️ 这仍然是启发式，不是判据。真要分清楚，缺的是**一次轻负载放电**的数据：
//    BATTLOG ON -> 拔线 -> 停在 Clock 页（Wi-Fi 关掉）跑到关机 -> 看那条曲线的上沿到哪。
static const float PLUGGED_MV = 4050.0f;

static float    ema       = -1.0f;   // 平滑后的电压
static float    windowRef = -1.0f;   // 上个结算点的电压，用来算斜率
static uint32_t lastSample = 0;
static uint32_t lastWindow = 0;
static uint32_t lastRiseMs = 0;      // 最近一次判定为"在涨"的时刻
static bool     charging   = false;

static void levelTick(uint32_t now);

// 取 9 次读数的中位数。⚠️ 不是为了降噪——2026-08-26 实测 ADC 本身 σ≈2mV、量化步长 2mV，
// 平均不出更多精度。这里要的是**抗离群**：偶尔会蹦出比中位高 10 多 mV 的读数
// （多半是采样瞬间撞上一次射频发射），中位数一票否决，而 EMA 会把它摊进后面十几个采样。
// 最近一次采样窗口内的峰峰值(mV)。-1 = 还没采过。
// ⚠️ 这个量本身就是信号，不只是噪声：TP4057 在快充阶段斩波，端电压抖得很厉害；
//    充满转恒压/涓流之后就安静了。2026-08-26 实测两种状态毫不重叠：
//        插着·电池满    峰峰 16~52 mV   (σ 3.4~7.6)
//        插着·正在快充  峰峰 238~300 mV (σ 72~90)
//    差一个数量级。所以它能回答"充满了没"——而这块板子没有电流传感、没有充电状态脚，
//    本来是答不了的。也正是"插着但已充满"这个第三态需要的判据。
static int lastSpread = -1;
int powerBatterySpread() { return lastSpread; }

// 量"采样窗峰峰"。⚠️ 必须**拉开采样间隔**才量得到：上面 readMv() 那 9 次是背靠背的，
// 窗口零点几毫秒，只看得见高频噪声。实测纹波随窗口长度增长到 32~64ms 才饱和：
//     8ms->54mV   16ms->118mV   32ms->248mV   64ms->310mV   256ms->332mV（快充中）
// 而满电时 64ms 窗口只有 16~18mV。取 16 次 × 2ms = 32ms：足够分辨（248 vs ≤18），
// 又不至于每次采样都在主循环里堵 64ms。
static int measureSpread() {
  int lo = 99999, hi = -1;
  for (int i = 0; i < 16; i++) {
    int mv = M5.Power.getBatteryVoltage();
    if (mv > 0) { if (mv < lo) lo = mv; if (mv > hi) hi = mv; }
    delay(2);
  }
  return hi < 0 ? -1 : hi - lo;
}

static int readMv() {
  int v[9], n = 0;
  for (int i = 0; i < 9; i++) {
    int mv = M5.Power.getBatteryVoltage();
    if (mv > 0) v[n++] = mv;
  }
  if (n == 0) return -1;
  for (int i = 1; i < n; i++) { int x = v[i], j = i - 1; while (j >= 0 && v[j] > x) { v[j+1] = v[j]; j--; } v[j+1] = x; }
  return v[n / 2];
}

void powerUpdate() {
  uint32_t now = millis();
  if (now - lastSample < SAMPLE_MS) return;
  lastSample = now;

  int mv = readMv();                         // 9 次取中位，抗离群
  if (mv <= 0) return;

  ema = (ema < 0) ? mv : (ema * (1.0f - EMA_ALPHA) + mv * EMA_ALPHA);

  // 纹波每 5 次采样（10 秒）量一次就够——它是"充没充满"这种慢变量，而每次要占 32ms，
  // 每 2 秒都量的话主循环会规律性打嗝。
  static int spreadTick = 0;
  if (spreadTick++ % 5 == 0) lastSpread = measureSpread();

  battLogTick(now, mv, ema, powerBatteryLevel(), charging, lastSpread);

  if (windowRef < 0) { windowRef = ema; lastWindow = now; lastRiseMs = now; return; }

  if (now - lastWindow >= WINDOW_MS) {
    float delta = ema - windowRef;
    // ⚠️ 2026-08-26 把顺序倒过来了：**绝对高位判据必须排第一**。
    //
    // 原来是"先看明显在跌"，注释里的理由是"满电拔线能立刻退出充电态，不会被高位走平
    // 按住不放（刚拔线时电压还没掉下 4.18）"。那个担心建立在 PLUGGED_MV=4180 上，
    // 而实测放电曲线显示拔线后电压**立刻**掉到 3.63V 附近——根本不存在"还没掉下来"
    // 的窗口，所以那条理由不成立了。
    //
    // 反过来，把斜率排在前面有个实测到的真问题：充电时电压噪声极大
    // （实测 σ≈90mV、峰峰 300mV，是静止时的 30 倍，TP4057 在斩波），12 秒窗口的
    // delta 会随机为负，于是"明显在跌"被误触发——屏幕上表现为插着电却显示没在充。
    // 而高位(>=4050)这件事噪声翻不过来：离电从没观测到过这么高的读数。
    if (ema >= PLUGGED_MV) {
      charging = true;                       // 高位 → 插着（含"已充满的恒压/涓流平台"）
      lastRiseMs = now;
    } else if (delta < -FALL_MV) {
      charging = false;                      // 电压在降 → 放电
    } else if (delta > RISE_MV) {
      charging = true;                       // 电压在升 → 充电
      lastRiseMs = now;
    } else if (charging && now - lastRiseMs > STILL_CHARGING_TIMEOUT_MS) {
      charging = false;                      // 走平走太久了，判定已经拔了
    }
    // 其余走平情况：保持原状态（迟滞），短暂的涓流平台不会来回跳
    windowRef = ema;
    lastWindow = now;
  }

  levelTick(now);        // 电量状态的唯一推进点，见文件下半部分
}

bool powerCharging() { return charging; }

// ---- 电量百分比：实测重建的查表 ----
//
// ⚠️ 2026-08-26 整表重做。旧表是照**教科书 LiPo 开路曲线**（4.2V→3.3V）填的，
// 而这块板子离电时的端电压根本不在那个区间。BATTLOG 记了一次完整放电（4.13 小时、
// 1385 个采样点、跑到自动关机）之后才看清：
//
//     离电全程最高电压   3634 mV      <- 满电！不是 4.2V
//     关机时             2882 mV
//     插着电时读到       4116 ~ 4424 mV
//
// 也就是说真实工作区间(3.63→2.88V)整个落在旧表的下半段。后果是灾难性的：
//
//     真实剩余 100% 的电压(3616mV)，旧表报 8%
//     真实剩余  50%          (3470)  旧表报 4%
//     真实剩余  10%          (3254)  旧表报 0%   <- 还能再跑 25 分钟
//
// 新表直接由那次放电重建：把"剩余运行时间的百分比"当作 SoC，取每个刻度对应的电压
// （5 点滑动中值去毛刺，再强制单调）。
//
// ⚠️ 三条必须知道的局限：
//   1. **这是重负载下的曲线**。那次放电 94.6% 的时间停在 Router 页（Wi-Fi 全程连着、
//      持续轮询），是全项目最费电的用法。负载轻时压降小，同样的剩余量会读到更高电压，
//      于是这张表会**偏乐观**。
//   2. 单台设备、单块电池、一次放电。换电池或电池老化之后需要重测。
//   3. 量的是"还能跑多久的占比"，不是库仑量。对用户来说前者其实更有用，但别拿它当容量。
//
// 重测方法：串口 BATTLOG ON（存 NVS，重启保持）→ 拔线用到自动关机 → 插回来
// LS /battlog 找那个最大的文件 → CAT 拉出来重跑一遍上面的拟合。
struct LevelPoint { float mv; uint8_t pct; };
static const LevelPoint CURVE[] = {
  {3616, 100}, {3580, 90}, {3550, 80}, {3538, 70},
  {3500, 60},  {3470, 50}, {3436, 40}, {3390, 30},
  {3330, 20},  {3254, 10}, {3158, 5},  {3028, 2}, {2906, 0},
};
static const int CURVE_N = sizeof(CURVE) / sizeof(CURVE[0]);

static uint8_t voltageToPercent(float mv) {
  if (mv >= CURVE[0].mv) return 100;
  if (mv <= CURVE[CURVE_N - 1].mv) return 0;
  for (int i = 1; i < CURVE_N; i++) {
    if (mv >= CURVE[i].mv) {
      float span = CURVE[i - 1].mv - CURVE[i].mv;
      float t = (mv - CURVE[i].mv) / span;
      float pct = CURVE[i].pct + t * (CURVE[i - 1].pct - CURVE[i].pct);
      return (uint8_t)(pct + 0.5f);
    }
  }
  return 0;
}

// 开机瞬间屏幕/WiFi/SD卡上电会让电压有一次明显下沉，这几秒内查表会显示"几乎没电"。
// 开机头 BOOT_SETTLE_MS 内不采信实时电压，先用上次关机前存的电量顶着（没存过就给个
// 不吓人的默认值），过了这段窗口再切到"按当前平滑电压查表"的正常模式。
static const uint32_t BOOT_SETTLE_MS = 4000;
static const uint8_t  DEFAULT_PCT_NO_HISTORY = 60;   // 从没存过数据时的兜底显示值

static uint8_t  lastKnownPct = 255;   // 255=还没从NVS读过
static uint8_t  lastPersistedPct = 255;
static uint32_t lastPersistMs = 0;
static const uint32_t PERSIST_MIN_INTERVAL_MS = 300000; // 变化后至少 5 分钟写一次，减少 flash 磨损
static const uint32_t PERSIST_LOW_INTERVAL_MS = 30000;  // 低电量(<=15%)时 30 秒记录

// key 从 "lastpct" 换成 "lastpct2" 是**故意的**：旧版本只要插着电就会把查表算出来的
// ~100% 持久化下来（那时端电压被充电 IC 顶在 4.2V），所以旧 key 里存的值构造上就是虚高的，
// 不能拿来当新逻辑的充电基准。换个 key 等于一次性作废那些脏数据，回落到默认值重新开始。
static const char* PCT_KEY = "lastpct2";

static uint8_t loadLastKnownPct() {
  uint8_t p = loadUChar("power", PCT_KEY, DEFAULT_PCT_NO_HISTORY);
  return (p <= 100) ? p : DEFAULT_PCT_NO_HISTORY;
}
static void persistPct(uint8_t pct, uint32_t now) {
  if (pct == lastPersistedPct) return; // 电量未变不写，避免每 30 秒盲写 flash
  uint32_t minInterval = (pct <= 15) ? PERSIST_LOW_INTERVAL_MS : PERSIST_MIN_INTERVAL_MS;
  if (lastPersistedPct != 255 && now - lastPersistMs < minInterval) return;
  lastPersistMs = now;
  lastPersistedPct = pct;
  saveUChar("power", PCT_KEY, pct);
}

// ---- 充电时的电量：查表在这时候是失效的 ----
// TP4057 充电时把电池节点顶到恒压 4.2V，端电压**不再反映剩余容量**——一块空电池插上几分钟
// 也会读到 4.1V+，查表就是 100%。实测：插着电显示 4.21V/100%，拔掉开机只有 3.7V(约13%)。
// 而这块板子没有电流传感（TP4057 的状态脚也没接到主控），充电时的真实剩余容量在硬件层面
// 就是测不出来的。所以充电期间改成推算：以"最后一次离电测到的百分比"为基准，按规格估一个
// 上涨速率往上爬，封顶 99%（没有电流就判不出涓流结束，不敢报 100%）。拔掉的那一刻立即恢复
// 真实测量——那时读数是准的，宁可跳一下也不要继续编一个好看的数字。
//
// ⚠️ 这套状态必须由 powerUpdate() 推进，不能放在 powerBatteryLevel() 里：后者是调用方
// （顶栏图标/LED/电池页）按需调的，屏幕不重绘时可能几分钟才调一次。第一版就是放在那里，
// 桌面测试台一眼看出来——每次调用间隔都超过会话宽限期，于是每次都被当成新会话重置，
// 推算值永远停在基准上不动。
static const float   CHARGE_PCT_PER_MIN = 0.5f;   // 1750mAh 配典型充电电流，满充约 3 小时
static const uint8_t CHARGE_CAP_PCT     = 99;
// 充电判定偶尔会抖一下（CC 段某个 12s 窗口正好走平）。抖动期间既不推进推算、也不拿被充电
// 抬高的电压去更新基准，否则基准会一路虚高。只有真正离开够久才结束会话。
static const uint32_t CHARGE_SESSION_GRACE_MS = 60000;

static float    chargeEst    = 0.0f;   // 本次充电会话的推算百分比
static bool     chargeActive = false;  // 是否处在一个充电会话里
static uint32_t chargeTickMs = 0;      // 上次推进推算值的时刻
static uint32_t chargeSeenMs = 0;      // 最近一次判定"在充电"的时刻

static void ensureLastKnownLoaded() {
  if (lastKnownPct == 255) {
    lastKnownPct = loadLastKnownPct();   // 只读一次 NVS
    lastPersistedPct = lastKnownPct;
  }
}

// 由 powerUpdate() 每个采样节拍调一次，是电量状态的唯一推进点
static void levelTick(uint32_t now) {
  ensureLastKnownLoaded();
  if (now < BOOT_SETTLE_MS || ema < 0) return;

  if (charging) {
    if (!chargeActive) { chargeEst = (float)lastKnownPct; chargeActive = true; }
    else               { chargeEst += (now - chargeTickMs) / 60000.0f * CHARGE_PCT_PER_MIN; }
    if (chargeEst > CHARGE_CAP_PCT) chargeEst = CHARGE_CAP_PCT;
    chargeTickMs = now;
    chargeSeenMs = now;
    return;                            // 充电期间不回写 NVS：该存的是实测值，不是推算值
  }

  if (chargeActive && now - chargeSeenMs <= CHARGE_SESSION_GRACE_MS) {
    chargeTickMs = now;                // 宽限期：可能只是判定抖了一下，先按兵不动
    return;
  }
  chargeActive = false;
  chargeTickMs = now;

  // 高位走平上面那条 PLUGGED_MV 规则本来就会判成充电；万一漏了，也绝不能把被充电顶高的
  // 电压当成真实电量存下来——那会污染下一次充电会话的基准。
  if (ema >= PLUGGED_MV) return;

  lastKnownPct = voltageToPercent(ema);
  persistPct(lastKnownPct, now);
}

int powerBatteryLevel() {
  ensureLastKnownLoaded();
  uint32_t now = millis();
  if (now < BOOT_SETTLE_MS) return (int)lastKnownPct;   // 开机去抖窗口，顶着上次存的值
  // 窗口都过了还一次有效电压都没采到（getBatteryVoltage() 一直返回 -1 之类），就如实说
  // "不知道"，让调用方走各自的未知分支，别拿旧值/默认值冒充实时电量
  if (ema < 0) return -1;
  if (chargeActive) return (int)(chargeEst + 0.5f);
  return (int)lastKnownPct;
}

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------
int powerBatteryMv() { return ema < 0 ? -1 : (int)(ema + 0.5f); }

void powerDiag(int n) {
  if (n < 4) n = 4;
  if (n > 256) n = 256;
  int* buf = (int*)malloc(n * sizeof(int));
  if (!buf) return;
  int cnt = 0;
  uint32_t t0 = millis();
  for (int i = 0; i < n; i++) {
    int mv = M5.Power.getBatteryVoltage();
    if (mv > 0) buf[cnt++] = mv;
    delay(2);
  }
  uint32_t dt = millis() - t0;
  if (cnt == 0) {
    Serial.println("[batt] 一次有效读数都没有（getBatteryVoltage 返回 <=0）");
    free(buf);
    return;
  }

  // 排序取分位数。样本量最多 256，插入排序足够
  for (int i = 1; i < cnt; i++) {
    int v = buf[i], j = i - 1;
    while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = v;
  }
  double sum = 0, sq = 0;
  for (int i = 0; i < cnt; i++) { sum += buf[i]; sq += (double)buf[i] * buf[i]; }
  double mean = sum / cnt;
  double sd = sqrt(sq / cnt - mean * mean);

  Serial.printf("[batt] %d 次采样耗时 %lums\n", cnt, (unsigned long)dt);
  Serial.printf("[batt] min=%d p10=%d 中位=%d p90=%d max=%d  峰峰=%d mV\n",
                buf[0], buf[cnt / 10], buf[cnt / 2], buf[cnt * 9 / 10], buf[cnt - 1],
                buf[cnt - 1] - buf[0]);
  Serial.printf("[batt] 均值=%.1f 标准差=%.1f mV\n", mean, sd);
  Serial.printf("[batt] 内部状态: ema=%.1f charging=%d level=%d%% 采样窗峰峰=%dmV\n",
                ema, charging ? 1 : 0, powerBatteryLevel(), powerBatterySpread());

  // 把**所有**可能的"插没插"硬件信号都读一遍。power_util.h 顶上写着 M5 那两个在本机无效，
  // 但那是从文档抄来的结论——自己读一次才算数。
  // ⚠️ 枚举顺序是 is_discharging=0, is_charging=1, charge_unknown=2。别凭直觉猜——
  //    本机固定返回 2(unknown)，头文件那段注释是对的：M5Unified 给 Cardputer ADV 配的是
  //    _pmic = pmic_adc（只有 GPIO10 的分压 ADC），isCharging() 的 switch 里根本没有本机
  //    的分支，直落 default -> charge_unknown。
  {
    const int ic = (int)M5.Power.isCharging();
    const char* icName = ic == 0 ? "discharging" : ic == 1 ? "charging" : "UNKNOWN(本机无充电状态脚)";
    Serial.printf("[batt] isCharging()=%d %s  current=%dmA  VBUS=%dmV  (三个都是本机测不到的)\n",
                  ic, icName, (int)M5.Power.getBatteryCurrent(), (int)M5.Power.getVBUSVoltage());
  }
  // 原生 USB CDC：主机打开串口时为 true。它只能证明"连着电脑"，插充电头是测不到的
  // （没有 CDC 枚举），所以最多当一个单向证据用：true 一定插着，false 说明不了什么。
  Serial.printf("[batt] USB CDC 已连接=%d\n", (int)(bool)Serial);
  // 全量原始值，给电脑那侧做直方图用
  Serial.print("[batt] raw:");
  for (int i = 0; i < cnt; i++) Serial.printf(" %d", buf[i]);
  Serial.println();
  free(buf);
}

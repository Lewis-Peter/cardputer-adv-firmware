[English](memory-audit.md) | **简体中文**

# 内存 / 资源泄漏审计（2026-09-02）

    结论先放这儿：**查出两处真泄漏，都在 chat.cpp 和 bt.cpp，已修**。
    其余按类逐项过了一遍，没有发现别的泄漏。
    另外加了 `tools/memsweep.py`——因为读代码只能证明"有人写了释放"，
    证明不了"释放干净了"，那件事只有在板子上量才算数。

---

## 查了什么、怎么查的

不是"通读一遍找感觉"，是按**泄漏的几种成因**分类，每一类都有可机械检查的判据：

| 类别 | 判据 | 工具 |
|---|---|---|
| 堆分配没配对 | `new`/`malloc` 与 `delete`/`free` 数量与路径 | grep + 逐处读 |
| 任务/队列/信号量 | `xTaskCreate` 与 `vTaskDelete` | grep |
| SD 文件句柄 | `SD.open` 与 `close` | grep + ESP32 `File` 是否 RAII |
| HTTP 连接 | `http.begin` 与 `http.end`，以及 `end()` 到底关不关 socket | grep + 读 Arduino 源码语义 |
| 射频状态 | `esp_wifi_set_promiscuous(true/false)`、`scanNetworks/scanDelete` | grep |
| Sprite / 大缓存 | `createSprite` 与 `deleteSprite` | grep |
| app 退出路径 | 每个 `xxxExit()` 是否都挂进 `cleanupApp()`；每条 `screen =` 是否都经过它 | grep 交叉核对 |
| 缓冲越界 | 外来数据当下标写固定数组 | 逐个解析器读 + cppcheck |
| 无界增长 | 循环里往 `String` 追加、输入框没有长度上限 | grep |

cppcheck（`--enable=all --inconclusive`，20k 行）**零 error、零 leak 告警**，
只有一些 style 提示。所以下面的两处是靠读出来的，不是靠工具。

---

## 发现 1（重）：`chat.cpp` 每问一次漏掉一整套 TLS 上下文

### 现象层面

Chat 页每次提问都会永久少掉几十 KB 堆，而这块板子空闲堆总共才 70KB 出头。

### 两个原因叠在一起，缺一条都不会漏

**① `vTaskDelete(nullptr)` 不跑 C++ 析构。** 它把整块栈回收掉，栈上对象的析构函数
一个都不会执行。而 `chatWorker()` 的栈上恰好有三样带堆的东西：

    WiFiClientSecure client   析构里才 stop()，mbedTLS 收发缓冲是 16KB+4KB 那个量级
    HTTPClient       http     析构里才 end()
    String reply, err         回复最长 CHAT_REPLY_MAX，也是一块堆

**② `http.end()` 顶不上。** `HTTPClient::disconnect()` 里有这么一条：

```cpp
if (_reuse && _canReuse) { /* tcp keep open for reuse */ }   // 根本不关 socket
else                     { _client->stop(); }
```

`_reuse` 默认就是 `true`，而 OpenAI 那类端点必然回 `Connection: keep-alive`，
于是 `_canReuse` 也是 `true`——`end()` 走的是上面那条，socket 和 TLS 上下文原样留着。

**这条项目里早就知道**：`router.cpp` 的 `trafficClose()` 顶上把这段原文抄下来了，
因为 `/traffic` 那条常开流被它坑过（复用脏 socket 解出 "http 47" 这种不存在的状态码）。
只是当时没意识到 chat.cpp 里有同一个问题——那边更隐蔽，因为它**连析构这条后路都没有**。

### 修法：不补 `stop()`，改结构

第一版补丁是在三处 `vTaskDelete` 前面各加一句 `client.stop()`。**撤掉了**——
那是靠人记住，下次谁加一条提前返回就又漏了。

改成把正文拎成一个**会正常 return** 的函数：

```cpp
static void chatWorkerBody() { ... 一律用 return ... }

static void chatWorker(void*) {
  chatWorkerBody();          // 这里返回 = 栈上的 client / http / String 真的被析构了
  workerBusy.store(false, std::memory_order_relaxed);
  workerDone.store(true, std::memory_order_release);
  vTaskDelete(nullptr);      // 全项目这个函数里唯一的一处
}
```

不是靠记住补一句，是让语言替我们保证。顺带把内存序也变强了：原来 release 之后
还有析构要跑，现在 release 是真正的最后一步。

---

## 发现 2（轻）：`bt.cpp` 的 `new BLESecurity()` 从不 delete

Arduino BLE 的示例一律写成 `new BLESecurity()` 且从不 `delete`。这个对象本身不持有
任何东西——三个 setter 各自直接调 `esp_ble_gap_set_security_param()`，配置进的是协议栈，
对象用完就没用了。改成栈对象即可。

量级只有十几个字节，跟同一带那笔约 13KB 的 `BLEDevice::deinit()` 库泄漏没法比
（那笔是 Arduino BLE 的已知行为、项目里有意接受的，见 `btReleaseForOtherApps()` 的注释），
但白漏没有任何理由。

---

## 查过、确认没问题的

- **app 退出路径**：15 个 `xxxExit()` 全部挂在 `cleanupApp()` 上（`btExit` 走
  `isBleScreen()` 那条分支）。`` ` `` 退出和 BtnA 一键回菜单共用同一份清理，
  串口 `MENU` 也走它。唯一不清理的是串口裸跳转 `GOTO`/`GNSSMAP`——那是**故意留的逃生口**
  （正在 LoRa 抓包时想跳去看一眼别的页面），而且它每次都会往串口吼一行 `[jump]` 提醒。
- **FreeRTOS 任务**：chat / radio / player 三个后台任务都能正常结束或被安全删除；
  `bctrail` 的看门狗任务起了就不停，这是写在注释里的有意取舍（只在 Debug 开着时才起）。
- **SD 文件句柄**：ESP32 的 `File` 是 `shared_ptr<VFSFileImpl>`，析构即 close，
  RAII 兜着。代码里该显式 close 的地方也都 close 了。
- **HTTP**：除 `router.cpp` 的 `trafHttp` 是全局（有显式 `trafficClose()`）外，
  其余全是函数内的栈对象，正常返回时析构接管。
- **射频状态**：`esp_wifi_set_promiscuous(true)` 与 `(false)` 处处配对；
  `scanNetworks()` 与 `scanDelete()` 处处配对。
- **Sprite**：只有地图那块 48KB（+8bpp 的 24KB 降级档），`gnssMapExit()` 里
  `deleteSprite()`，且挂在 `cleanupApp` 上。申请失败时 `_img` 是空指针，不会半吊子泄漏。
- **缓冲越界**：`wsniff.cpp` 那几个 802.11 解析分支逐条核过——
  `ssid[33]` 配 `slen <= 32`、`ridSubCount[16]` 配 `fsub = (fc>>4)&0xF`、
  `body[24]/body[32]` 和 `ridOdidRaw[100]` 都先夹后 `memcpy`，IE 遍历有
  `off + 2 + tlen > len` 的护栏。`odid.cpp` 另有 `tools/odidtest` 带 ASan 的截断遍历用例。
- **无界增长**：所有文本输入框都有长度上限（算式 48 / 聊天 200 / Wi-Fi 密码 63 /
  热点密码 32 / 换算 16 / 电台 URL 120 / LoRa 消息 64 / 探针目标 40）；
  两处从网络流里攒 `String` 的地方（`clashLine` / `trafficPoll`）都有 400 字节的护栏。

---

## `tools/memsweep.py`：把"审计"变成能重复做的测量

读代码有个天花板：它能证明"有人写了释放"，证明不了"释放干净了"。
真正的证据只有一个——进去、退出、看堆有没有回到原位。

这个项目已经有全部原语（[serial-sweep.zh-CN.md](serial-sweep.zh-CN.md) 那套），缺的只是把它们串起来：

```bash
python3 tools/memsweep.py                 # 每个 app 进出 3 轮，报净堆变化
python3 tools/memsweep.py --rounds 5 --only Quake IR
python3 tools/memsweep.py --selftest      # 不用插板子，拿假设备验一遍工具本身
```

判定逻辑的关键是**只看第 1 轮之后**：第一次进去建缓存、连一次网、装个驱动，
都是一次性的，天然会掉一块又不再掉；只有"每一轮都掉"才是泄漏。
自检里专门埋了这两种形态各一个（`Leaky` 每轮漏 2KB / `Weather` 第一轮一次性掉 48KB），
要求前者被抓出来、后者不被误报。

⚠️ 默认跳过 BLE 三件套、Ducky、Hotspot、Settings，理由写在脚本里（简单说：
它们的堆变化不是泄漏，是有意的行为或者不该让脚本乱按的东西）。

---

## 后续：`RAMLOG`——补上"开机那一半"

这份审计查的是**运行期**：进一个 app、退出来，还回来没有。它答不了另外半个问题——
**开机走完之后那些内存是被哪一步吃掉的**。而这半个问题是有账的：[README.zh-CN.md](../README.zh-CN.md) 记着静态 RAM
四周内从 94,876 涨到 119,420（+24.5KB），当时查不出花在哪，因为手上只有两种数：
`pio run` 末尾的一个总数，和 `STAT`/`MEMCAP` 的"此刻还剩多少"。两种都是快照，
而"是谁吃的"是个**差值**问题。

所以补了 `src/ram_profile.{h,cpp}` + 串口 `RAMLOG`：`setup()` 里 11 个阶段各打一枪，
Wi-Fi 真正连上时再补一枪，打出来是一张带正负号的表。

两个不太显然的地方：

- **先攒着，不当场打。** 参考的做法（Bruce 固件的 `ram_profile`）是每个阶段直接
  `Serial.printf`。这块板子上那样会丢掉最想看的部分：走的是原生 USB CDC，
  `Serial.begin()` 排在 `setup()` 很靠后，而 `M5.begin()` 和主画布那 64KB 都在它之前，
  当场打的话这两步**没有任何地方收得到**。改成记进一个 16 格的静态数组，随时 dump。
- **满了丢新的，不挤掉旧的。** 开头那几步才是这套东西存在的理由。

代价是 320 字节静态 RAM（16 × 20）。这个项目对静态 RAM 很敏感，但换来的正是当初
记下 94,876→119,420 时缺的那样东西，所以也**没做成编译开关**——要重烧一次才能用的
诊断等于没有。

`tools/ramtest/` 是它的桌面试跑台（编的是同一份 `src/ram_profile.cpp`，只把底下的
`heap_caps_*` 换成一串编好的数），验的是表本身：列对齐、Δ 的正负号、16 格灌满后的行为。
另外 uisim 的语法检查给这个文件单开了 `-Werror=format`——整段输出都是格式串，而格式串
跟参数类型对不上在板子上是**静默**的：照样烧得进去，只是打出来的数是垃圾。

---

## 还没验的

- **本次两处修复都没有在硬件上跑过。** `chat.cpp` 那处的正确验证方式是：
  修前修后各连问 5 次，每次之间敲 `STAT` 记 `heap`。修之前应该看到阶梯状下降，
  修之后应该基本回到原位。`memsweep.py` 量不到它——那个脚本只进出 app 不发问题。
- **`memsweep.py` 只跑过自检**（假设备），没对着真板子跑过。第一次用建议加 `--verbose`
  先看几行收发对不对。
- **`RAMLOG` 的表在桌面上验过（`tools/ramtest`），但那串数字是编的。** 真机第一次跑出来的
  重点看两行：`canvas` 那行的 Δ 应该接近 -64,800（240×135×2），对不上说明打点位置错了；
  `wifi up` 那行应该接近 -36,000（[README.zh-CN.md](../README.zh-CN.md) 里实测的协议栈常驻量）。这两个数都有独立来源，
  正好互相印证。

---

## CanvasLease 推广（2026-09）

### 背景与契约规则

全屏画布 `cv` 独占 64.8KB（240×135×2 DMA 连续内存）。ESP32-S3 无 PSRAM，mbedTLS 握手需两块约 16.7KB（合计 ~33KB）的连续堆缓冲。在运行一段时间堆碎片化后，握手往往因拿不到连续块而失败。`CanvasLease`（`src/globals.h`）通过 RAII 在 TLS 抓取期间将全屏画布临时释放，出作用域自动恢复。

推广必须严格遵守四项铁律：
1. **证书与客户端前置**：`WiFiClientSecure client; tlsUseCaBundle(client);` 必须在 lease 之前建好。`tlsUseCaBundle` 会 calloc 一块永不释放的证书索引（~484B），若在 lease 作用域内分配，会落在画布腾出的空洞里将其钉死，导致显存永久无法恢复。
2. **逆序析构保证**：`CanvasLease` 必须声明在 `JsonDocument`、`filter` 等临时大对象之前。C++ 栈对象逆序析构，保证所有临时解析对象先释放干净，最后再向系统申请恢复画布。
3. **作用域内禁止持久化分配**：lease 作用域内只准有"用完即放"的瞬态分配。要持久保留的结果（数据数组、String 错误信息、缓存等）必须在进入 lease 作用域前备好（预先分配、`reserve()` 容量或使用静态缓冲），否则新增内存会插在画布空洞内造成显存再分配失败。
4. **渲染与并发安全**：确认抓取是在主线程同步执行（渲染自然暂停，无并发冲突）还是在后台任务中执行。若为主线程同步执行，需配合 `DeferredFetch` 在首帧渲染后触发，保证界面提示正常。

---

### 各应用评估与改造明细

| 应用模块 | 数据源与协议 | 改造方案 | 安全性分析与设计决策 |
|---|---|---|---|
| **src/quake.cpp** | USGS GeoJSON (HTTPS) | 在 `fetchQuakes()` 中应用 `CanvasLease` | **主线程同步**。`errMsg.reserve(96)` 与 client 前置；结果解析写入静态数组 `eqs[MAX_EQ]`；`filter` 与流式解析临时文档均逆序先于 lease 释放。 |
| **src/okx.cpp** | OKX C2C + CoinGecko Fallback (双 HTTPS) | 单个 `CanvasLease` 覆盖主源与回退源两次握手全过程 | **主线程同步**。两次握手间无渲染，单个 lease 避免中间无意义的 64.8KB 显存恢复与再释放抖动，确保持续为第二次握手提供大块连续内存；client 在两次握手间显式 `client.stop()`；前置预留 `errMsg`、`okxErr`、`fbErr` 避免错误分支分配。 |
| **src/typhoon.cpp** | JMA 防灾 JSON (多段 HTTPS) | `fetchList()` 应用 lease；`ensureDetail()` 用单个 lease 覆盖实况与预报两次握手 | **主线程同步**。`jmaGet` 改造为引用外部 client，避免反复分配证书索引；详情延迟加载在 400ms 防抖后执行；实况 specifications 与预报 forecast 在单次 lease 内连续完成，两次请求间显式 `client.stop()` 释放 TLS 缓冲；数据写入静态 `list` 与 `det`。 |
| **src/adsb.cpp** | ADS-B 单机航线查询 (api.adsbdb.com, HTTPS) | 飞机列表走明文 HTTP 不动；在 `fetchRoute()` 中应用 `CanvasLease` | **主线程同步**。350ms 防抖后拉取航线；前置 `fetchErr.reserve(64)` 与 client；lease 包裹 `filter`/`doc`；航线结果填入栈对象并存入静态环形缓存 `rcache`。 |
| **src/fx.cpp** | Frankfurter 外汇汇率 (HTTPS) | 在 `fetchFx()` 中应用 `CanvasLease` | **主线程同步**。前置分配 `pts` 结构体数组（~700B），前置 `errMsg.reserve(96)` 与 client；lease 声明于 `filter`/`doc` 之前；解析直接写入预备好的 `pts`。 |
| **src/github.cpp** | GitHub 贡献图 (HTTPS) | 在 `fetchContrib()` 中应用 `CanvasLease` | **主线程同步**。前置 `errMsg.reserve(96)` 与 client；lease 包裹 `fetchJsonStreamArray`；流式回调解析入静态数组 `tmpCount`/`tmpLevel`，无动态内存驻留。 |
| **src/chat.cpp** | LLM API (HTTPS) | **保持原状，不作修改** | Chat 拥有独立的后台 FreeRTOS Worker 任务与手动的 `canvasRelease()` / `canvasRestore()` 调度体系，涉及多线程并发交互，不适用简单的同步作用域 lease。 |
| **其它模块** | 天气/路由器/地图瓦片/IP定位 (HTTP) | **不适用** | 均为明文 HTTP 请求，无 TLS 握手的 33KB 连续大缓冲压力，无需借用画布。 |

---

### 真机验证检查清单

在连接串口或开启屏幕底部 Debug Bar 的情况下，真机重点验证：
1. **Quake 地震速报**：
   - 进 Quake，敲 `STAT` 记录 `free heap` 与 `largest`；
   - 按 `r` 触发重拉，观察抓取完成后 `STAT` 的 `largest` 是否恢复至抓取前水平；
   - 切换数据源（`m`）拉取 2.5_day 较大 GeoJSON，验证流式解析下画布恢复无异常。
2. **OKX 汇率**：
   - 进入页面，验证 OKX 主源成功后画布正常恢复；
   - 断网或在网关拦截 OKX 域名模拟 Fallback，验证连走两次握手后页面是否能正常渲染 CoinGecko 数据且显存不泄漏。
3. **Typhoon 台风预警**：
   - 进入页面，验证列表拉取后首屏正常推屏；
   - 连续按 `;` 或 `.` 翻动台风，验证 400ms 防抖触发后，实况+预报两次握手完成后地图与详情能正确渲染，`largest` 恢复原位。
4. **ADS-B 航线查询**：
   - 进入 ADS-B 页面，等待明文飞机列表刷出；
   - 按 `;` 或 `.` 切换选中飞机，350ms 后触发航线查询，观察航线起降机场代码与进度条是否正常显示，无白屏或断言崩溃。
5. **FX 外汇走势**：
   - 进页面查看走势图，按 `m` 切换 30d/90d 窗口，退出页面后核对 `pts` 释放是否彻底。
6. **GitHub 贡献图**：
   - 配置合法 `GITHUB_USER`，进入页面查看热力图加载，验证逐元素流式解析后画布按时恢复。

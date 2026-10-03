[English](ble-teardown.md) | **简体中文**

# 查 BLE 拆除路径那个偶发卡死

2026-08-09 的一整天。结论先写在前面：**没修好**——蓝牙来回切几次之后还是得重启，
UI 上已经明说了。但这一天产出的东西比结论值钱：一套取证设施（`src/bctrail.*`，已包含在固件中）、两条方法论上的教训，以及一堆能省掉别人重走一遍的实测数字。

尝试修复的代码留在实验分支中，**没有合入主线、也不建议合入**，理由见最后一节。

---

## 背景：为什么会去动这个

Arduino 的 `ESP32 BLE Arduino` 库没有拆除路径——`BLEServer` 连析构函数都没声明，
`createServer()` 直接覆盖上一个。于是每走一轮 `btHidSetup()` 就把一整套
server + service + characteristic + descriptor 永久漏掉。实测最大连续块：

```
63476 → 23540 → 18420 → 17396 → 10740 → （继续跌到崩）
```

`MEMCAP` 的块数印证了这是泄漏而不是纯碎片：来回两次，空闲总量 73460→80032（**反而涨了**），
已分配块 401→593。多出的 192 块永不释放，像钉子一样把连续空间钉碎。

---

## 第一轮：把库 vendor 进来补析构

把库抄进 `lib/BLE/`（跟框架原版只差 52 行、9 个文件），照持有树递归 delete，
新增 `BLEDevice::destroyServer()`。几处关键判断，事后看都是对的：

- **`BLEHIDDevice::~` 保持空的。** 它持有的是 server 树的**非拥有别名**，再 delete
  一次就是 double free。真正的 owner 是 `BLEServer → ServiceMap → CharacteristicMap`。
- **`BLECharacteristic::~` 里那行被注释掉的 `free` 没有放开。** `m_value` 是 `BLEValue`，
  内部由 `std::string` 管理，放开会把不属于自己的地址当堆块释放。
- **`createServer()` 改成"已有就返回"**，避免直接覆盖旧 server 造成整棵树失联。

### 内存效果：确实修好了

同样的操作序列（蓝牙 ↔ Calc 来回切），看 largest：

```
基准线： 63476 → 23540 → 18420 → 17396 → 10740 → （持续下降到崩）
修复后： 63476 → 25588 → 22516 → 19444 → 19444 → 18420 → 收敛不再跌
```

从"持续下降到崩溃"变成"收敛在 18~19KB"，这正是修好泄漏该有的形状。
**这条结论独立成立**，后面卡死那摊事没有推翻它。

### 但引入了偶发卡死

约每 10 次来回挂一次，第 3~9 轮之间随机。关键证据：

- **卡死时 largest 还有 18420，内存充足**——不是内存耗尽（早期固件第 5 轮卡死是内存耗尽，那是另一回事）。
- Saved PC 落在 `esp_pm_impl_waiti`，说明是**阻塞**不是 panic，主循环在等一个不会来的东西。

对随身设备来说，偶发卡死比缓慢泄漏更糟：后者可预测、重启能恢复。所以留在了实验分支上。

---

## 教训一：插桩会掩盖故障（典型 Heisenbug）

第一个假设是 FreeRTOS 信号量：这些对象每个都挂着信号量（service 4 个 / characteristic 3 个），
而 FreeRTOS 明确规定**不能删除还有任务阻塞在上面的信号量**，时机取决于 BLE 栈任务那一刻
在干什么，正好解释"偶发"。

于是在析构每一步之间打串口标记 + `Serial.flush()`。**15 轮一次没挂。**

也就是说，把析构过程摊开在时间上，故障就消失了。所以标记法拿不到现场——最后一条标记
永远是正常的那一条。这条本身是信息：敏感点在析构序列**内部的节奏**上。

> **可迁移的那部分**：查竞态时，任何带 I/O 的插桩都可能把窗口撑开到故障消失。
> 记录动作必须便宜到不改变时序。这直接催生了后面那套面包屑。

## 教训二：加延时只降频率，不消除

```
无延时                  第 3、9 轮挂      约 1/8
deinit 后 delay(200)     第 15 轮挂        约 1/15
每 service delay(20)     第 35 轮挂        约 1/35
```

频率随延时量近似成比例下降——**竞态的典型特征**。说明加延时这条路根本走不通：
加到多少都只是把概率往下压，永远留一条尾巴。

顺带也证伪了信号量那个假设，或者说 200ms 根本不够：在 `deinit` 和 `delete` 之间
插 `delay(200)`，第一轮 12 次干净，第二轮第 3 次就挂。

---

## 教训三：换成不扰动时序的取证——面包屑

`RTC_NOINIT` 内存里的环形缓冲：`bcMark()` 只做一次字节写入 + 下标自增，零 I/O 零锁。
卡死后靠复位存活，开机自动回放，也能用串口 `TRAIL` 随时读。

**这次抓到了现场。** 连续两次卡死，面包屑都终止在同一处：

```
... 1F <SERVER出   06 destroySrv后   00 注销GATTS前   01 deinit前
（02 deinit后 从未出现）
```

也就是说**对象树析构完整跑完、干干净净**，阻塞发生在下一轮的 `BLEDevice::deinit()` 内部。

> 判读要点：看**哪个编号没出现**。`02` 从未出现，说明卡在 `01` 和 `02` 之间，
> 也就是 `deinit()` 里面。

### 所有人都找错了地方

前两轮的注意力全押在析构上——方向就是错的。而整个 BLE 生命周期设计
（**永不 deinit、只 enable/disable**，见 `bt.cpp` 顶部）本来就是为了绕开 deinit 的不可靠，
`btReleaseForOtherApps()` 是全项目唯一还调 `deinit` 的地方。

回头看，早些时候测试那版第 5 轮卡死（largest 剩 7668），当时归因于内存耗尽，
**现在看很可能也是同一个 deinit 阻塞，只是被低内存掩盖了。**

---

## 顺带发现：GATTS app 槽位泄漏（真问题，但不是这个卡死的原因）

库里 `createApp()` 每次 `esp_ble_gatts_app_register(m_appId++)`，而 **GATTS 侧从来不注销**
（只有 client 侧有 unregister，还注释掉一半）。Bluedroid 的 GATTS 槽位有限，反复重建会耗尽。

实验代码里加了 `BLEDevice::unregisterServerApp()`，并把 `btReleaseForOtherApps()` 改成三段式：
**注销（协议栈还活着）→ deinit → 删对象（回调已停）**。实测加了之后仍在第 9 轮卡住，
且面包屑证明注销本身成功了——所以它不是本次卡死的原因。

⚠️ **这条在主固件里是否真的会耗尽，没有验证过。** 主固件的流程确实还在跑这条路
（`btReleaseForOtherApps()` 清 `hidStarted` → 下次进蓝牙 `btHidSetup()` → `createServer()`
→ `createApp(m_appId++)`），但 `deinit(false)` 走的 `esp_bluedroid_disable` 有没有把旧注册
一起回收，没人查过。要坐实这条得上机测，别照抄结论。

---

## 为什么不合入那个分支

要拿到内存修复，就得连整个 vendor 进来的 BLE 库（约 15k 行）一起吞，而那个版本带着
约 1/10 的偶发卡死。**净亏。** 当前固件的取舍是：接受泄漏，在蓝牙菜单上提前警告余量
（低于 20KB 标橙 `restart soon`，低于 13KB 标红 `restart before using`），让人主动重启。
在 MCU 上重启是合法策略。

## 下一步真要接着查的话

- **卡死时打 `vTaskList()`**——直接看是哪个任务阻塞在什么上，而不是猜。装个定时器在主循环
  停滞时输出，平时不扰动时序。`bctrail` 的停滞看门狗已经是这个形状了，把 `vTaskList()`
  加进去就行。
- **或者上 JTAG**，卡死时直接看调用栈（Linux 上的 udev 规则见 [linux-setup.zh-CN.md](linux-setup.zh-CN.md)）。
- 标记法已经证明不可用（会掩盖），不要再走。

## 留下来的东西

`src/bctrail.{h,cpp}`——面包屑 + 主循环停滞看门狗，已包含在固件中。跟 BLE 完全解耦，
以后任何"卡死但看不见现场"的问题都能复用。面包屑本体白拿（64 字节在 RTC slow memory，
不占 DRAM 堆）；看门狗要付 3KB 常驻栈，所以跟着 Settings 里的 Debug 开关走。

**"软复位不丢"这条已在固件上实测过**（2026-08-26）：`TRAIL MARK` 埋下
`AB 11 1F FF`，RTS 硬复位之后开机回放原样吐出这四个字节、连 `next=4` 的下标都保住了。
这个自证动作值得每次真开始查问题之前先做一遍——面包屑平时读出来全是零，
工具坏了跟"没走到那一步"长得一模一样。

---

## 2026-09-30 复查：拆除顺序错了

对照框架（arduino-esp32 2.0.17 / IDF v4.4.7 `38eeba213a`）源码重读了一遍：

- **早期试验的 `unregisterServerApp()` 是空操作。** 它在 `bleSuspended` 之后才调，此时
  bluedroid 已被 `btExit()` disable，而 `esp_ble_gatts_app_unregister()` 第一行就是
  `ESP_BLUEDROID_STATUS_CHECK(ENABLED)`（esp_gatts_api.c:63），直接返回 INVALID_STATE。
  "面包屑证明注销成功"只证明了函数返回了。另外 `bta_gatts_deinit()` 会在每次 bluedroid
  deinit 时清空 GATTS 控制块，所以跨 deinit 的"槽位耗尽"本来也不成立。
- **真正可疑的是顺序。** `btExit()` 先 disable 了 bluedroid **和 controller**，之后
  `BLEDevice::deinit()` 才去 `esp_bluedroid_deinit()`——在 controller 已停时拆 host 栈，
  违反 IDF 规定的 disable→deinit(host)→disable→deinit(controller)。`esp_bluedroid_deinit`
  对 BTC 任务是无限期 `future_await`；BTC 那边拆 HCI/BTU 线程 join 只给 1 秒、超时硬
  `vTaskDelete`，被杀线程若持锁，后续 `osi_alarm_deinit()` 永久等锁。这条链是推断。
- 修改：`btReleaseForOtherApps()` 先重新 enable controller，再按 IDF 顺序逐步拆，
  每步面包屑 `B0..B5`（出错时插 `BE <err低字节>`）。

判读：`B1` 有 `B2` 无 = 卡在 `esp_bluedroid_deinit`；`B3` 有 `B4` 无 = 卡在
controller deinit（闭源 blob）。完整一轮应为 `B0 B1 B2 B3 B4 B5`，无 `BE`。

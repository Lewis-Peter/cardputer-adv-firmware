[English](README.md) | **简体中文**

# Cardputer ADV 自制固件


> **开源版说明**：这是脱敏后的公开版本（全新提交历史）。使用前请 `cp src/secrets.h.example src/secrets.h` 并填入自己的 API key；
> SSH / VPN 探测目标等个人配置都在设备上填写（存 NVS），默认值为空或文档保留地址（203.0.113.10）。
> 编译期默认值（NTP 服务器、热点名/默认密码、网络探测目标）见 `src/config.h`；`cp src/config_local.h.example src/config_local.h` 后只写想覆盖的宏。
> 电台列表：改 `CFG_RADIO_PRESETS`，或在 SD 卡根目录放 `/radio.txt`（每行 `名称|http://直链|风格`，`#` 注释，最多 12 条，免重编译）。
> 地图在线瓦片源：`CFG_MAP_TILE_URL`（占位符 `{s}{x}{y}{z}`）/ `CFG_MAP_TILE_SUBS` / `CFG_MAP_TILE_WGS84`（坐标系）。
> 文档里提到的 `cardputer-bridge`（PC 端配套工具）目前未公开。许可证：MIT（`lib/` 与 `sha512_crypt.*` 为第三方代码，保留原许可）。

M5Stack Cardputer ADV 上的自制固件，40 个 app 塞在一个 240×135 的屏幕里。
从"显示一个会走的时钟"起步，长成了现在这样。

- 主控：ESP32-S3FN8 (Stamp-S3A)，8MB Flash，**无 PSRAM**（这条限制贯穿整个项目，见下面的"踩过的坑"）
- 屏幕：1.14" ST7789，240×135
- 键盘：TCA8418 I2C 键盘控制器（**不是**老 Cardputer 的 GPIO 矩阵，老代码抄不过来）
- 框架：Arduino + M5Unified，PlatformIO 管理
- 扩展：Cap LoRa-1262（SX1262 + ATGM336H GNSS），插着才有 LoRa / GNSS

硬件引脚、I2C 地址、各种硬件坑详见 [HARDWARE.zh-CN.md](HARDWARE.zh-CN.md)。
Linux 上跑工具链的坑见 [docs/linux-setup.zh-CN.md](docs/linux-setup.zh-CN.md)；
用串口自动跑一遍所有 app（测内存、批量截图）见 [docs/serial-sweep.zh-CN.md](docs/serial-sweep.zh-CN.md)；
查"卡死了但看不见现场"的取证办法见 [docs/ble-teardown.zh-CN.md](docs/ble-teardown.zh-CN.md)；
一次全项目的内存/资源泄漏审计（含两处已修的真泄漏）见 [docs/memory-audit.zh-CN.md](docs/memory-audit.zh-CN.md)。

---

## 烧录

```bash
brew install platformio          # macOS；没装过的话
pipx install platformio          # Linux；apt 里没有，用 pipx 装
pio run -t upload                # 编译并烧录
pio device monitor               # 看串口（115200）
```

平台、Arduino core 和所有库在 `platformio.ini` 里都钉死在精确版本上，**别改成版本范围**，
也别顺手升级：新版 FastLED 会多占 375KB Flash 和 6KB RAM，新版 M5GFX 会让屏幕刷新慢一倍。
原因、实测数据和升级步骤见 [`docs/dependency-pins.zh-CN.md`](docs/dependency-pins.zh-CN.md)。

板子走 ESP32-S3 的原生 USB（VID `303A`），**两个平台都不用装 USB 转串口驱动**：
macOS 枚举成 `/dev/cu.usbmodem*`，Linux 是 `/dev/ttyACM*`（内核自带 cdc_acm）。
串口一般能自动找到；找不到就先看设备名再在 `platformio.ini` 里指定：

```bash
python3 -m serial.tools.list_ports -v    # 找 VID:PID = 303A:1001 那一条
```

> **Linux 权限**：`sudo usermod -aG dialout $USER`（重新登录生效），否则打不开
> `/dev/ttyACM*`。**Arch 系没有 `dialout` 组，串口属于 `uucp`**，要用
> `sudo usermod -aG uucp $USER`。要用内置 JTAG 调试的话再加一条 udev 规则放开裸 USB 节点：
> `SUBSYSTEM=="usb", ATTR{idVendor}=="303a", MODE="0660", GROUP="uucp", TAG+="uaccess"`
> （Debian 系把这里的 `uucp` 换成 `plugdev`。）

> **Linux 上第一次 `pio run` 报 `No module named pip` / `MissingPackageManifestError`**：
> pipx 装的 PlatformIO venv 里没有 pip，装不上 esptool。它自己提示的
> `sudo apt install python3-dev ...` 是误导，修不好。完整过程和其它 Linux 坑见
> [docs/linux-setup.zh-CN.md](docs/linux-setup.zh-CN.md)。

> **连不上 / 烧录卡住**：按住板子顶部的 **G0 键别松**，插 USB，保持 1~2 秒再放。
> 固件如果在开机阶段崩溃重启，USB CDC 起不来，esptool 也会连不上，这时只能用这招。

### Wi-Fi 和密钥

Wi-Fi 不用改代码：烧完在设备上 **Settings → Wi-Fi** 扫描选网、输密码，连过的网络记进 NVS。

**Wi-Fi 是常开 + 自动重连的**：开机就连上并一直保持，天气 / 飞机 / 卫星 / Router / 地图瓦片
这些联网页面进去就能用，不用每次现连十几秒；记住的网不在范围内时协议栈会在后台一直重试，
回到覆盖范围自动就上。当板载热点（Hotspot）开启时，Wi-Fi 扫描（Settings → Wi-Fi）、信道分析（WiFi Chan）
和 Wardrive 踩点采用 ESP32 的 **AP+STA 混杂共存模式**扫描，**不再强关热点**，连着热点的设备不会掉线；
LAN 扫描只探测 STA 网段，不影响热点；只有嗅探（Sniffer）和无人机侦测（Drone ID）在混杂模式独占射频期间
会暂挂热点，退出后自动恢复热点；退出这些 app 后，`loop()` 里的看门狗会把 STA 拉回来。

代价是协议栈常驻约 **36KB 堆**（2026-08-26 实测 `STAT`：开着 72.7KB 空闲、关掉 109.4KB，
差 36,744 字节）。⚠️ **差值稳定，绝对值会随固件长大而缩水**：2026-07-30 同样的测法是
102KB / 139KB，四周后掉了约 29KB——其中 24.5KB 是静态 RAM 从 94,876 涨到 119,420 吃掉的
（`pio run` 末尾那行 RAM 就是它）。所以引用空闲堆的绝对值一定要带日期，差值才是能跨版本比的。
那 24.5KB 花在哪，当时**查不出来**——`pio run` 只给一个总数，`STAT` / `MEMCAP` 又都只报"此刻还剩多少"。
现在有 `RAMLOG` 了：开机每一步各吃掉多少，带正负号列成一张表，跨版本比第一行就行。
⚠️ **用过蓝牙之后是个例外**：BLE 协议栈占着约 64KB，跟 Wi-Fi 在这块板子上装不下，
所以退出蓝牙回到主菜单时 Wi-Fi 会保持断开——**停在菜单上不会自己恢复**，进任意一个
别的 app 就会释放 BLE 并把网拉回来（`btReleaseForOtherApps()`，故意拖到那一刻才做，
好让蓝牙里进进出出是零开销的）。

这块板子没 PSRAM，
所以 GNSS 地图那 48KB 底图缓存有可能申请不到——它会先退 8bpp（24KB，角标标 `8bit`，缩放照样能用、
影像有色带），两档都拿不到才显示 `no zoom: low mem`。

时区在 `src/globals.cpp` 的 `TZ_INFO`（默认 `CST-8`）。

需要 API key 的几个 app 得自己配一份 secrets：

```bash
cp src/secrets.h.example src/secrets.h    # 已在 .gitignore 里，不会被提交
```

| 配置项 | 给谁用 | 不填的后果 |
|---|---|---|
| `CHAT_BASE_URL` / `CHAT_API_KEY` / `CHAT_MODEL` | ChatGPT 页 | 问答报错 |
| `N2YO_API_KEY` | Sats 卫星过顶页 | 页面提示 "no N2YO key"（而不是报网络错） |
| `CLASH_BASE` / `CLASH_SECRET` | Router 页 | 连不上时会分开说：secret 错报 `auth failed - check CLASH_SECRET`，地址/服务不通才报 `no reply from ...` |
| `GITHUB_USER` | GitHub 贡献热力图 | 页面提示没配用户名（这个接口不要 token，填用户名就行） |

Weather / Planes / GNSS 地图 / Typhoon / Quake / FX / OKX 用的接口都免 key，不用配。

---

## 操作

导航键：`;` 上　`.` 下　`,` 左　`/` 右　`Enter` 进入/确认　`` ` `` 返回

顶部 **G0 键**（`M5.BtnA`）：熄屏时唤醒 · 在 app 里一键回主菜单 · 已在主菜单里则翻到下一组。

主菜单是分组网格，每组正好一屏（4×2），所以不用上下滚动。左右键在组内移动、走出边界切到相邻组，
顶部 tab 条显示当前在哪一组。

| 组 | app |
|---|---|
| **TOOLS** | Time（时钟 / 秒表+倒计时 两页）· IMU · Files（含 txt 阅读器）· Spectrum（麦克风 FFT）· Calc · Converter · Player（SD 卡 WAV）· **Crypto Suite（密码工坊：AES/RC4/XOR加解密 · 凯撒/维吉尼亚/Base64/Hex · Linux Shadow · 赛博暖手宝跑分 · 穷举挑战）** |
| **SCAN** | WiFi Chan（信道分析）· Sniffer（混杂嗅探）· Wardrive · **Drone ID**（无人机 Remote ID 扫描）· **BLE Scan**（含设备详情 + 找物雷达）——**五个都靠混杂模式/扫描，一个都不需要真的连上网**，没网的地方照样能用 |
| **NET** | Hotspot（含扫码入网）· LAN Scan · NetProbe · DnsFuzz · Router（Clash/mihomo 监控）· **SSH（交互式终端客户端）**——要真连上（或自己当 AP）才有意义 |
| **HID** | **BT Keys**（BLE 键盘）· **BT Media**（BLE 媒体遥控）· Ducky（USB HID）——三个出口原本散在两个组里，其实是同一件事：**把这台设备当键盘/遥控器去敲别人**，只是一个走 BLE 一个走 USB |
| **SIG** | LoRa（SX1262 嗅探）· GNSS（六页：定位/详情/卫星/配置/速度/行程诊断）· **Map**（高德卫星影像）· **IR（红外遥控，只发不收）** |
| **SKY** | **Astro（日照 / 月相 / 晨昏线 三页，完全不联网）** · Weather（实况 / 未来24h / 风 / 空气质量 / 五天预报 五页）· Planes（ADS-B 飞机雷达）· Sats（卫星过顶）· Typhoon（台风预警 + 路径）· **Quake（全球地震速报）** |
| **MORE** | ChatGPT · GitHub（贡献热力图）· Radio（HTTP MP3 网络电台）· **FX（美元/人民币汇率 + 走势）** · **OKX（USDT/人民币换算率）** · Reader（独立书架）· BadApple · Settings |

### Settings（系统设置）

Settings 菜单提供 14 项系统与硬件偏好配置（配置均存储于 NVS，重启或断电后保持）：
- **Wi-Fi**：扫描周围 Wi-Fi 热点并输入密码连接，记住的凭据存 NVS，开机常驻自动连。
- **PC MODE**：进入 PC 主导模式（释放 64.8KB 全屏画布直推状态屏，供 PC 端 cardputer-bridge（配套工具，暂未公开）调度 GNSS / IMU / Wi-Fi 传感器流；按 ` 键或串口 PCEXIT 随时退出并返回设置页）。
- **Brightness**：屏幕背光亮度控制（0~100%）。
- **Volume**：扬声器主音量调节（0~100%）。
- **Boot sound**：开机音效开关（ON / OFF，开启时开机播放高保真 5 阶升调琶音；关闭后启动完全静音，安静场合不扰人）。
- **LED Mode**：机身 RGB LED 指示灯模式。按 Enter 循环切换 6 种模式：关 (OFF) / 电量指示 (Battery，默认) / 呼吸灯 (Breathe) / 彩虹流光 (Rainbow) / 跑马灯 (Chase) / 音乐律动 (Music Reactive)。选择存 NVS，开机保持。支持外部功能临时接管（频谱页律动、倒计时闹铃），退出接管自动恢复。
- **UI Theme**：8 种全系统高科技 HUD 配色主题（Auto Reactive / Cyan / Green / Amber / Red / Purple / Ice Blue 等）。
- **Auto sleep**：无按键操作自动熄屏超时（Never / 30s / 1m / 3m / 5m）。
- **Timezone**：系统 POSIX 时区切换（默认 CST-8）。
- **Weather Unit**：温度单位（Celsius 摄氏度 / Fahrenheit 华氏度）。
- **Battery**：电池实时电压、电量百分比、放电曲线与标定状态。
- **Debug**：底部遥测状态条常驻开关（堆余量/最大连续块/WiFi/电压/电量/帧耗时）。
- **Format SD**：SD 卡快速格式化（带双重确认防护）。
- **About**：固件版本、编译时间及硬件资源状态。

**LED 模式与接管优先级**：
Settings 设置的 LED 模式为全局底色。当有特定应用需要使用指示灯时，通过 `ledSetOverride(true)` 临时接管，此时常规灯效让路：
1. **频谱页（Spectrum）**：按 `l` 键开启麦克风 LED 律动，直接映射现场声音响度换色，接管 LED。
2. **倒计时闹钟（Timer Alarm）**：闹铃触发时，若麦克风、播放器或电台在用（扬声器忙），借用 LED 红光闪烁警示。
3. **优先级保护**：若频谱页已经接管了 LED（正在随声律动），倒计时闹铃不会强抢 LED（避免抢占后再释放导致频谱律动被提前灭掉）；只有在无人接管时闹铃才借用。任何接管在退出或停闹时调用 `ledSetOverride(false)`，无缝恢复此前 Settings 里设定的 LED 模式。

**Settings → Debug** 打开后，屏幕底部会常驻一条状态条（存 NVS，重启还在）：

```
h100k b87k     -49dBm +4126 83%      23ms s2
 ↑堆余量 ↑最大连续块  ↑WiFi ↑电压mV ↑电量  ↑这帧画了多久 ↑Screen序号
```

电池那两个数的符号位是充电推测（`+` 在充 / `-` 没在充）。**拔线之后串口是断的，
这里是唯一能看见电压和电量的地方**——而电量查表的标定恰恰只能在拔线状态下做
（见 `src/power_util.cpp` 里那张实测重建的曲线表）。

最大连续块低于 50KB 会标橙——GNSS 地图那 48KB 底图缓存就是从这儿要的，不够就只能退回世界图。
这条会盖掉底部的按键提示/页码点，关掉就恢复。同时它也放开那些平时太吵的串口日志（比如每块地图瓦片一行）。

### 开机唤醒序列与硬件自检 (Cyberpunk Boot Sequence & POST)

固件启动时执行全套高科技赛博朋克唤醒动画与硬件自检：
- **赛博朋克视效**：激光地平线水平扫掠、战术 HUD 四角准星瞄准框 `┌ ┐ └ ┘`、微点阵底纹基底。
- **动态核心拓扑**：中央六角芯片核心与脉冲流动电路总线，配合 "CARDPUTER" 逐位字符流矩阵解密动画与 "ADV" 战术霓虹发光徽章。
- **等离子充能轨**：16 段等离子能量充能轨（翠绿 -> 赛博青 -> 极光白高亮过载），动态轮巡刷新硬件状态（`CORE / TCA8418 / AOD / AUDIO / SEC BUS`）。
- **合成器开机音效**：5 阶升调琶音音效（`C5 -> E5 -> G5 -> C6 -> G6 Ready`），由 `M5.Speaker` 实时合成；可在 `Settings → Boot sound` 中随时一键静音。
- **极速按键跳过**：开机时优先初始化键盘控制器 `kbd::begin()` 与扬声器，**按键盘任意键或顶部 G0 物理键可毫秒级打断并跳过动画**，秒进主系统，毫无迟滞。
- **硬件自检 (POST AUDIT)**：提供高科技深色卡片控制台 HUD，启动时自动审计 ESP32-S3FN8 核心、TCA8418 键盘矩阵、MicroSD 存储卡、RTC 面包屑总线及网络栈。

### 重点 App 详解

多页的 app 用 `.`/`/` 前后翻页，底部有页码点。几个特殊键：

- **GNSS（六页）**：Cap LoRa-1262 上的 ATGM336H 导航定位模组。用 `.`/`/` 翻页：
  - **第 1 页 定位主页**：全周天际天空图（圆心天顶、圆周地平线，按星座上色、SNR 映射点大小）、经纬度、海拔、对地航速等。
  - **第 2 页 详情检视**：2D/3D 定位状态、GGA 定位质量指示（单点/DGPS/RTK Fix/Float/惯导推算等）、PDOP/HDOP/VDOP 紧凑多维精度因子、大地水准面差距（Geoid Separation）、在用/可见卫星数；**按 `c` 键循环切换 4 种坐标格式**（DEG 经纬十进制度 / DMS 度分秒 `dd°mm'ss.s"` / GRID Maidenhead QTH 瓦片定位格 / UTM 投影分区与米制坐标）。
  - **第 3 页 卫星信号表**：`[` / `]` 滚动查看每颗卫星的星座、PRN 编号、SNR、仰角和方位角；列表中**明确区分并在前缀标注在用卫星（In-Use，来自 GSA）与仅可见卫星**。
  - **第 4 页 模块配置**：`[` / `]` 选择项目，`-` / `=` 调整值，`Enter` 应用，`S` 保存配置到模块 Flash；支持 5 项参数：Rate 更新频率（1Hz/2Hz/5Hz，默认 5Hz）、System 星座组合（GPS/BDS/GLONASS等）、Dynamic 动态模型（Aero <2g / Aero <1g / Vehicle / Portable，开机默认 Aero <2g 高动态）、NMEA 输出语句集（full / nav+gsv 精简集，默认 nav+gsv）、RF antenna 天线电源开关。
  - **第 5 页 速度表**：半圆运动仪表盘，带动态发光弧与机械指针；**自适应动态量程**：120 km/h（市区骑行/驾驶）/ 360 km/h（高铁/高速，时速超 100km/h 自动升档）/ 1000 km/h（民航客机巡航，时速超 360km/h 升档），内置 50/100 与 300/360 km/h 回差缓冲消除临界跳变与死区。
  - **第 6 页 行程与诊断**：累计行程里程（自适应抗漂移滤波，连续跳点过滤）、总时长与纯移动时长、最高航速与平均速度、TTFF 首次定位耗时与 REACQ 重定位耗时、在用/可见星数、运动状态（MOVING/IDLE）、Top-4 卫星平均 SNR 与最近 2 分钟 SNR 历史时域曲线；**按 `r` 键重置行程统计与波形历史**。
  - ⚠️ **开机优化与数据可信度**：开机自动下发高动态模式（`PCAS11,6` Aero <2g）、5Hz 高更新率（`PCAS02,200`，避免出厂 1Hz 在高速移动下几十米的严重滞后）、精简 NMEA 语句集至 nav+gsv（`PCAS03` 仅保留 GGA/GSA/GSV/RMC，避免 5Hz 双星座全量打爆串口波特率与 RX 缓冲）。带开机 3.5s 延迟兜底重发与热插拔非阻塞 500ms 重发。TinyGPS++ 的 `isValid()` 一旦有一次定位便永久为 true，固件严格施加 3 秒内更新判据（`GNSS_FIX_STALE_MS = 3000`）；速度与航向仅在定位新鲜且 HDOP ≤ 8.0 时才视为可信，失锁或遮挡大跳点时页面显示 `n/a`，杜绝静止漂移显示虚假时速。
- **Map**：`[` 缩小 `]` 放大，五档 WORLD / z5 / z8 / z11 / z14。
  没有卫星定位时会退回一次 **IP 定位**当中心点（顶栏标 `ip`），所以室内也能开；
  但**尾迹和"你在这"的标记只在真有 GPS 时才画**——IP 定位是个城市级的点，
  拿它画当前位置是撒谎，连成轨迹更是。
  ⚠️ 进这一页会申请 48KB 底图缓存（实测 `STAT`：进入 heap 72632→24132，退出完整还回来）。
  它原来是 GNSS 的第 5 个子页，翻页翻到就悄悄申请；拆成独立 app 之后这笔开销是一次明确的进入动作。
- **LoRa**：Cap LoRa-1262 模组驱动（SX1262 射频收发），分两页：
  - **第 1 页 频谱与遥测**：2.4 寸频段能量柱状图与瀑布图，支持 ISM(863-928MHz) 与 VHF(161.5-162.5MHz) 双频段切换（`v`）；四联装仪表舱实时显示射频参数（Freq/BW/SF/CR）及当前底噪 RSSI/SNR 指针；支持 `a` 开启「AIS 船舶蹲守」与自动写入 SD 卡 CSV 日志。
    ⚠️ **这是纯 RSSI 能量检测，不解调**——`up` 是"AIS 信道比对照点亮了一下"的轮数，
    不是收到的船舶报文数，本项目至今没解出过任何一条 AIS 报文。
    ⚠️ 底部读数会被 debug 条盖住，量测前先在设置里关掉 Debug。
    蹲守时自动写 SD：`/ais/YYYYMMDD_HHMMSS.csv`，每 5 秒一行加上每次 UP/DN 事件一行，
    列是 `t_s,round,a1,a2,c1,c2,d,bias,spread,thr,ev,lat,lon,alt`。屏幕上的 `up/dn` 是
    相对现场自适应门限算的，**换地方就换了刻度、不能跨地点比**；要比不同位置/不同天线，
    比 CSV 里 `a1/a2` 的绝对 dBm 和 `a-c` 的分布。顶栏没显示 `SD` 而是 `NOLOG` 就是没记上。
  - **第 2 页 数据包嗅探与 P2P 战术终端**：实时捕获空口 LoRa 数据包（显示 Hex Raw Dump、RSSI、SNR、数据长度），支持直接打字发送 P2P 战术短报文。
- **BLE Scan / BT Keys / BT Media**：原来是一个 Bluetooth app 下的三个子模式，
  现在是三个独立 app（BLE Scan 归 SCAN，另两个归 HID）——它们之间**只共享一次 BLE init**，
  功能上毫不相干。三个之间来回切**不花钱**：`btReleaseForOtherApps()` 只在
  「不在主菜单 **且** 不在任何 BLE 页面」时才真的释放，所以经过主菜单转场是零开销的。
  ⚠️ **蓝牙和其它 app 来回切几次之后要重启**。Arduino 的 BLE 库没有拆除路径
  （`BLEServer` 连析构函数都没声明，`createServer()` 直接覆盖上一个），所以每来回一次
  就永久漏掉一整套 GATT 对象树。实测最大连续块 `63476 → 23540 → 18420 → 17396 → 10740`，
  再往下 `BLEDevice::deinit()` 会永久阻塞、整机卡死（用 RTC 面包屑抓到的现场，
  卡在 deinit 内部而不是析构路径）。所以蓝牙菜单会在余量低时提前警告：
  低于 20KB 标橙 `restart soon`，低于 13KB 标红 `restart before using`。
  尝试修库时发现内存确实能修好（收敛在 18KB 不再跌），但 deinit 的阻塞未能解决，因此未合并。**整个排查过程、实测数字和两条方法论上的教训
  （插桩会掩盖故障 / 加延时只降频率不消除）见 [docs/ble-teardown.zh-CN.md](docs/ble-teardown.zh-CN.md)**——
  当时留下的面包屑取证设施已合入固件，见下面的 `TRAIL` 指令。
  - **Keyboard mode**：把设备当成一块真的 BLE 键盘，整块键盘原样转发给主机。
    - **组合键**：`ctrl` / `opt` / `alt` 三颗键现在是真的修饰键（`opt` 的 HID 语义是
      Cmd/Win）。按住不放是常规用法；**单独点一下再松开＝锁定**，下一个键用完自动解开，
      单手也能发 Ctrl+C。屏幕上四个方块：实心＝按住，描边＝锁定。
    - **Fn 层**：`Fn+Tab`＝Esc，`Fn+1..0-=`＝F1~F12，`Fn+;`/`.`/`,`/`/`＝↑↓←→，
      `Fn+退格`＝前向删除，`Fn+[`/`]`＝Home/End。Tab 也能发了。
    - **长按连发**（420ms 后开始，约 18 键/秒），只在这一页开，不影响别处的菜单导航。
    - 屏幕中间是**回显框**，显示主机那边这一行大概长什么样（退格会真的退一格，
      **回车会清空**——主机那一行已经提交，新的一行是空的，继续挂着上一行就是在假装），
      下面一行是最后发出去的组合键，右上角是累计发送键数。没连上还在打字会标红
      `keys dropped`，明确区分「没发出去」和「发了没生效」。
    - **`Fn+Enter` 切 IME模式**（右上角标青色 `IME`）。主机上开中文输入法时必须打开：
      按键本来就是实时逐个发的，候选栏会正常实时跳出来，但**回显框会失真**——屏幕上是
      拼音、主机上是汉字，而且退格删的是输入法的候选缓冲区，本机无从得知删掉了什么，
      上屏一次就彻底错位。开了 IME 模式后回显退化成老实的「本机键流」：退格只记一个
      `<`、回车记 `|`，不再假装知道主机状态；同时自动关掉长按连发（连发退格会把整串
      候选一口气吃光）。纯英文输入时关掉它，回显手感更好。
    - ⚠️ **退出这一页是 `` Fn+` ``，不是裸 `` ` ``**——裸反引号要留给主机打出来。
      顶上的 GO 物理键也能退，作用一样。Esc 因此让位到了 `Fn+Tab`。
      为了别让人学了一个手势换一页就失效，`` Fn+` `` 在**其余所有页面**都等价于裸 `` ` ``。
  - **Media remote**：把设备当 BLE 媒体遥控器。`,`/`/` 上下曲，`;`/`.` 音量，`Enter`
    播放/暂停，`m` 静音，`s` 停止（`s` 以前只有 README 写了、屏幕上不显示，现已补进底部提示）。
    跟 Keyboard mode 是同一个 HID 设备、同一次配对。
    ⚠️ 从旧固件升上来的话，主机可能缓存着旧的 HID 报表描述符导致媒体键没反应，
    在主机上「忘记此设备」重配一次即可。
  - **设备详情页**还显示 Service Data（AD type 0x16）。这跟 Service UUID 是两个不同的
    字段，Remote ID（OpenDroneID）的载荷就播在这里——UUID `0xFFFA`、首字节 `0x0D`，
    命中会标绿。用来确认某台无人机是不是走 BLE 播 Remote ID。
  - **找物雷达 (Sonar Radar HUD)**：Scan devices → 选中设备 `Enter` 看详情 → 再 `Enter` 进雷达。
    战术全周扫描雷达圆盘与旋转扫描光束，持续跟踪目标设备 RSSI，画时间历史衰减曲线；结合五声音阶接近音频音效（越近音阶越高，`m` 静音，`r` 重置曲线）。
    距离是按路径损耗反推的**粗估**，人体遮挡/朝向/金属家具都能让它差一倍，只当
    「近了还是远了」用。⚠️ 按 MAC 跟踪，而 AirPods / 防丢标签 / 手机这类用随机地址
    的设备每十几分钟换一次地址，换掉就跟丢——回扫描列表重新选即可（地址类型在详情页看）。
- **WiFi Chan**：2.4GHz 13 信道频谱能量柱状图与峰值保持标尺，实时统计信道内 AP 数量、最大 RSSI 及信道占用拥挤热度。
  - `;`/`.` 切换选中信道，`r` 强制立即重扫，`a` 切换自动轮询扫描（Auto Scan）。
  - `Enter` 进入**信道详情检视**（`SCREEN_WIFI_CHAN_DETAIL`），列出该信道内所有扫描到的 AP（SSID、BSSID、RSSI、加密方式、OUI 厂商识别如 Apple/Huawei/Xiaomi/TP-Link/DJI 等），排查同频干扰。
  - **热点共存**：当板载热点（Hotspot）开启时，扫描自动采用 ESP32 的 `WIFI_AP_STA` 混杂共存模式，**不再强关热点**，连着热点的设备不掉线；退出后切回纯 AP 模式。
- **Wardrive**：Wi-Fi 战术踩点与地理标绘。结合 Wi-Fi 混杂模式与 Cap LoRa-1262 上的 ATGM336H GNSS 定位模组，边走边测周围无线热点。
  - 顶部 HUD 实时显示当前 GPS 经纬度定位状态、跟踪卫星数、已发现 AP 计数与信号质量。
  - 自动写入 SD 卡标准 Wigle CSV 日志（`/wardrive/YYYYMMDD_HHMMSS.csv`），便于导入 WiGLE 或 GIS 工具。
  - `Enter` 开始/停止单次扫描，`a` 切换自动循环扫描，`r` 重置统计。
  - **热点共存**：热点开启时同样以 `WIFI_AP_STA` 模式共存扫描，连入设备不掉线。
- **Drone ID**：扫附近无人机的 Remote ID（ASTM F3411 与国标 GB 两套都解）。列表显示编号、
  运行状态（AIR 在飞 / GND 地面 / EMG 紧急 / LOST 失联）、离你多远和什么方位、高度、信号；
  `Enter` 看详情（完整坐标、速度、航向、飞手位置、登记号）。`;`/`.` 选择，`r` 清空。
  一旦发现目标就**锁死它所在的信道**——实测跳频会丢掉约 92% 的包（蹲死信道 25 秒收 140 包，
  跳频只收到 10~12 包），锁频之后收包率高一个数量级；目标失联再自动回去跳频。
  ⚠️ 只收 2.4G Wi-Fi：ESP32-S3 没有 5G，而 BLE 那条要独占射频、跟 Wi-Fi 混杂模式共存不了
  （实测国产机走的就是 Wi-Fi beacon；真遇到只播 BLE 的，用串口 `BTDUMP` / `BTEXT` 兜底）。
  ⚠️ **无人机一般只在飞行时才广播**，停在地上多半什么都收不到——这不是没扫到。
  ⚠️ **热点暂挂**：因混杂模式需独占射频，进入时自动暂挂热点（`hotspotSuspend()`）并显示挂起状态，退出时自动恢复热点（`hotspotResume()`）。
- **Sniffer**：Wi-Fi 混杂模式空中嗅探与数据包统计。与 Drone ID 一样，独占射频期间自动暂挂热点，退出自动恢复。
- **Hotspot**：`Enter` 开关 AP，`p` 改密码，`q` 出二维码（手机相机扫一下直接入网）。
  - 支持在 Wi-Fi 扫描（Settings）、WiFi Chan 和 Wardrive 运行时以 AP+STA 混杂共存模式保持在线；
  - 独占射频的 Drone ID / Sniffer 运行期间自动暂挂，退出后无感恢复；LAN Scan 仅扫描 STA 网段，不影响热点。
- **SSH**：全功能交互式 SSH 终端客户端（基于 LibSSH 协议库）。
  - **无 PSRAM 内存自愈**：进入 SSH 连接时自动临时释放 64.8KB 全屏画布（`cv.deleteSprite()`），SRAM 空闲暴涨至 140KB+，用于承载 24KB 独立线程栈与加密握手堆；断开退出时自动恢复 `cv`，其他 40+ 个系统页面无缝运行。连接过程直接向屏幕绘制进度，无显存浪费与闪烁。
  - **VT100/ANSI 硬件直绘终端与动态字号切换**：`M5.Display` 硬件字符直绘，零闪烁、刷新极快；完整支持 ANSI 16色/256色/24位真彩色、光标绝对/相对定位、清屏清行、退格滚屏与 UTF-8 盒形图回退。在活动终端中**按 `Fn + Enter` 循环切换 3 种字号模式**：
    - `8x16`（30 列 × 8 行，经典 VGA 大字，字大清晰易读）
    - `8x8`（30 列 × 15 行，宽体中字，兼顾清晰度与行数）
    - `6x8`（40 列 × 15 行，紧凑小字，最大容纳 40 列宽）
    切换时实时向远端发送 PTY 窗口大小重协商（`TIOCSWINSZ` / `ssh_channel_change_pty_size`），htop、vim 等自适应排版。
  - **认证方式：密码与 SD 卡私钥自动探测**：除密码认证外，完整支持基于 RSA / ED25519 等私钥认证。支持在 MicroSD 卡根目录或 `.ssh/`、`ssh/` 目录下放置私钥文件（自动候选探测 `/id_ed25519`、`/id_rsa`、`/id_ecdsa`、`/ssh.key` 等 13 个常见文件名），或在 SD 卡 `/ssh.cfg` / `/ssh.txt` 中显式指定 `key=/path/to/key`；若私钥带 Passphrase 保护，密码字段自动用作解密口令。
  - **串口终端透传**：通过电脑 USB 串口连接时，当处于活动 SSH 终端界面，除系统控制指令（`STAT`/`MENU`/`REBOOT`/`VOL` 等）外，敲入的所有字符串命令（如 `ls`、`pwd`、`fastfetch` 等）均会直接原样透传至远程 SSH 终端 Shell 并执行。
  - **按键映射**：`Fn + Tab` 发送 `Esc`（支持 vim/nano）；`Fn + ; . , /` 映射为 ANSI 方向键 `↑ ↓ ← →`；`Fn + [ / ]` 为 `Home / End`；`Fn + Backspace` 为 `Delete`；`Ctrl + 字母` 支持 `Ctrl+C` / `Ctrl+D` / `Ctrl+Z` 等任意组合；`Fn + \`` 或物理 `G0` 键随时安全退出（CAS 所有权机制杜绝 session/channel 双重释放）。
- **Planes**：`;`/`.` 换关注哪一架，`r` 刷新
  ⚠️ 所有 HTTP 请求现在统一带 `User-Agent: cardputer-adv/1.0`（见 `http_json.h`）。
  Arduino 默认发的 `ESP32HTTPClient` 已经被 adsb.lol 按 UA 拦了
  （403 `User-Agent too generic; include valid contact info.`）。
  **注意 `http.addHeader("User-Agent", ...)` 是无效的**——Arduino 的 HTTPClient 有一张
  自己管的头名单（Connection / User-Agent / Host），addHeader 遇到它们直接静默跳过，
  必须用 `setUserAgent()`。
- **Sats**：`c` 切 Starlink/GPS，`r` 刷新。需要 `N2YO_API_KEY`。
  搜索半径按类别走（Starlink 25° / GPS 90°），因为"能看见"的条件是卫星星下点落在
  地平线圆内，而那个圆的半径 `acos(Re/(Re+h))` 只跟轨道高度有关：
  Starlink 550km → 23°，GPS 20200km → 76°。两者都写死 70° 的话，对 GPS 偏小、
  对 Starlink 则会拉回 122 颗共 18KB（绝大多数在地平线以下、进来就被扔掉），
  而这一页还背着 TLS，直接把堆挤到 6.2KB 导致响应读不完。
- **Typhoon**：`;`/`.` 换台风（同时有多个时），`r` 刷新；第二页是路径图
- **IR**：红外遥控。`,`/`/` 换设备，`;`/`.` 选按键，`Enter` 发送，`p` 关机扫频
  （把每台设备的 Power 挨个发一遍），`r` 重读 SD。
  ⚠️ 这块板子的红外**只有发射、没有接收**（GPIO44，见 [HARDWARE.zh-CN.md](HARDWARE.zh-CN.md)），
  所以**学不了别人的遥控器**，码只能来自码表。内置了 LG / Samsung / Sony 三台电视，
  但那些值是抄自公开码表、**没有实机验证过**（右上角标着 `[BUILT-IN]` 就是这个意思；SD 卡载入的显示 `[SD]`）。
  自己的设备写成 SD 卡上的 `/ir/<名字>.ir`，一行一个按键：

  ```
  # 井号开头是注释；可选的一行 name 覆盖文件名
  name    My TV
  Power   nec32  0x20DF10EF        # NEC 时序、32 位 MSB 优先 —— 公开码表最常见的写法
  Ch+     nec    0x0408            # 高字节 addr、低字节 cmd，反码自动补
  Mute    sony   0x290 12          # Sony 要给位数（12/15/20）
  Beep    rc5    0x300C
  AcOn    raw    38000 3400,1700,430,1300,430,430
  ```

  发射走 **RMT** 而不是软件打节拍：这台机器的 Wi-Fi 常开，协议栈随时会抢走 CPU 几百微秒，
  而红外的位间隔只有 560µs——软件打节拍必然被拉长，表现是"有时能用有时不行"。
  RMT 是硬件定时的。⚠️ 它占 RMT 3 号通道，**只在这一页开着时安装驱动**，退出就还回去
  （FastLED 驱动 SK6812 也要用 RMT）。
  编码那一半（时序、位序、曼彻斯特极性）在电脑上有测试台：`tools/irtest`。

- **FX**：美元/人民币汇率。`;`/`.` 翻条目，`r` 刷新，`m` 切 30 天 / 90 天窗口（记进 NVS）。
  第一页是现价 + 当日变化 + 走势图，第二页是逐个交易日的收盘价和日变化。
  数据来自 **frankfurter**（欧洲央行参考汇率，免 key 不限流）。选它的关键理由是
  **一个请求就拿到整段时间序列**——画 30 天走势不用发 30 次请求，而对这块板子来说
  请求次数就是成本（每次 TLS 握手要抢 40KB 连续堆）。
  ⚠️ 两条局限写在页面语义里了：欧洲央行**只在工作日公布**，所以横轴是"第几个交易日"
  不是"第几天"，周末在图上是被压掉的；而且这是**参考汇率不是银行结售汇价**，
  跟你换汇时看到的差几个点是正常的。
  涨跌用**红涨绿跌**（国内习惯，跟欧美相反），但 ▲/▼ 和 ± 号才是主要信号——
  颜色只是辅助，换个习惯的人也不会读反。

- **OKX**：USDT → 人民币实时行情（双源高可用：OKX 实时 C2C + CoinGecko 自动 Fallback），`r` 手动刷新（自动 5 分钟一次）。
  - **主源（OKX C2C 实时行情）**：拉取 OKX 实时 C2C 买入行情（`v3/c2c/otc-ticker/quotedPrice`），真实反映买卖双方撮合的法币/OTC 实时市场价（含 OTC 实时溢价）。
  - **备选源（CoinGecko Fallback）**：针对国内网络可能遇到的 GFW 阻断、连接超时或接口异常，设备自动快速降级切换至 CoinGecko USDT/CNY 汇率接口（免翻墙、极速稳定）。
  - **UI 状态感知**：页面顶部状态栏清晰标注当前数据源（青色 `OKX C2C` 或黄色 `CoinGecko`），下方副标题明确说明数据来源与性质，换算反向（1 CNY = x USDT）同步计算。
  - 串口支持通过 `OKXDUMP` 查看主源原始报文，或通过 `OKXDUMP FB` 查看备选源原始报文。

- **Radio**：赛博朋克 Hi-Fi 流媒体网络电台，实时解码 HTTP MP3 广播流。
  - 深色调谐器界面，内置 15 频段动态音频频谱仪（动态 FFT 律动波形）与扬声器主音量显示。
  - 双仪表舱分别显示实时缓冲余量波形与码率（kbps）及流媒体元数据（ICY-Title / Station Name）。
  - `;`/`.` 切换预设电台（Groove Salad / Lush / Deep Space 1 等），`Enter` 播放 / 暂停，`,`/`/` 或 `-`/`=` 调整音量。
  - `u` 呼出自定义流媒体 URL 输入终端（`SCREEN_RADIO_URL`），实时输入并解析外部网络广播流（空格键自动填充 `http://` 前缀）。

- **Quake**：USGS 的公开摘要源，全球覆盖、免 key。`;`/`.` 选条目，`r` 刷新，
  `m` 在两个源之间切（**M2.5+ / 24 小时** 和 **M4.5+ / 一周**，选择记进 NVS）。
  第一页是列表（震级 / 地名 / 多久之前，选中那条另给深度、离你多远、当地时间），
  第二页把它们打到世界地图上，点的大小和颜色按震级分档。
  跟 Typhoon 刻意分工：JMA 只发**西北太平洋**的热带气旋，这一页是全球的。
  ⚠️ 这一页**不走 `fetchJsonHttp`**：2.5+/24h 那个源常有 100 多条、整包 200KB 以上，
  整包进 ArduinoJson 必然 NoMemory（GitHub 页就是这么炸过的）。改成逐 feature 流式解析 +
  过滤器，堆占用跟返回大小无关。震级为 `null` 的条目（刚发生、还没定级）直接跳过。
- **GitHub**：`r` 刷新（半小时内不重复拉）。
  ⚠️ **`today` 可能显示 `--`**，那是"本地今天还没有数据"，不是"今天提交了 0 次"。
  贡献接口的窗口按 **UTC** 收尾，而设备用本地时区——UTC+8 的清晨（本地已跨日、UTC 还没跨）
  本地今天压根不在窗口里。等 UTC 跨日就会出数。
  （私有仓库的提交**会**被统计，前提是你在 GitHub 上开了 "Include private contributions
  on my profile"。）
- **Spectrum**：麦克风 256 点 FFT 音频频谱仪。
  - **`W` 键切换瀑布图与柱状图**：按 `W` 切换**瀑布图（语谱图 / Spectrogram）**与**经典柱状图（Bars）**。瀑布图采用局部滚屏技术，60 dB 自适应动态范围映射 AGC 前原始分贝，过滤 DC 直流分量，仅在新 FFT 帧到达时平滑推进行，黑→蓝→青→绿黄→橙红→白伪彩色渐变；柱状图显示 32 频段能量与白色峰值保持线。
  - **`l` 键切换"麦克风驱动 LED 律动"**：通过 `ledSetOverride` 接管板载 RGB LED，随现场音量动态映射红/黄/绿律动跳跃。
  - **`s` 键开关频谱流式输出**（顶栏标红 `REC->serial`）：开了之后每秒 5 帧往串口吐 `SPEC {json}`，配 `tools/spec_view.py` 存成 ndjson / 渲染语谱图，之后在电脑上用 `tools/spec_analyze.py` 慢慢分析。
    设备端**一行语义都不做**——这块芯片没 PSRAM，装不下任何像样的音频分类模型；但它有麦克风、能带出门、能用电池跑，而电脑反过来有算力没麦克风，所以就这么分工。流里存的是**自动增益之前**的原始幅度：屏幕上那套 AGC 会把安静和响亮都拉到满量程，绝对电平一丢，"这声音有多大"就没了。
- **Time**：第 1 页时钟，第 2 页秒表/倒计时。
  - **第 1 页 时钟**：**按 `f`（或 `s`）循环切换 4 套表盘**：原版（层次进度条）、指针（经典模拟表盘，~12fps 平滑扫秒）、大字数码（7段数码管，500ms 闪烁冒号）、QlockTwo 文字（极简英文字词高亮），选择自动存 NVS，开机保持。
  - **第 2 页 秒表/倒计时**：`m` 切秒表/倒计时，`Enter` 起停，`l` 计次，`r` 归零。倒计时预设（没在跑的时候可调）：`[`/`]` ∓1 分钟，`-`/`=` ∓10 秒（范围 10s~99min）。⚠️ **不是方向键**——这一页在翻页链里，`;.,/ ` 四个键会被翻页逻辑先吃掉，所以跟 GNSS 配置页用同一套 `[ ] - =`。
  - **闹铃全局安全消音**：倒计时到点触发闹铃时，在**任何页面**按键盘任意键或物理 G0 键均可立即全局消音停止，不会误触发当前页面的按键操作（主循环优先拦截）。
  - **外设避让与 LED 报警**：闹钟鸣叫前检查音频状态，若麦克风（Spectrum）、音频播放器（Player）或网络电台（Radio）正在使用中，自动降级为 LED 红光闪烁警示，不强占扬声器避免破音与冲突；若频谱页已接管 LED 律动，闹钟主动避让不抢占 LED。
- **Astro**：三页全是本地算的，**一个字节网络都不要**（晨昏线连位置都不需要，
  日照只要一对经纬度，没 GPS 时会退回一次 IP 定位）。
  - **日照**：太阳高度角曲线 + 日出/正午/日落/昼长。日出日落按太阳中心高度角穿过
    **−0.833°** 算（大气折射 34′ + 视半径 16′，天文学通用定义），不是 0°。
    正午和左上角那个 `max NN°` 是**直接在曲线上求极大**，不靠日出日落取中点——
    极昼/极夜没有穿越，把时区设成跟当地经度对不上的那一档（比如人在国内、
    时区选 UTC）也会让日出日落在本地这一天里配不成对，中点法在这两种情况下都会失效。
  - **月相**：相位、月龄、照亮比例、下次满月/新月
  - **晨昏线**：世界地图上的昼夜分界 + 当前太阳赤纬
  - 三页共用 `r` 重新定位
- **Crypto Suite（密码工坊 / 赛博暖手宝）**：多算法密码学与加密解密实验套件。按 `m` 在五大子模式间轮换：
  - **Mode 0 烘焙跑分（BENCH OVEN）**：双核满载压力测试，中央高亮 H/s 实时转速表、单 Hash 毫秒延迟、动态电热丝动画与电池毫安功耗遥测；`Enter` 起停，`d` 切换单核 / 双核 Turbo 模式，`r` 归零。
  - **Mode 1 对称密码（SYMMETRIC CIPHER）**：现代对称加密与解密。`a` 切换算法（**AES-128-CBC**、**RC4 (ARC4)**、**XOR (OTP)**）；`t` 切换 **ENCRYPT（加密）** 与 **DECRYPT（解密）**；`;`/`.` 切换明文/密文预设，`,`/`/` 切换密钥，实时输出 Hex 密文或还原明文，`s` 吐至 USB 串口。
  - **Mode 2 古典/CTF密码（CLASSIC & CTF）**：常用古典密码学与编码工具。`a` 切换算法（**Caesar/ROT**、**Vigenère**、**Base64**、**Hex**）；凯撒模式下按 `[`/`]` 可在 1~25 间无级调整字母位移（自动标出 ROT13）；维吉尼亚与 Base64/Hex 模式支持按 `t` 快速切换 Encode/Decode，`s` 吐至串口。
  - **Mode 3 哈希与Shadow（HASH & SHADOW）**：`a` 切换散列算法（**$6$ SHA-512-crypt** 5000轮、**$1$ MD5-crypt** 1000轮、**SHA-256**、**MD5**）；实时进度条与耗时统计，`Enter` 计算，`s` 吐至串口。
  - **Mode 4 荒谬穷举挑战（FUTILITY CRACK）**：`[`/`]` 切换密码空间（从 4 位纯数字 12 分钟，到 8 位大小写+数字 5.2 亿年）；`Enter` 开始实机穷举比对；**各档位均配备真实破解判定**：底层生成算法适配对应字符集（数字/小写字母/大小写字母数字），5 档预设均支持真实破解匹配（0123 / card / cardpt / infinity / entropy9），命中即触发 `TARGET PWNED!` 炫酷徽章，不再只是 0 档演示；任务生命周期安全管理（后台协程优雅停止与世代计数器废弃在途批次，防 UAF 与死锁）。

---

## 串口调试通道

`pio device monitor` 里直接敲指令（实现见 `src/serial_cmd.cpp` 的 `handleSerialCmd`）。
单个字符会原样当按键喂给 UI，所以不碰实体键盘也能操纵整机。

| 指令 | 作用 |
|---|---|
| `HELP` / `?` | 列出全部指令 |
| 单字符 / `ENTER` `BACK` `UP` `DOWN` | 当按键喂给当前界面 |
| `BOOTANIM` | 播放赛博朋克开机动画并输出整屏 RGB565 hex 截图 |
| `BOOTPOST` | 运行硬件自检 (POST AUDIT) 控制台并输出截图 |
| `BOOTFRAME [t]` | 步进绘制开机动画第 t 毫秒画面（t=0..1500）并截屏（逐帧调优） |
| `BOOTSND [ON\|OFF]` | 查询或配置开机音效偏好（存 NVS） |
| **`SHOT`** / `SHOTRAW` | **串口截屏**（RGB565 hex，配 `tools/shot.py` 存成 PNG）；SHOTRAW 为裸流快速截屏 |
| `MENU` / `NETPROBE` / `DNSFUZZ` / `GNSSMAP` / `HASHOVEN` | 跳到指定界面并初始化 |
| `LORA` / `WIFICHAN` / `RADIO` | 直跳对应 App 并初始化对应外设 |
| `BTSCAN` / `BTDEV` / `BTRADAR` | 直跳蓝牙扫描 / 选中设备详情 / 找物声呐雷达 |
| `GOTO n` | 按 `Screen` 枚举序号直跳任意界面（调试用：只有取数类页面会调 enter，且**故意不 cleanup**，会打一行 `[jump]` 提醒） |
| `GPS` | GNSS 收数诊断：字节数 / 校验和通过与失败数 / 静默时长 / 定位状态 |
| `PROBE host[:port]` | 一次性 TCP 连通性探测（排查"到底是网络不通还是内存不够"的利器） |
| `STAT` | 只读状态快照：WiFi 状态 / 堆余量 / 最大连续块 / loop 任务栈余量 / 熄屏状态（`screenOffOnArrival` = 这条指令到达时屏是不是灭的，唤醒前的快照）（不像 `WIFI` 那样会顺手触发连接） |
| `WIFI` / `WIFIOFF` | 用 NVS 里的凭据直连 / 断开并关闭（会挂起看门狗，否则下一帧就被拉回来） |
| `HGET` / `HPOST` / `TCPHEX` | 原始 HTTP / 裸 TCP 请求 |
| `CAT path` | 读 SD 卡文件（主要看 netprobe.log / dnsfuzz.log） |
| `RIDSCAN [秒] [信道]` | Remote ID：混杂模式扫管理帧，**解码 ASTM ODID 和国标 GB 两套**（编号/登记号/经纬度/高度/速度/航向/运行状态/遥控站位置），同时打出原始 vendor IE + NAN action 帧 + AP 列表。给了信道就蹲死不跳频（查 NAN 必须蹲 ch6；实测国产机在 ch6） |
| `BTDUMP [秒]` | BLE 取样：扫一遍，打出每个设备的 Service Data / Manufacturer Data |
| `BTEXT [秒]` | BLE **扩展广播**取样（BT5 Long Range / Coded PHY）。BTDUMP 那套 API 看不见这一类 |
| `RFSCAN [秒]` | 2.4G 逐信道 802.11 活动普查（帧数/峰值 RSSI/噪声底），找空信道用。⚠️ 看不到非 Wi-Fi 信号——原本是为探测大疆 O4 图传写的，实测否掉了：混杂模式只在认出 802.11 前导后才回调，O4 那类私有调制连失败事件都不产生 |
| `TRAIL MARK n` | 手动埋一个字节（十进制或 `0x..`）。用来**自证工具是好的**：埋一个 → 按复位 → 看开机回放里还在不在。平时轨迹读出来全是零，没有这条就永远没机会验证「RTC 内容扛得过复位」这个前提 |
| `TRAIL` | 打印**面包屑轨迹**——查"整机卡死但看不见现场"用。在可疑路径上撒 `bcMark(n)`，卡死后复位，开机自动回放（存在 RTC 内存里，软复位不丢）。判读看**哪个编号没出现**。这套设施是查 BLE deinit 阻塞时逼出来的，因为串口插桩本身会把竞态窗口撑开、让故障消失，详见 [docs/ble-teardown.zh-CN.md](docs/ble-teardown.zh-CN.md) |
| `LS [path]` | 列 SD 目录（名字 + 字节数），默认根目录。串口通道一直有 `CAT` 却没有 `LS`，文件名猜不出来的时候（比如 `/battlog` 里到底有几个日志）就卡死了 |
| `BATT [n]` | 连采 n 次电池电压（默认 64），打出 min/中位/max/峰峰/标准差 + 内部状态（ema / charging 猜测 / level）。**顺带把三个"插没插"的硬件信号都读一遍**：`isCharging()` / `getBatteryCurrent()` / `getVBUSVoltage()` —— 本机三个全是无效值（unknown / 0 / -1），所以电量只能从电压反推 |
| `BATTLOG ON\|OFF` | 电池曲线每 10 秒写一行到 SD `/battlog/*.csv`。**开关存 NVS、重启保持**——因为它存在的全部意义就是记录"串口断开之后"发生了什么，那时你没法再发指令。⚠️ 必须在**拔线之前**打开 |
| `RAMLOG` | **开机各阶段的内存轨迹**，带逐步 Δ。`STAT`/`MEMCAP` 答的是"此刻还剩多少"，这条答的是"**是哪一步**吃掉的"——上面那 24.5KB 静态 RAM 涨到哪去了，只有这种表回答得了。⚠️ 是先攒在 RAM 里、随时 dump，不是当场打：`Serial.begin()` 排在 `setup()` 很靠后，而 `M5.begin()` 和主画布那 64KB 都在它之前，当场打的话最想看的两步恰好全丢 |
| `MEMCAP` | 分池看内存（8BIT/INTERNAL/DMA/32BIT）+ **碎片形态**（空闲散在几个块里、已分配几块）。调试条那个 largest 只是 8BIT 的；而 free/largest 两个数分不出"泄漏"和"纯碎片"，块数能 |
| `RIDFAKE` | 把真机抓到的两帧 Remote ID 灌进 Drone ID 页，没无人机时用来核对排版。注入的条目会打上洋红 `FAKE` 徽标 + 顶部横幅，跟真检测区分开 |
| `RANDMAC` / `SETMAC` | 改 STA MAC |
| `FONT [12\|14\|16]` | 切换 TXT 小说阅读器字号 |

---

## 电脑端配套工具

详见 [tools/README.zh-CN.md](tools/README.zh-CN.md)。八个：

- **`tools/shot.py`** —— 串口截屏存 PNG。板子插着时这是唯一能"真看到屏幕"的办法，调 UI 全靠它：
  `python3 tools/shot.py --cmd GNSSMAP --scale 3`
- **`tools/uisim/`** —— 桌面 UI 模拟器，**直接编译 `src/` 里那份真绘制代码**，在 Mac 上离屏渲染成 PNG。
  板子不在手边时用来核对布局有没有重叠；只能跑不依赖硬件的页面
- **`tools/lora_view.py`** —— LoRa 包实时表格化查看器
- **`tools/spec_view.py`** —— 声学采集：把 Spectrum 页的频谱流拉回电脑，存 ndjson +
  渲染语谱图（不依赖任何第三方库，PNG 是手写的）。`--render` 可以只渲染已有日志
- **`tools/spec_analyze.py`** —— 声学分析：`spec_view.py` 只管采集，这个脚本读它存的
  ndjson 做语义判断——噪声基线、有没有持续性声源、窄带音（哨音/嗡鸣，多少赫兹）、
  环境分类、时段切分，`--json`/`--png`（带刻度语谱图）可选。依赖 numpy（采集端刻意
  不依赖是为了在现场好装，分析端不用顾虑这个）。⚠️ 固件端 5 帧/秒 × 16ms 窗，占空比
  约 7%，能看"环境声学性格"和"持续声源"，看不了单次瞬时事件、说话内容/人声、
  10~25Hz 旋翼调制（详见脚本内文档字符串）
- **`tools/rid_view.py`** —— 无人机 Remote ID 实时查看器。进 Drone ID 页后固件会流式吐
  `RIDPKT {json}`，这个脚本表格化显示，并可 `--snapshot` 持续写一份"每架无人机的最新
  状态"文件。**存在的理由是延迟**：串口那条 `RIDSCAN` 是阻塞式的（扫 N 秒、最后一次性
  打印），在无人机飞行这种"环境每几秒就变"的场景下，每次采集都是一段失明期。改成流式
  之后任何时刻想知道当前值，读一下快照即可
- **`tools/odidtest/`** —— Remote ID 解码器测试台，同样编译 `src/` 那份真代码。手上没有会播
  RID 的无人机，而解析器最容易错的就是字段偏移（差一个字节照样编译通过、照样"解出"一个
  看着像模像样的坐标），只能靠已知输入比已知输出
- **`tools/powertest/`** —— 电量/充电逻辑测试台，同样编译 `src/` 那份真代码。存在的理由是
  板子必须插着 USB 才能烧录/读串口，而这套逻辑最要紧的分支恰恰是"拔掉之后"，真机测不了
- **`tools/ramtest/`** —— `RAMLOG` 那张表的试跑台，同样编译 `src/ram_profile.cpp` 那份真代码，
  只把它底下的 `heap_caps_*` 换成一串编好的数（含一段"内存反而变多"的回收，专门试 Δ 的正号）。
  验的是**表打出来长什么样**：列对不对齐、正负号有没有反、16 格灌满之后丢的是不是新的。
  这些在板子上要烧一次才看得见。另外 uisim 的语法检查会给它单开 `-Werror=format`——
  格式串跟参数类型对不上在板子上是**静默**的（照样烧得进去，只是打出来的数是垃圾）

---

## 踩过的坑

这块板子没有 PSRAM，主画布本身就吃掉 64KB 堆，所以**内存是绝大多数问题的根源**。
下面几条都是实机调出来的，代码注释里也都写着，别再踩一遍：

**G38 是背光和 RGB LED 电源共用的。** M5GFX 用 LEDC PWM 占着它控背光，自己
`pinMode`/`digitalWrite` G38 会把背光钉成常亮 → 亮度调节失效、自动熄屏关不掉屏，
表现像"卡死但屏不灭"。LED 只驱动数据脚 G21。详见 [HARDWARE.zh-CN.md](HARDWARE.zh-CN.md)。

**GNSS 串口的 RX 缓冲必须放大。** 默认 256 字节在 115200 波特下只装得下 22ms，而主循环一帧
（render + `delay(20)`）本来就要 30~50ms —— 等于每帧都在丢字节，NMEA 语句被拦腰截断。
`setRxBufferSize(4096)` 必须在 `begin()` **之前**调。

**PNG 在这块板子上解不出来。** deflate 要一块 32KB 连续内存装 LZ77 窗口（格式规定，压不下来），
而实测 free heap 只剩 42KB 且 largestBlock 只有 24KB。所以 GNSS 地图用的是**高德卫星影像
（JPEG）**而不是更好看的路网图（只提供 PNG）—— tjpgd 按 MCU 块流式解码，几 KB 就够。

**别把大缓冲从堆搬到 `.bss` 想给堆腾地方。** 两者是同一块 DRAM，搬过去只是让它变成永久占用、
堆池同步缩小，实测 largestBlock 反而从 24KB 掉到 16KB。

**图片解码不要先 malloc 整块压缩数据。** z8/z11 的瓦片有 26~35KB，那会儿 largestBlock 只剩 11KB
→ malloc 失败 → 只画出半张图。用 `drawJpg(Stream*)` 直接从 HTTP 流里解。

**碎片整理在这块板子上做不到，但多数时候你要的也不是它。** 碎片整理需要移动已分配的块，
移动就要更新所有指向它的指针——这要求句柄式分配（老 Mac OS / Palm OS 那套）或带精确指针
映射的 GC。而 mbedTLS、Arduino String、ArduinoJson、BLE 协议栈全都直接持有 malloc 来的
裸指针，没人知道谁指着哪块，所以谁都不能动。这是 C 堆的固有性质，不是 ESP-IDF 的缺陷。

用 `MEMCAP` 的块数可以判别是三种情况里的哪一种：

| 现象 | 结论 | 该查什么 |
|---|---|---|
| 空闲总量不回来 | 真泄漏 | 谁没 free / 谁没析构 |
| 空闲回来了、空闲块数涨、**已分配块数也涨** | 泄漏造成的碎片 | 多出来的那些块是谁分配的 |
| 空闲回来了、空闲块数涨、已分配块数没涨 | 纯碎片 | 分配顺序，找长寿命的中等对象 |

实测蓝牙来回 2 次：空闲 73460→80032（**反而涨了**），最大块 63476→18420，
空闲块 10→37，已分配块 **401→593**。多出的 192 块永不释放，像钉子一样把连续空间钉碎——
所以是第二种，根子是泄漏不是碎片。修好析构之后 largest 就从"持续下降"变成"收敛"了。

真正能做的是：① 别产生钉子（修泄漏）；② 大而长寿的先分配（那 64KB 主画布开机就占住、
永不释放，看着吓人其实是最优摆放，它在堆底一动不动不参与碎片化）；③ 降级阶梯
（地图 48KB→8bpp 24KB→离线世界图，不假设能拿到大块）；④ 重启——在 MCU 上这是合法策略。

**HTTPS 握手峰值吃掉近 70KB，而我们总共只有 74KB。** 2026-08-09 实测（ChatGPT 页）：
握手前 `minEver=70284`，握手失败后 `minEver=4236`——**整个堆被抽干到 4.2KB**，
于是连 SHA 那个 128 字节的 DMA 小缓冲都申请不到（报 `esp-sha: Failed to allocate buf memory`）。
一度怀疑是 DMA 内存单独紧张，用 `MEMCAP` 排除了：DMA 池跟 8BIT 基本是同一个，
刚复位时都有 63KB 连续块。真正的元凶是 `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384`
带来的两个 16KB I/O buffer，加上证书链解析的临时对象。所以 ChatGPT 页是**临界可用**：
偶尔成功、多数失败。想调那个配置得重编 framework（Arduino 的 mbedtls 是预编译的），
治本方向是减少请求时刻的常驻内存。

**TLS 既费内存又费时间。** mbedTLS 握手要 40~50KB 连续堆，地图页那会儿根本挤不出来
（`start_ssl_client: -1`，而同一台机器 `PROBE 443` 是 8ms 通的）；握手本身还要几百毫秒。
所以**公开数据 + 请求不带凭据**的都走明文 HTTP：地图瓦片、ADS-B、天气、IP 定位。
实测 ADS-B 改明文后握手期间的 `largestBlock` 不再从 90KB 掉到 47KB。
**保留 HTTPS 的有三处**：一是 Sats——N2YO 把 API key 塞在 URL 里，走明文等于把 key
明着发出去；二是 ADS-B 的**航线查询**（adsbdb.com，起降机场 + 行程进度）；三是 **Typhoon**
（jma.go.jp，台风实况与路径）；四是 **Quake**（earthquake.usgs.gov）。后三个都只提供
HTTPS、明文会 301/跳转，也都不带凭据。Typhoon 和 ADS-B 航线只在选中某一项时才查、
响应几百字节到几 KB，那次握手的代价可以接受；Quake 的响应大得多，所以它是**逐元素流式
解析**的——握手的峰值躲不掉，但至少不再叠上一个整包 JSON（见 `src/quake.h`）。

> ### 安全边界（明确写清楚，别只当成性能取舍）
>
> 这是个玩具项目，网络这块有三条已知的、有意为之的弱点：
>
> 1. **明文 HTTP 的响应可被篡改。** 地图瓦片 / ADS-B / 天气 / 空气质量 / IP 定位走的都是
>    明文。这几个不带凭据，所以泄密风险为零；但同网段的人可以改返回内容，让你看到错误的
>    位置、天气或地图。对这个项目的用途来说可以接受——**但它是完整性风险，不只是省内存**。
>
>    **一个例外：Router 页是明文 + 带凭据的。** `CLASH_BASE` 是 `http://`，而每次轮询都会
>    发 `Authorization: Bearer <CLASH_SECRET>`（`router.cpp`）。这个 secret 只在局域网内
>    传输、也只能用来读你自己路由器的流量统计，但"明文的都不带凭据"这句话对它不成立，
>    别把它当成零风险。
> 2. **HTTPS 全部真校验了（2026-08-26 起）。** 以前 `sats.cpp` / `github.cpp` /
>    `adsb.cpp` 的航线查询 / `typhoon.cpp` 用的是 `client.setInsecure()`，只挡得住被动
>    窃听、挡不住主动中间人——而 **Sats 尤其要紧**，N2YO 把 API key 塞在 URL 里，
>    拿张自签证书就能收下那个 key（当时记作"保留 HTTPS 的理由只兑现了一半"）。
>
>    现在改成**根证书 bundle**：121 张 Mozilla 根证书编进 flash（约 56KB，见
>    `platformio.ini` 的 `board_build.embed_files` 和 `src/tls_ca.h`）。RAM 开销很小，
>    只有索引常驻。
>
>    Chat 的三档策略（从严到宽）：填了 `CHAT_ROOT_CA` → 证书钉扎；否则 → bundle 真校验
>    （**现在的默认，不用再手贴证书**）；显式把 `CHAT_ALLOW_INSECURE` 改成 `true` → 降级，
>    每次往串口打警告。当初不肯硬编证书的理由是"根证书会过期，到期后设备就连不上"——
>    bundle 正好解决它：过期一张还有其余 120 张，换 CDN 换 CA 也照样验得过。
>
>    ⚠️ **代价：证书链校验有额外的堆峰值。** GitHub 页当场被压垮过——它同时背着
>    TLS 常驻缓冲 + 校验峰值 + 一整年 15.5KB 的 JSON，报 `json: NoMemory`，
>    `STAT` 里 `minEver=584`（堆只剩 584 字节）。改成逐元素流式解析 JSON 之后
>    回到 3948，能跑但仍然紧。**这一页现在是 TLS 内存压力的哨兵**——哪天它再报
>    NoMemory，就该去动 `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`（收发缓冲不对称，
>    收 16KB 发 2KB，能省约 14KB，但要重编 framework）。
>
>    ⚠️ **新引入一个失败模式：校验证书要比有效期，所以必须先对上时间。** 开机后 NTP 是
>    后台异步跑的，那几秒里系统时钟停在 1970，任何证书都会被判成"还没生效"。这几页都加了
>    `tlsClockReady()` 守卫，会明说 `waiting for clock (NTP)`，不会混成网络错误。
>    代码里不硬编证书：根证书会过期，到期后设备就连不上了。
> 3. **`src/secrets.h` 里的 key 会被编进固件。** 文件本身 gitignore 了、也从没进过
>    git 历史，但 `.pio/build/*/firmware.bin` 里是明文——**别把编好的固件发给别人**，
>    要发就先用占位 key 重编一次。
>
> 想收紧的话，代价是：给这几个域名各带一份根证书（flash 占用 + 证书过期后设备会连不上），
> 或把 key 改成设备端配置、不进固件。
**一次 HTTPS 就能把堆永久打散，后面的大块申请全遭殃。** 实测（板子上 `STAT` 读的）：冷启动
`largestBlock` 有 94KB，进 GNSS 地图能顺利拿到 48KB 底图缓存；但**只要发生过一次 TLS 握手**
（Sats / Typhoon / Chat 任意一个），`largestBlock` 就永久掉到 41~47KB —— 注意总空闲量几乎没变
（100KB→98KB），少掉的不是内存而是**连续性**。48480 这个需求正好卡在门槛上方，症状就是
"逛过 Sats 之后地图缩放莫名其妙失灵"。所以大块缓存一定要有降级档（地图退 8bpp），
而且**降级了要在界面上说出来**——原来的写法在缓存申请失败时把 z 强制回 0，角标却还照着
zoomIdx 显示 `z8`，画的是世界图、标的是 z8、按 `[ ]` 只有标签在动，看着就像功能坏了。

**别在按键回调 / 绘制路径里同步拉网络。** `handleKey` 和 `render` 都在 `loop()` 里，
一发 HTTPS 请求要一秒上下，期间键盘根本轮询不到——表现就是"进这一页卡一下"。
Planes/Sats/Router 的进入和刷新都改成了"先把 loading 那一帧推上屏，再由 loop 里的 update 去拉"。

**耗时操作不能放在绘制路径里。** `render()` 是在 `loop()` 里调的，在里面同步拉网络会把主循环
整个卡住，键盘都轮询不到 —— 表现就是"整机卡死"。瓦片改成了状态机，每帧最多推进一步。

**16 位的 `TFT_*` 宏和 24 位十六进制字面量不能混在同一个返回类型里。** LGFX 见到 `uint32_t`
会按 RGB888 解释，于是 `TFT_WHITE`(0xFFFF) 被画成青色、`TFT_CYAN` 画成蓝色。要么全用
`uint16_t` + `color565()`，要么全用 24 位。

**国内可达性：** Carto / OpenStreetMap 的瓦片服务实测 TCP 443 直接超时，高德 8ms 就通。
高德用 GCJ-02 坐标，而 GPS 给的是 WGS-84，差 300~600m，必须换算（`wgs2gcj()`）。

**TinyGPS++ 的 `isValid()` 不等于"此时此刻有定位"。** TinyGPS++ 内部只要历史上曾经成功解析出一个定位点，
`gps.location.isValid()` 就会永久为 true。在进隧道、进室内彻底失锁后，它不会自动变 false。如果直接拿它当定位有效判据，
所有页面都会继续显示最后一次定位的陈旧经纬度；更严重的是速度：室内只有微弱反射信号时 HDOP 会暴涨到十几甚至几十，
经纬度在百米范围内疯狂漂移，模块内部计算出的对地速度会凭空报出三四十 km/h，静止放在桌上"狂飙"。所以必须双重设防：
① 判定定位必须检查新鲜度（`location.age() < 3000`）；② 速度与航向只在定位新鲜且 `HDOP <= 8.0` 时才视为可信（`gnssSpeedTrusted()`），
不满足一律显示 `n/a`。

**HTTP/1.1 分块传输编码（Chunked Transfer）在无 PSRAM 设备上的内存杀伤力。** 服务端如果在响应里带
`Transfer-Encoding: chunked`，客户端必须在内存里维系 chunk 解码缓冲，对于 ChatGPT 等流式响应很容易多吃 12KB 以上动态堆，
在仅剩几 KB 的 TLS 握手现场直接引发 OOM。解决方案是向服务端明确发 `HTTP/1.0` 请求头，强制服务器关闭 chunk 编码，
直接以确定长度传输，把 chunk 缓冲彻底抹掉。

**Wi-Fi 模式切换的时序窗口与混杂模式独占性。** ESP32 的 Wi-Fi 协议栈在模式切换（`WiFi.mode()`）时是异步的，
调用后必须等待 `WiFi.getMode()` 实际生效且协议栈状态位就绪（`waitStatusBits(STA_STARTED_BIT)`），否则立刻调 `scanNetworks()` 会百分百静默失败。
此外，`esp_wifi_set_promiscuous(true)` 开启混杂模式（Drone ID / Sniffer）会霸占无线射频，与 SoftAP 热点和常规连接不能并存。
必须在独占前通过 `hotspotSuspend()` 安全暂挂 AP，退出时 `hotspotResume()` 恢复；而普通扫描（WiFi Chan / Wardrive）
则可以用 `WIFI_AP_STA` 模式实现与热点无缝共存。

**I2S 音频与麦克风总线互斥。** ES8311 编解码器在录音（Spectrum）与播放（Player / Radio）之间切换时涉及模拟电路下电/上电瞬态，
易产生直流爆音；更重要的是音频硬件总线同一时刻只能由一个功能持有。如果后台倒计时闹钟在电台或播放器放歌时直接调 `Speaker.tone()`，
会导致总线冲突、破音甚至任务死锁。倒计时闹铃必须在鸣叫前通过 `isAudioBusy()` 侦测外设占用，有占用时主动降级为红光 LED 闪烁，绝不硬抢音频。

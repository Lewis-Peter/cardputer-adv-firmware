[English](feature-inventory.md) | **简体中文**

# 功能清单（Cardputer ADV 固件）

从代码里逐个扒出来的，不是从 README 抄的。口径：
**40 个 app / 85 个 Screen / 7 个分组**。

读法：
- **子页** = 在 `pages.cpp` 的翻页链里，用 `.`/`/` 前后翻，底部有页码点
- **子模式** = 同一个 Screen 里靠状态切换的不同形态（不占 Screen 名额）
- **子屏** = 独立 Screen 但不在翻页链里（靠 Enter 进、`` ` `` 退）
- 通用键不重复列：`;.,/` 导航、`Enter` 确认、`` ` `` 返回、`G0` 回主菜单

---

## 当前分组与余量

| 组 | 已用 | 余量 | app | 归组属性 |
|---|---|---|---|---|
| **TOOLS** | 8/8 | **0** | Time · IMU · Files · Spectrum · Calc · Converter · Player · Hash Oven | 本地工具，不依赖射频 |
| **SCAN** | 5/8 | 3 | WiFi Chan · Sniffer · Wardrive · Drone ID · BLE Scan | 被动找东西（混杂模式/扫描，不需要连上） |
| **NET** | 5/8 | 3 | Hotspot · LAN Scan · NetProbe · Router · SSH | 主动网络收发（需连上或自己当 AP） |
| **HID** | 3/8 | 5 | BT Keys · BT Media · Ducky | 设备当键盘/遥控器敲别人（BLE/USB） |
| **SIG** | 4/8 | 4 | LoRa · GNSS · Map · IR | 其它收发通道（隔空信号与定位） |
| **SKY** | 6/8 | 2 | Astro · Weather · Planes · Sats · Typhoon · Quake | 抬头看天（天文/气象/天灾/空中交通） |
| **MORE** | 8/8 | **0** | ChatGPT · GitHub · Radio · FX · OKX · Reader · BadApple · Settings | 个人服务仪表盘、多媒体与系统设置 |

⚠️ **每组硬上限 8（4×2 网格，每组正好一屏，不做上下滚动）**。TOOLS 和 MORE 已经满员（8/8）。
总容量 56、已用 39，余量分布在 SCAN(3) / NET(3) / HID(5) / SIG(4) / SKY(2)。

---

## TOOLS（本地，不依赖射频）

### Time — 2 子页（原 Clock + Timer 合并）
1. **时钟**（`SCREEN_CLOCK`）：走动时钟 + 日期。**按 `f`（或 `s`）循环切换 4 套表盘**：原版（层次进度条）、指针（经典模拟表盘，~12fps 平滑扫秒）、大字数码（7段数码管，500ms 闪烁冒号）、QlockTwo 文字（极简英文字词高亮），选择自动存 NVS，开机保持。
2. **秒表 / 倒计时**（`SCREEN_STOPWATCH`）：
   - `m` 切 **秒表 / 倒计时**
   - `Enter` 起停 · `l` 计次（秒表，上限 `LAP_MAX`）· `r` 归零
   - 倒计时未跑时可调预设：`[`/`]` ±1 分钟，`-`/`=` ±10 秒，范围 10s~99min（⚠️ 不是方向键，因翻页链优先吃掉方向键）
   - **闹铃全局安全消音**：响铃时在**任何页面**按任意键或物理 G0 键均可立即全局消音，不触发原本按键逻辑
   - **外设避让与 LED 报警**：麦克风/播放器/电台在使用中时，改用 LED 红光闪烁警示，不抢喇叭避免破音与冲突；若频谱页已接管 LED 律动，闹钟主动避让不抢占 LED
   - 退出不清计时（基准是 `millis()`），后台持续走时

### IMU — 2 子页
1. **指南针**
2. **详情**（`SCREEN_COMPASS_DETAIL`）

### Files — 1 屏 + 1 子屏
- 目录浏览、进出目录、`` ` `` 逐级上退（保留文件列表与光标，不重复扫目录）
- `Enter` 开 `.txt` → **阅读器**子屏（`SCREEN_READER`）
- `退格` 删文件（带二次确认）
- 阅读器：`.`/`/` 翻页，`[`/`]` 快退/快进 ~5%，退出时存进度

### Spectrum — 1 屏
- 麦克风 256 点 FFT 音频频谱仪
- **`W` 键切换瀑布图与柱状图**：按 `W` 切换**瀑布图（语谱图 / Spectrogram）**与**经典柱状图（Bars）**。瀑布图采用局部滚屏技术，60 dB 自适应动态范围映射 AGC 前原始分贝，过滤 DC 直流分量，仅在新 FFT 帧到达时平滑推进行，黑→蓝→青→绿黄→橙红→白伪彩色渐变；柱状图显示 32 频段能量与白色峰值保持线
- `l` 切"麦克风驱动 LED 律动"（通过 `ledSetOverride` 接管板载 RGB LED，随现场音量动态映射换色）
- `s` **频谱流式输出**到串口（`SPEC {json}`，5 帧/秒），配 `tools/spec_view.py` / `spec_analyze.py`

### Calc — 1 屏
- 表达式计算，`Enter`/`=` 求值，`退格` 删（算式空了顺手清结果）
- 求值后再输运算符＝接着上次结果算，输数字＝重新开始

### Converter — 1 屏
- `,`/`/` 切类别 · `;`/`.` 切 FROM 单位 · `[`/`]` 切 TO 单位
- 数字输入，`.` 有输入时当小数点、无输入时当切单位（同键分流）

### Player — 1 屏
- SD 卡 WAV 播放，列表选择，`Enter` 播/停
- 缓冲播放动态计算等待空位超时（防止 8kHz 单声道低码率提前退出）；协作式退出防 FATFS 锁死；支持音量调节、自动切歌与实时 VU 仪表

### Hash Oven — 1 屏（密码工坊 / 赛博暖手宝）
- 多算法密码学与加密解密实验套件，按 `m` 在五大子模式间轮换：
  - **Mode 0 烘焙跑分（BENCH OVEN）**：双核满载压力测试，中央高亮 H/s 实时转速表、单 Hash 毫秒延迟、动态电热丝动画与电池毫安功耗遥测；`Enter` 起停，`d` 切换单核 / 双核 Turbo 模式，`r` 归零
  - **Mode 1 对称密码（SYMMETRIC CIPHER）**：现代对称加密与解密。`a` 切换算法（**AES-128-CBC**、**RC4 (ARC4)**、**XOR (OTP)**）；`t` 切换 **ENCRYPT（加密）** 与 **DECRYPT（解密）**；`;`/`.` 切换明文/密文预设，`,`/`/` 切换密钥，实时输出 Hex 密文或还原明文，`s` 吐至 USB 串口
  - **Mode 2 古典/CTF密码（CLASSIC & CTF）**：常用古典密码学与编码工具。`a` 切换算法（**Caesar/ROT**、**Vigenère**、**Base64**、**Hex**）；凯撒模式下按 `[`/`]` 可在 1~25 间无级调整字母位移（自动标出 ROT13）；维吉尼亚与 Base64/Hex 模式支持按 `t` 快速切换 Encode/Decode，`s` 吐至串口
  - **Mode 3 哈希与Shadow（HASH & SHADOW）**：`a` 切换散列算法（**$6$ SHA-512-crypt** 5000轮、**$1$ MD5-crypt** 1000轮、**SHA-256**、**MD5**）；实时进度条与耗时统计，`Enter` 计算，`s` 吐至串口
  - **Mode 4 荒谬穷举挑战（FUTILITY CRACK）**：`[`/`]` 切换密码空间（从 4 位纯数字 12 分钟，到 8 位大小写+数字 5.2 亿年）；`Enter` 开始实机穷举比对；**各档位均配备真实破解判定**：底层生成算法适配对应字符集（数字/小写字母/大小写字母数字），5 档预设均支持真实破解匹配（0123 / card / cardpt / infinity / entropy9），命中即触发 `TARGET PWNED!` 炫酷徽章，不再只是 0 档演示；任务生命周期安全管理（后台协程优雅停止与世代计数器废弃在途批次，防 UAF 与死锁）

---

## NETWORK（Wi-Fi 射频收发与网络）

### WiFi Chan — 1 屏 + 1 子屏
- 信道占用分析，`r` 重扫，`a` 自动扫描开关
- `Enter` → **详情**子屏（`SCREEN_WIFI_CHAN_DETAIL`，可在详情里继续 `;`/`.` 换信道）
- **热点共存**：当板载热点（Hotspot）已开启时，自动采用 ESP32 的 `WIFI_AP_STA` 混杂共存模式扫描，**不再强关热点**，连着热点的设备不掉线；退出后切回纯 AP 模式

### Sniffer — 1 屏
- 混杂模式抓包统计，`r` 清计数
- **热点暂挂**：混杂模式独占射频期间通过 `hotspotSuspend()` 自动暂挂热点并显示状态，退出时通过 `hotspotResume()` 自动恢复

### Hotspot — 1 屏 + 2 子屏
- `Enter` 开关 AP · `p` → **改密码**子屏（`SCREEN_HOTSPOT_PW`，≥8 位才收） · `q` → **二维码**子屏（`SCREEN_HOTSPOT_QR`，进入时自动开 AP）
- 主界面实时显示已连客户端 DHCP 租约 IP 与 MAC 列表
- **扫描共存与暂挂恢复**：支持在 Wi-Fi 扫描（Settings）、WiFi Chan 和 Wardrive 扫描时以 `WIFI_AP_STA` 混杂共存模式保持在线，连入设备不掉线；在 Drone ID / Sniffer 独占射频期间自动暂挂，退出后无感恢复；LAN Scan 仅扫描 STA 网段不影响热点

### LAN Scan — 1 屏
- 局域网设备发现（ARP 缓存 + ICMP Ping 扫描已连 STA 网段）+ 反向 DNS / mDNS 取真实设备名
- `Enter` 重扫，列表上下
- **不影响热点**：仅探测已连接的 STA 局域网，不需要独占射频，板载热点开着就保持开着

### NetProbe — 1 屏（两种模式）+ 编辑子模式
- 网络连通性多目标探针与 DNS 压力/模糊测试合并 app，按 `m`/`M`/`Tab` 在两种模式间平滑切换：
  - **Probe 模式**：TCP/DNS/HTTP 多目标连通性轮询（Google/Baidu/Aliyun/Cloudflare/VPN节点/gstatic 204），自动检测 AS32934 投毒 IP 与强制门户跳转，综合判定连通状态。结果写 SD `/netprobe.log`
  - **DNS 模式**：对路由器/网关 DNS（默认对准 DHCP DNS/网关，支持 `g` 一键重对齐）注入 10 种 RFC 畸形用例（自环指针/截断label/超长name/非法保留位/伪造EDNS0/多问题/残缺头/空包/TCP基线/长度不匹配），每次畸形后通过控制查询量化 RTT 延迟；目标无应答时安全自动暂停（`HALTED`），可按 `Enter` 续跑。结果写 SD `/dnsfuzz.log`
- 快捷键：
  - `Enter`：重新运行当前模式；DNS 模式暂停时续跑
  - `m` / `M` / `Tab`：在 Probe 与 DNS 两种模式间切换
  - `a` / `A`：（仅 Probe 模式）自动巡检开关（每 5 分钟轮询一轮）；DNS 模式没有 auto，切模式会关掉 auto，避免无人值守地反复 fuzz 全屋共用的 DNS
  - `e` / `E`：编辑当前模式目标（Probe 改 vpn-node，DNS 改 DNS 目标 IP/端口；两段式，` 取消）
  - `g` / `G`：（仅 DNS 模式）一键对准当前网络默认 DNS/网关
  - `` ` ``：退回主菜单

### Drone ID — 1 屏 + 1 子屏
- 无人机 Remote ID 扫描（ASTM F3411 + 国标 GB 两套解码）
- 列表：编号 / 运行状态(AIR/GND/EMG/LOST) / 距离方位 / 高度 / 信号
- **雷达视图**：`m` 切列表/雷达（北向上、自动量程 100m~50km，需 GNSS 定位）；选中机显示读数、航向线和飞手位置方框
- **接近告警**：`a` 循环告警距离（关/200/500/1000/2000m，默认关），`b` 开关告警声音（默认开）；两项存 NVS。有目标进入圈内或新出现 EMG 时红灯闪 3 秒 + 提示音（进入=上行两声，EMG=三声高音），圈内每 8 秒轻响一声；出圈需退到 1.2 倍距离外才重新布防。仅在 Drone ID 页开着时评估；静音（Debug/音量 0）时只闪灯。
- **运营人 ID 提示**：详情页 `Reg` 行按观察结果着色——观察满 15 秒且收到 ≥8 包仍没有运营人 ID 显示橙色 `not broadcast`；播了但内容像占位符（全空格 / 全相同字符 / 直接抄机身序列号）显示黄色并加 `?`；观察不够久时显示 `--`。**这是观察提示，不是合规判定**，不做任何国家/地区的编号格式校验。串口 JSON 增加 `"opid":"ok|none|suspect"`（pending 时不输出）。
- `;`/`.` 选择 · `r` 清空 · `Enter` → **详情**子屏（完整坐标、速度、航向、飞手位置、登记号）
- 发现目标后锁死信道（跳频会丢约 92% 的包）
- 流式吐 `RIDPKT {json}`，配 `tools/rid_view.py`
- **热点暂挂**：因混杂模式需独占射频，进入时通过 `hotspotSuspend()` 自动暂挂热点并显示状态，退出时通过 `hotspotResume()` 自动恢复

### SSH — 1 屏 + 终端子模式
- 全功能交互式 SSH 终端客户端（基于 LibSSH 协议库）
- **无 PSRAM 内存自愈**：进入连接时自动释放 64.8KB 全屏画布（`cv.deleteSprite()`），SRAM 空闲暴涨至 140KB+ 容纳 24KB 独立线程栈与加密握手堆，断开退出时自动恢复 `cv`
- **VT100/ANSI 硬件直绘终端**：`M5.Display` 硬件字符直绘，零闪烁、刷新极快；完整支持 ANSI 16色/256色/24位真彩色、光标绝对/相对定位、清屏清行、退格滚屏与 UTF-8 盒形图回退
- **字号切换与 PTY 重协商**：在活动终端中按 `Fn + Enter` 循环切换 3 种字号模式：
  - `8x16`（30 列 × 8 行，经典 VGA 大字，字大清晰易读）
  - `8x8`（30 列 × 15 行，宽体中字，兼顾清晰度与行数）
  - `6x8`（40 列 × 15 行，紧凑小字，最大容纳 40 列宽）
  切换时实时向远端重协商 PTY 窗口大小（`TIOCSWINSZ` / `ssh_channel_change_pty_size`），htop、vim 等自适应排版
- **认证方式**：支持密码认证与 SD 卡私钥（RSA/ED25519）自动探测（候选探测 `/id_ed25519`、`/id_rsa`、`/ssh.key` 等常见路径（还可用 `config_local.h` 的 `CFG_SSH_EXTRA_KEYS` 追加），或通过 `/ssh.cfg` 指定 `key=...`；支持私钥 Passphrase）
- **串口终端透传**：电脑 USB 串口连接时，除基本系统控制指令（`STAT`/`MENU`/`REBOOT` 等）外，敲入的 Shell 命令直接原样透传至远程 SSH 终端执行
- **按键映射与连接安全**：`Fn + Tab` ＝ Esc，`Fn + ; . , /` ＝ 方向键，`Fn + [ / ]` ＝ Home/End，`Fn + Backspace` ＝ Delete，`Ctrl + 字母` 组合键；`Fn + \`` 或物理 `G0` 键随时退出（CAS 所有权守卫机制杜绝 session/channel 双重释放）

---

## SIGNAL（Wi-Fi 以外的收发通道）

### LoRa — 4 子模式 + 1 蹲守子态（全在同一个 Screen）
| 模式 | 干什么 | 进入 |
|---|---|---|
| **SCAN** | 扫频找峰值 | `;` 上翻 |
| **LISTEN** | 蹲一个频点收包 | `.` 下翻 / SCAN 里 `Enter` 锁到峰值 |
| **AUTO** | 在当前频点轮流试 SF/BW，蹲中就停 | `a`（再按放弃） |
| **CHAT** | 用私有帧收发消息，几乎所有键当打字 | `c` |

- `,`/`/` 切预设频率 · `r` 清峰值保持/抓包日志/蹲守统计
- SCAN 下 `v` 切 **ISM(863-928) / VHF(161.5-162.5)** 频段
- VHF 下 `a` 开 **AIS 蹲守**（纯 RSSI 能量检测，不解调；写 SD `/ais/*.csv`）
- ⚠️ AIS 蹲守和 LoRa AUTO 共用 `a`，靠"当前是不是 VHF"分流

> 拆分观察：**CHAT 跟其余三个不是一类东西**——它是收发聊天，其余三个是频谱侦察。
> 而且它霸占了几乎所有按键。

### Bluetooth — 3 个并列子屏
1. **Scan devices**（`SCREEN_BT_SCAN`）→ **设备详情**（`SCREEN_BT_DEVICE`，含 Service Data / Remote ID 识别）→ **找物雷达**（`SCREEN_BT_RADAR`，RSSI 曲线 + 五声音阶提示音，`m` 静音 `r` 重置）
2. **Keyboard mode**（`SCREEN_BT_KEYBOARD`）：当真 BLE 键盘。组合键（ctrl/opt/alt，按住或点一下锁定）、Fn 层（Esc/F1~F12/方向/Home/End）、长按连发、回显框、`Fn+Enter` 切 IME 模式、`Fn+` ` 退出
3. **Media remote**（`SCREEN_BT_MEDIA`）：`,`/`/` 上下曲 · `;`/`.` 音量 · `Enter` 播放/暂停 · `m` 静音 · `s` 停止

> 拆分观察：这**三个几乎是三个独立 app**，只是共享一次 BLE init。
> 而 Keyboard/Media 是"设备当 HID 用"，跟 MORE 组里的 **Ducky（USB HID）是同一个家族**。

### GNSS — 5 子页（Map 已拆出为独立 app，模块配置已挪入 Settings）
1. **定位**（`SCREEN_GNSS`）：天空图（星座颜色、SNR 点大小）、经纬度、海拔、对地航速等
2. **详情**（`SCREEN_GNSS_DETAIL`）：2D/3D、GGA 定位质量指示（单点/DGPS/RTK Fix/Float/惯导推算等）、PDOP/HDOP/VDOP 紧凑多维精度因子、大地水准面差距（Geoid Separation）、在用/可见星数；**按 `c` 键循环切换 4 种坐标格式**（DEG 经纬十进制度 / DMS 度分秒 `dd°mm'ss.s"` / GRID Maidenhead QTH 瓦片格 / UTM 投影网格与米制坐标）
3. **卫星信号**（`SCREEN_GNSS_SAT`）：默认**抬头天际图**（与 Sats 统一为横轴方位、纵轴仰角坐标系），每颗星映射方位/仰角，颜色表示星座，大小与实心表示 SNR，参与定位加绿辉光环，防文字重叠自适应标注 PRN；按 `m` 键在**天空图 / 信号表格**之间无级切换；表格保留按 SNR 排序、`[`/`]`（或 `j`/`k`）选行、8 格信号条，`Enter` 弹出单星探针（轨道/仰角方位/斜距时延/频点）；天空图下 `Enter` 进表格
4. **速度表**（`SCREEN_GNSS_SPEED`）：半圆运动仪表盘，自适应动态量程（120 / 360 / 1000 km/h，时速超 100 升 360、超 360 升 1000，带 50/100 与 300/360 回差缓冲消除死区与跳变）
5. **行程与诊断**（`SCREEN_GNSS_TRIP`）：累计行程里程（自适应抗漂移滤波）、总时长与纯移动时长、最高航速与平均速度、TTFF 首次定位耗时与 REACQ 重定位耗时、在用/可见星数、运动状态（MOVING/IDLE）、Top-4 卫星平均 SNR 与最近 2 分钟 SNR 历史波形；**按 `r` 键重置行程统计与波形历史**
- **模块配置**（`SCREEN_GNSS_CONFIG`，现已移入 Settings 系统设置作为独立项，不再占用 GNSS 分页链）：`[`/`]` 选项目，`-`/`=` 调值，`Enter` 应用，`s` 保存到 Flash；5 项参数：Rate 更新频率（1Hz/2Hz/5Hz，默认 5Hz）、System 星座组合（GPS/BDS/GLONASS等）、Dynamic 动态模型（Aero <2g / Aero <1g / Vehicle / Portable，开机默认 Aero <2g 高动态）、NMEA 语句集（full / nav+gsv 精简集，默认 nav+gsv）、RF antenna 开关；按 `` ` `` 键返回 Settings。
- **底层与安全机制**：开机自动下发高动态模式（`PCAS11,6` Aero <2g）、5Hz 高更新率（`PCAS02,200`）、精简 NMEA 语句集至 nav+gsv（`PCAS03` 仅保留 GGA/GSA/GSV/RMC）。带开机 3.5s 延迟兜底重发与热插拔 500ms 重发。TinyGPS++ 的 `isValid()` 一旦有一次定位便永久为 true，固件严格施加 3 秒内更新判据（`GNSS_FIX_STALE_MS = 3000`）；速度与航向仅在定位新鲜且 HDOP ≤ 8.0 时才视为可信，失锁或遮挡大跳点时页面显示 `n/a`，杜绝静止漂移显示虚假时速。

### Map — 1 屏（独立 app）
- `[` 缩小 `]` 放大，五档 WORLD / z5 / z8 / z11 / z14
- 高德卫星影像（JPEG 流式解码），48KB 底图缓存（拿不到退 8bpp 24KB，再拿不到退世界图）
- 室内无 GPS 时退回 IP 定位当中心（顶栏标 `ip`），但尾迹和"你在这"只在真有 GPS 时画，位置获取走 DeferredFetch 不阻塞渲染循环

### IR — 1 屏（红外遥控）
- 硬件：GPIO44，**只有发射、没有接收**，学不了别人的遥控器，码来自内置或 SD 码表
- `,`/`/` 换设备，`;`/`.` 选按键，`Enter` 发送，`p` 关机扫频，`r` 重读 SD `/ir/*.ir`
- 内置 LG / Samsung / Sony 三台电视码表；支持 SD 卡载入 NEC32 / NEC / Sony / RC5 / RAW 格式码表
- 发射走 RMT 3 号通道（硬件定时），只在这一页开着时安装驱动；单行缓冲扩至 1024 字节并在 loop 外静态分配；OOM 重试后恢复 RMT 发射器

---

## SKY（抬头看天）

### Astro — 3 子页（纯离线，本地计算）
1. **日照**（`SCREEN_ASTRO_SUN`）：太阳高度角曲线 + 日出/正午/日落/昼长（−0.833° 天文通用折射判据，曲线上求极大值），无 GPS 时退回 IP 定位
2. **月相**（`SCREEN_MOON`）：相位、月龄、照亮比例、下次满月/新月
3. **晨昏线**（`SCREEN_ASTRO_TERM`）：世界地图上的昼夜分界 + 当前太阳赤纬
- 三页全是纯算，一个字节网络都不要，共用 `r` 重新定位

### Weather — 5 子页
1. **实况** 2. **未来 24h 曲线** 3. **风与空气** 4. **空气质量(AQI)** 5. **五天预报**
- 五页共用 `r`/`Enter` 刷新
- 单位公制/英制在 Settings 里切；日照页已剥离独立归入 Astro

### Planes — 1 屏
- ADS-B 飞机雷达，`;`/`.` 换关注目标，`r` 刷新（顺带清航线缓存）
- 明文 HTTP 飞机列表不受系统 NTP 时钟是否同步限制，选中项查航线（adsbdb.com）走 HTTPS
- 统一携带 `User-Agent: cardputer-adv/1.0` 避免被上游按 UA 拦截（403）

### Sats — 1 屏
- 卫星过顶，`c`（或 `;`/`.`）切 Starlink/GPS，`r` 刷新；需 `N2YO_API_KEY`
- 搜索半径分类设定（Starlink 25° / GPS 90°），防超大 JSON 响应挤爆 TLS 堆

### Typhoon — 2 子页
1. **预警实况** 2. **路径图**
- 两页共用 `;`/`.` 换台风、`r` 刷新；走 HTTPS（jma.go.jp）

### Quake — 2 子页
1. **列表检视**（`SCREEN_QUAKE`）：震级、地名、发生时间、深度与相对距离
2. **世界地图打点**（`SCREEN_QUAKE_MAP`）：全球地震散点图，点大小与颜色按震级分档
- USGS 公开摘要源，全球覆盖、免 key；`m` 切换两个源（M2.5+/24h 与 M4.5+/7d），选择存 NVS；逐 feature 流式解析防 OOM

---

## MORE（个人仪表盘、多媒体与系统设置）

### Ducky — 3 子模式
- **DK_PICK**：选 SD 上的脚本（列表）
- **DK_READY**：`Enter` 开跑（需 BLE 已连），`p` 回选择
- **DK_RUN**：执行中不收键，`` ` `` 中止整个 app

### ChatGPT — 1 屏 + 1 子屏
- 输入问题（≤200 字符），`Enter` 发（后台任务，页面画会动的 "asking..."）
- 回复页（`SCREEN_CHAT_REPLY`）：`;`/`.` 翻页；保留最近 3 轮上下文；需 `CHAT_*` 配置
- **内存抗压防护**：HTTPS 请求发起前临时释放 64.8KB 全屏画布（`cv.deleteSprite()`），期间使用 `M5.Display` 硬件直绘文字提示，请求完成或退出时无感恢复画布；强制发 `HTTP/1.0` 避免 chunked 传输带来的 12KB 缓冲区开销，彻底杜绝 TLS 握手 OOM 与退出死锁

### GitHub — 1 屏
- 贡献热力图，`r` 刷新（半小时内不重复拉）；数据拉取至临时数组再提交，保留实际错误信息并在有旧数据时显示于底部

### Radio — 1 屏 + 2 子屏
- 6 个 SomaFM 预设 + 1 个 `[Custom URL]`
- 列表页：`;`/`.` 选台 · `Enter` 播 · `u` 编辑自定义 URL · `[`/`]` 或 `-`/`=` 音量 · `m` 静音 · `r` 重连
- **播放页**（`SCREEN_RADIO_PLAY`）：15 频段动态音频频谱仪、实时缓冲余量波形、码率与 ICY-Title 流媒体元数据
- **URL 编辑页**（`SCREEN_RADIO_URL`）：只收 `http://`（≤120 字符）
- 双核协作式退出流防 lwIP 互斥锁死；音频忙时通知倒计时闹钟避让扬声器

### Router — 6 页（`n` 切换；Top Hosts 页 `m` 切字节/连接数；History 页为内存/连接数曲线，12 秒一点、约 24 分钟）
- Clash/mihomo 状态：连接数、当前节点、版本、延迟；`r` 刷新；需 `CLASH_*` 配置
- 节点与组名 URL 编码转义，PUT 请求体采用 ArduinoJson 序列化

### FX — 2 子页
1. **现价走势**（`SCREEN_FX`）：现价 + 当日涨跌幅 + 30/90 天历史折线图
2. **逐日收盘**（`SCREEN_FX_DAYS`）：逐个交易日收盘价与日变化列表
- 欧洲央行（frankfurter）公开参考汇率源，免 key；单请求拉取整段时序；红涨绿跌；`m` 切 30/90 天窗口，`r` 刷新

### OKX — 1 屏
- USDT → 人民币实时行情，`r` 刷新（每 5 分钟自动更新一次）
- 双源高可用：主源 OKX 实时 C2C 撮合买入价（含实时溢价），被墙/超时自动降级切 CoinGecko 备选源，顶栏清晰标明当前数据源

### Reader — 1 屏 + 阅读器子屏
- 独立电子书架：扫描 MicroSD 卡中的 TXT 电子书，支持字号切换、断点进度记忆与翻页快速跳转

### BadApple — 1 屏
- 字符画动画与音频播放：播放落后超过 1 帧时直接 seek 到目标帧追赶，保证与真实时间轴严格同步

### Settings — 14 项
| 项 | 形态与行为 |
|---|---|
| Wi-Fi | → 扫描子屏 → 密码输入子屏（存 NVS，开机常驻自动连） |
| **GNSS Config** | → 子屏（`SCREEN_GNSS_CONFIG`：Rate/星座/动态模型/NMEA语句集/天线供电等硬件参数配置，` 键返回 Settings） |
| **PC MODE** | → 进入 PC 主导模式（释放 64.8KB 画布直推极简状态屏，串口 PCM 协议握手；` 键或 PCEXIT 退出回 Settings） |
| Brightness | → 子屏（`;`/`.` ±5%，调节屏幕背光） |
| Volume | → 子屏（`;`/`.` ±10%，扬声器主音量） |
| Boot sound | **原地开关**（ON/OFF，开机 5 阶升调琶音开关，关闭完全静音） |
| **LED Mode** | **原地按 Enter 循环切换 6 种模式**：关 (OFF) / 电量 (Battery) / 呼吸 (Breathe) / 彩虹 (Rainbow) / 跑马 (Chase) / 音乐律动 (Music Reactive) |
| UI Theme | **原地按 Enter 循环切换 8 种高科技 HUD 配色主题** |
| Auto sleep | → 子屏（Never / 30s / 1m / 3m / 5m 自动熄屏超时） |
| Timezone | → 子屏（POSIX 时区切换，默认 CST-8） |
| Weather Unit | **原地开关**（Celsius 摄氏度 / Fahrenheit 华氏度） |
| Battery | → 子屏（只读：实时电压、电量百分比、放电曲线与标定） |
| **Debug** | **原地开关**（屏幕底部常驻遥测状态条，存 NVS） |
| Format SD | → 子屏（两段确认，`y` 两次安全格式化） |
| About | → 子屏（固件版本、编译时间及开机 RAM 阶段轨迹表） |

> **LED 接管与优先级仲裁**：
> - Settings 的 LED 模式为系统全局基底灯效。
> - 当频谱页开启麦克风律动（`L` 键）或倒计时闹铃到点触发时，通过 `ledSetOverride(true)` 临时接管 LED，常规灯效暂停；退出或消音时调用 `ledSetOverride(false)` 自动恢复原模式。
> - 优先级保护：若频谱律动已接管 LED，倒计时闹钟不会强抢 LED（避免破坏律动且防止释放时误灭灯）；未被接管时，麦克风/播放器/电台忙时闹钟借用 LED 红光闪烁警示。

---

## 不占菜单的功能：串口通道

`pio device monitor` 里敲，实现在 `serial_cmd.cpp`。**整机可以完全从电脑驱动**
（单字符原样当按键喂给 UI）。

导航：`HELP`/`?` · 单字符/`ENTER`/`BACK`/`UP`/`DOWN` · `MENU` · `GOTO n` · `GNSSMAP`
诊断：`STAT` · `MEMCAP` · `RAMLOG` · `TRAIL` / `TRAIL MARK n` · `GPS` · `SHOT` · `CAT path`
网络：`PROBE` · `WIFI`/`WIFIOFF` · `HGET`/`HPOST`/`TCPHEX` · `RANDMAC`/`SETMAC`
射频取样：`RIDSCAN` · `BTDUMP` · `BTEXT` · `RFSCAN` · `RIDFAKE`
开关：`DEBUG ON`/`DEBUG OFF`

> 这里有些能力**菜单里没有对应入口**（`RFSCAN` 的 2.4G 普查、`BTEXT` 的扩展广播取样、
> `MEMCAP` 的碎片形态、`RAMLOG` 的开机内存轨迹）。重排时可以考虑哪些值得提上屏。

---

## 重排时值得先决定的几件事

1. **TOOLS / NETWORK 已满**。要拆里面的东西，得先决定是加第六个组，还是把某些 app 挪走。
2. **三个"重量级 app"内部各自装了 3~6 件事**：GNSS(6 页)、Bluetooth(3 个独立模式)、
   LoRa(4 模式)。拆开会立刻吃掉大量格子——GNSS 拆成 3 个就要 3 格。
3. **HID 家族是散的**：Bluetooth/Keyboard、Bluetooth/Media、Ducky 分在两个组，
   但它们是同一件事的三个出口。
4. **IR 是空壳**，占着一格。要么做，要么先摘掉。
5. **NetProbe / DnsFuzz 结构重复**，已合并为 NetProbe 的 Probe / DNS 双模式。
6. **串口能力和菜单能力不对称**，有些只在串口里有。
7. **Settings 里"原地开关"和"进子屏"混排**，可能值得分成两段。

---
---

# 重排方案（2026-08-26 定）

## 已定的三条

1. **Astro 保持纯离线** —— 只收 月相 / 晨昏线 / 日照 三页，**不收 Sats**（要联网 + N2YO key，
   会破坏"断网可用"这个身份）。
2. **Clock 合并为 Time** —— 时钟 + 秒表 + 倒计时 一个 app。
3. **重量级 app 拆开** —— GNSS / Bluetooth / LoRa 各自拆出独立入口。

## 分组原则的改变

旧的五组混了两套原则：TOOLS/NETWORK/SIGNAL 是**按硬件**（实现视角），SKY 是**按主题**
（用户视角），MORE 是残留。月相放在 Clock 里别扭，根源就在这——Clock 落在"不用射频"
这个硬件属性上，而月相是个主题物件。

新方案**让主题赢**。硬件冲突（谁抢射频）是真实属性，但那是"进去之后会发生什么"，
属于页面上的提示；不是"我现在想干什么"，不该当导航轴。

## 目标布局：34 个 app / 7 组

| 组 | 数量 | 余量 | app | 归组理由 |
|---|---|---|---|---|
| **TOOLS** | 7 | 1 | **Time**★ · IMU · Files · Spectrum · Calc · Converter · Player | 本地工具 |
| **SKY** | 5 | 3 | **Astro**★ · Weather · Planes · Sats · Typhoon | 抬头看天 |
| **SCAN** | 5 | 3 | WiFi Chan · Sniffer · Wardrive · Drone ID · **BLE Scan**★ | 被动找东西（混杂模式/扫描，不需要连上） |
| **NET** | 4 | 4 | Hotspot · LAN Scan · NetProbe · DnsFuzz | 主动收发（要真连上网） |
| **SIGNAL** | 5 | 3 | LoRa · **LoRa Chat**★ · GNSS · **Map**★ · IR | 其它收发通道 |
| **HID** | 3 | 5 | **BT Keyboard**★ · **BT Media**★ · Ducky | 设备当键盘敲主机 |
| **MORE** | 5 | 3 | ChatGPT · GitHub · Radio · Router · Settings | 个人服务仪表盘 + 设置 |

★ = 新增或从别处拆出来的

容量 56、已用 34、**每组都有余量**（旧方案 TOOLS/NETWORK 是 0 余量）。

## 逐项改动

### 合并

- **Time** ← Clock(第1页) + Timer。Clock 的另外两页(晨昏线/月相)去 Astro。
- **Astro**★新 ← 月相(Clock p3) + 晨昏线(Clock p2) + 日照(Weather p3)。
  三页全是纯算：`moon.cpp` / `ui_common.cpp:subsolarPoint()` / `weather.cpp:solarElev()`。

### 拆分

- **GNSS**(6页) → **GNSS**(定位/详情/卫星/配置，4页) + **Map**★(地图独立成 app)
- **Bluetooth**(3模式) → **BLE Scan**(扫描/详情/找物雷达) + **BT Keyboard** + **BT Media**
- **LoRa**(4模式) → **LoRa**(SCAN/LISTEN/AUTO) + **LoRa Chat**(私有帧收发)

### 顺带修掉的 bug

**日照页被天气的空状态守卫误伤。** `drawWeatherSun()` 开头是 `if (drawEmptyState()) return;`，
天气拉不到就不画那条太阳曲线——但曲线值全是 `solarElev()` 本地算的，一个字节网络都不要。
拆进 Astro 之后自然就好了。

## 代价（要认的）

1. **7 个组意味着 tab 更长。** G0 在主菜单里是"翻到下一组"，从第 1 组走到第 7 组要按 6 次
   （现在最多 4 次）。左右键走出边界也会切组，路径变长。
2. **⚠️ 拆 Bluetooth 会增加 BLE 拆除次数。** 现在三个模式是 BT app 内部的子屏，来回切**不花钱**；
   拆成三个顶层 app 之后，从 Keyboard 换到 Media 要经过主菜单，多走一遍 `btExit()`。
   而 BLE 的拆除路径正是全项目唯一已知会**永久阻塞**的地方（见 [ble-teardown.zh-CN.md](ble-teardown.zh-CN.md)）。
   拆之前得先确认这条路径上不会触发 `btReleaseForOtherApps()` 的 deinit，否则等于把
   最脆的那条路暴露得更频繁。**这条是本次重排最大的技术风险。**
3. **Map 独立后要自己拿位置。** 现在它靠 GNSS 的 fix；独立成 app 后应该走 `geoGet()`
   （GPS 没锁定时退回 IP 定位）——顺带让它在室内也能用，算是改进。
4. `GROUPS[]` 的 `static_assert` 会拦住改到一半的状态，这是好事。

## 还没定的

- **GNSS 配置页**本质是"设置"，留在 GNSS 里还是挪进 Settings？

---

## 实施进度

- **2026-08-26 第 0 步**：`isBleScreen()` 收拢三份拷贝（commit 37a8a38）。顺带查出并修掉
  "用过蓝牙回主菜单会卡在 8KB 空闲 + Wi-Fi 死掉"（commit 4e9b455）。
- **2026-08-26 第 1 步**：Time 合并 + Astro 抽取。
  - Clock + Timer → **Time**（时钟 / 秒表+倒计时 两页）
  - 月相(Clock p3) + 晨昏线(Clock p2) + 日照(Weather p3) → **Astro**（SKY 组）
  - TOOLS 8→7，SKY 4→5，APPS 仍是 30 个
  - 天文计算从三个文件（ui_common.cpp / weather.cpp / moon.cpp）收拢到 astro.cpp
  - **修掉**：日照页原来被 `drawWeatherSun()` 开头的 `drawEmptyState()` 误伤，
    天气拉不到就不画那条纯本地算的曲线
  - **修掉**：日出/日落原来抄接口返回的字符串，现在自己算（−0.833° 判据），
    weather.cpp 顺带不再请求 `sunrise,sunset` 两个字段
  - **修掉**：Timer 底部提示行 42 字符超出 240px 屏宽，两头被切
  - Timer 倒计时预设键 `;.,/ ` → `[ ] - =`（翻页链会先吃掉方向键）

- **2026-08-26 第 2 步**：NETWORK 劈成 SCAN / NET。
  - 分界是**被动 vs 主动**：SCAN 那四个全靠混杂模式/扫描，一个都不需要真连上网；
    NET 那四个要连上（或自己当 AP）才有意义。这条界线用户能直接感觉到（没网时哪些还能用）。
  - 6 组：TOOLS 7 · SCAN 4 · NET 4 · SIGNAL 4 · SKY 5 · MORE 6 = 30
  - ⚠️ 组名从此**最多 6 个字符**：tab 条平分屏宽，6 组时每格 240/6 = 40px，
    Font0 6px/字符 —— "SIGNAL" 已占 36px，只剩 2px 边距
  - **新增编译期护栏** `groupsChainOk()`：原来只有一条"APPS 数量 == 30"的 static_assert，
    配着一句"GROUPS 的覆盖范围没法在编译期算"——其实能算，把 GROUPS 改成 constexpr 就行。
    现在会验三件事：各组首尾相接（不留缝不重叠）、每组不超过一屏、合起来正好盖满 APPS[]。
    三种错法都实测过会在编译期报出来。
    （写成递归而不是 for：工具链是 gnu++11，C++11 的 constexpr 只允许单条 return。）

  ⚠️ **HID 组没有按原计划在这一步建**：它要装的 BT Keyboard / BT Media 得等 Bluetooth
  拆开才存在，现在建只有 Ducky 一个，白占一整屏。挪到第 4 步跟 Bluetooth 拆分一起做。

- **2026-08-26 第 3 步**：GNSS 拆出 Map。**LoRa 不拆**（读完代码后推翻了原计划）。
  - **Map 独立成 app**（SIGNAL 组，摆在 GNSS 旁边）。拆它的理由不是"页数多"，
    而是那 48KB 底图缓存原本翻页翻到就悄悄申请；现在是一次明确的进出。
    实测：进入 heap 72632→24132（−48.5KB），退出完整还回 72072。
  - **顺带让它在室内能用**：没有卫星定位时退回 IP 定位当中心（顶栏标 `ip`）。
    刻意把 `haveGps` 和 `haveCenter` 分成两个概念——尾迹和"你在这"只在真有 GPS 时画，
    IP 定位是城市级的点，拿它画当前位置是撒谎。
    位置获取走 DeferredFetch，绝不在 draw 里同步拉（geoGet 没 fix 时会发 HTTP）。
  - GNSS 从 6 页降到 5 页，且**退出时不再需要任何清理**（那 48KB 只可能由 Map 申请）。
  - 7 组 31 个 app。`groupsChainOk()` 这次直接把改 GROUPS 的活验了，没出错。

### ⚠️ LoRa Chat 不拆——原计划是错的

  读代码才发现 CHAT 跟其余三个模式**共享射频状态**：`curFreq` / `presetIdx` /
  `activeModem` 都是全局，进 chat 走 `loraApplyPreset()` 用的就是你当前正在监听的预设，
  退出直接回 LISTEN 接着蹲。也就是说 chat 的语义是"**用我刚才验过的这个频点说话**"，
  不是一件独立的事。

  拆成顶层 app 的代价：你得先进 LoRa 选好预设 → 退到菜单 → 再进 LoRa Chat，
  而 chat 页里 `,`/`/` 被打字占了、没法在里面选预设。比现在按一下 `c` 差得多。
  原来的"拆分观察"是照着功能清单写的，没看状态耦合。

- **2026-08-26 第 4 步（收尾）**：Bluetooth 拆成三个 app，新建 HID 组。
  - **BLE Scan → SCAN 组**（含设备详情 + 找物雷达）。它跟 WiFi Chan / Sniffer /
    Wardrive / Drone ID 是同一件事——找周围有什么，不需要连上谁。
  - **BT Keys + BT Media + Ducky → 新的 HID 组**。三个出口原本散在两个组里，
    其实是同一件事：把这台设备当键盘/遥控器去敲别人，只是一个走 BLE 一个走 USB。
  - 原来那个三选一的蓝牙子菜单（SCREEN_BT / drawBtMenu / btMenuIndex）整个删掉。
  - main.cpp 里 BtnA 对键盘页的特判也删了：它存在的理由是"只退回蓝牙子菜单，不做
    btExit"，而子菜单没了，通用路径就是对的。
  - **新增第二条编译期护栏 groupNamesFit()**：7 组时每格只有 240/7 = 34px，
    而 "SIGNAL" 要 36px——这一步正是被它逼着把 SIGNAL 缩成 SIG 的。tab 溢出是**静默**的
    （名字太长直接画到相邻组头上），与其写进注释靠人记，不如让它编译失败。实测会响。
  - 实测三个 app 独立进出正常；经过主菜单转场零开销（btReleaseForOtherApps 只在
    "不在菜单且不在任何 BLE 页"时才真释放）。

  最终：**7 组 33 个 app**
  TOOLS 7 · SCAN 5 · NET 4 · HID 3 · SIG 4 · SKY 5 · MORE 5

- **2026-09 扩充至 40 个 app**：
  - TOOLS 补入 **Hash Oven**（密码工坊：跑分/对称/古典/哈希/穷举 5 模式，满员 8/8）
  - NET 补入 **SSH** 交互式终端客户端（6/8）
  - SIG 补齐 **IR** 红外发射器（RMT 硬件驱动，内置+SD码表，4/8）
  - SKY 补入 **Quake** 全球地震速报（6/8）
  - MORE 补入 **FX**（汇率走势）、**OKX**（USDT行情）、**Reader**（电子书架）、**BadApple**（字符画播放）（满员 8/8）
  最终当前格局：**7 组 39 个 app / 84 个 Screen**
  TOOLS 8 · SCAN 5 · NET 5 · HID 3 · SIG 4 · SKY 6 · MORE 8

- **2026-09-24 ~ 2026-09-25 功能演进与多模块稳定性重构**：
  - **时钟多表盘切换**：`Time` app 第 1 页按 `f` 循环切换 4 套表盘（原版进度条、指针模拟、大字数码、QlockTwo 文字），NVS 记忆持久化；指针表盘平滑扫秒（~12fps），数码管 500ms 翻转冒号
  - **LED 灯效系统与多模块接管优先级**：Settings 新增 6 种 LED 模式（关/电量/呼吸/彩虹/跑马/音乐律动）；实现 `ledSetOverride` / `ledOverrideActive` 全局仲裁机制：频谱页麦克风律动接管 LED 优先；倒计时闹铃检测音频外设（麦克风/播放器/电台）占用自动降级为 LED 报警；若频谱页已接管 LED，闹铃主动避让不抢占
  - **倒计时闹铃全局安全消音**：任何页面按任意键或物理 G0 键均可立即全局消音停闹，不触发当前页面原有操作
  - **GNSS 全面优化**：开机自动下发高动态模式（`PCAS11,6` Aero <2g）、5Hz 高更新率（`PCAS02,200`）、精简 NMEA 语句集至 nav+gsv（`PCAS03` 仅保留 GGA/GSA/GSV/RMC）；开机 3.5s 延迟兜底重发与热插拔 500ms 重发；详情页支持 2D/3D、PDOP/VDOP、GGA 定位质量与大地水准面差距；按 `c` 切换 4 种坐标格式（DEG/DMS/GRID/UTM）；卫星表区分在用/可见；速度表自适应动态量程（120/360/1000 km/h）并消除死区；新增第 6 子页行程与诊断（`r` 重置统计与 2m SNR 波形）；严格执行定位 3s 新鲜度与 HDOP ≤ 8 速度可信度判定（否则标 n/a）
  - **Spectrum 瀑布图**：按 `W` 切换瀑布图与柱状图；瀑布图采用局部滚屏技术，60 dB 自适应动态范围映射 AGC 前原始分贝，过滤 DC 直流分量，仅在新 FFT 帧到达时平滑推进行，黑蓝青绿黄橙红白渐变
  - **Hash Oven 穷举突破**：荒谬穷举挑战模式（MODE_CRACK）支持动态字符集（数字/小写字母/大小写字母数字）生成与全档位通用解真实匹配（0123 / card / cardpt / infinity / entropy9），各档位均能真正命中并触发 `TARGET PWNED!` 炫酷徽章；后台协程世代计数器废弃在途批次防 UAF
  - **Wi-Fi 与热点多模式共存与协调**：热点开启时，Wi-Fi 扫描、WiFi Chan 和 Wardrive 采用 `WIFI_AP_STA` 混杂共存模式，不关热点、连入设备不掉线；LAN Scan 仅扫描 STA 网段不影响热点；Drone ID 与 Sniffer 混杂模式独占射频期间通过 `hotspotSuspend()` 自动暂挂热点，退出通过 `hotspotResume()` 自动恢复
  - **SSH 全面增强**：支持密码认证与 SD 卡私钥（RSA/ED25519）自动探测认证（13 个常见路径，支持 Passphrase）；在活动终端中按 `Fn + Enter` 循环切换 3 种字号（8x16 / 8x8 / 6x8）并重协商 PTY 窗口大小；电脑 USB 串口连接时除系统控制指令外的 Shell 命令直接原样透传至远程 SSH 终端；CAS 所有权机制杜绝任务与析构双重释放
  - **代码审查缺陷修复 (1-12)**：播放器低码率等待超时与协作退出防 FATFS 锁死；IR 单行缓冲扩至 1024B 移出 loop 栈并恢复 RMT 发射器；Drone ID 循环边界防越界；JSON 流式数组校验防残缺；GitHub 错误信息保留；Geoloc NTP 校验与 10 分钟 GPS 优先；Router URL 编码；ADS-B 明文列表免时钟依赖；NetProbe 忽略大小写；Radio 协作停止防 lwIP 锁死；BadApple 掉帧 seek 追赶；LED 息屏唤醒在接管状态下维持背光 255 供电
- **2026-09-28 GNSS 模块配置移入 Settings 系统设置决议**：
  - **定位决议**：GNSS 模块配置页（`SCREEN_GNSS_CONFIG`）定为**系统级硬件设置**，不再作为普通定位/观星/测速的轮播子页（不干扰日常定位监控与运动看板的连续翻页流程），从 `GNSS_PAGES[]` 分页链摘出，入口收拢至 `Settings`（紧随 Wi-Fi 之后）。
  - **交互独立与按键无冲突**：原在分页链内时，通用翻页快捷键（`.`/`/`/`;`/`,`）与页内参数选择/修改按键存在冲突隐患，退出亦依赖外层全局折回；移入 Settings 后作为独立子屏，`[`/`]` 选项目、`-`/`=` 修改、`Enter` 应用、`s` 存盘，按 `` ` `` 键直接精准返回 Settings，界面层级清晰且无状态直接复用。

- **2026-09-27 PC MODE 系统级决议与 Settings 入口收拢**：
  - **定位决议**：PC MODE（`SCREEN_PCMODE`）定为**系统级功能**，不进 `APPS[]`/`GROUPS[]` 菜单网格（不占用 7 组 40 个 app 的名额与网格位置），入口放进 `Settings`（第 1 页第 2 项，Wi-Fi 旁）。
  - **连线协议纪律**：USB 插线默认不自动进入 PC MODE，保持用户主动手动进入或由 PC 桥接工具经串口发 `PCMODE` 指令进入。
  - **统一进入路径与退出去向**：抽离 `pcmodeEnter()` 作为串口 `PCMODE` 与 Settings 的唯一共用入口（统一释放 64.8KB 画布 `cv`、下发 `PCM {"t":"enter"}` 与 `hello` 握手报文）；动态记录进入来源（`pcmodeReturnScreen`），从 Settings 进入退出（`` ` `` 键、`PCEXIT` 等）精准返回 Settings 原选中项，从串口或主菜单进入退出则保持返回主菜单；画布生命周期安全恢复。

- **2026-09-27 GNSS 卫星页与 Sats 统一天际图画法**：
  - **决议落实**：GNSS 卫星页（`SCREEN_GNSS_SAT`）改用与 Sats、Planes 统一的「抬头天际图」坐标系（横轴方位 N-E-S-W-N，纵轴仰角 0~90°）。
  - **坐标投影抽取**：将 `(az, el) → (x, y)` 纯计算从 `sats.cpp` 抽入 `skyview.cpp` 的 `skyCoord()`，Sats 和 GNSS 共享，Sats 画面保持**像素级不变**。`skyview.cpp` 严守无板级依赖原则，tools/uisim 桌面模拟器可直接编译。
  - **天空图多维信息呈现**：
    - 星座：按现有 UI 配色常量上色（GPS 白 / BDS 橙 / GLO 青 / GAL 紫 / QZS 黄），同时在 PRN 标签前缀星座字母（如 `C08`, `G12`, `R03`）。
    - 信号：点半径随 SNR 动态变化（r=2~4），无 SNR 信号星绘制空心圆。
    - 定位参与：在用卫星（来自 GSA）高亮追加外圈 ACCENT 绿色辉光环。
    - 防重叠标签：按定位状态与 SNR 降序排序，结合 4 候选方位（右/左/上/下）与障碍物碰撞检测，密集时优先标注强信号星，避免文字重叠。
    - 底部 HUD 卡片：显示各星座可见颗数统计、最高 SNR 卫星读数与图例说明。
  - **表格视图与按键无损保留**：按 `m`（及 `t`/`v`/`Enter`）在"天空图 / 信号表格"之间无级切换。表格保留原样 6 行斑马线、SNR 排序、9 格信号强度条与 `[`/`]` 滚动。底部提示行适配 240px 屏宽与居中页码点避让。
  - **冷启动空状态**：未收到 GSV 或天线供电关闭时显示合理的居中引导卡片，不画空坐标系。

- **2026-09-27 NetProbe 与 DnsFuzz 合并**：
  - 将结构高度重复的 NetProbe 与 DnsFuzz 合并为一个统一的 NetProbe app，支持 Probe 模式（多目标连通性/投毒/门户检测）与 DNS 模式（10 大 RFC 畸形用例测试与存活量化）
  - 按 `m`/`M`/`Tab` 键在 Probe 与 DNS 两种模式间平滑切换；提取并共用目标主机/端口两段式编辑器、SD 卡日志写入框架；auto 巡检（5 分钟一轮）只在 Probe 模式有效——DNS 模式故意不给 auto，免得无人值守地反复 fuzz 家里的 DNS
  - 严格保持各自的 SD 日志路径（`/netprobe.log` 与 `/dnsfuzz.log`）和格式不变，保持 NVS 命名空间（`netprobe` 与 `dnsfuzz`）完全兼容
  - 串口命令 `DNSFUZZ` 保留为别名直接进入 DNS 模式并启动测试，`NETPROBE` 直进 Probe 模式
  - 顶层菜单 APPS 移除 DnsFuzz，NET 组由 6/8 变为 5/8（余量增至 3 个）；GROUPS 通过 `groupsChainOk()` 编译期校验；全局 app 总数由 40 变为 39，Screen 总数由 85 变为 84

### 后续观察

- 串口独有、菜单里没入口的能力：RFSCAN / BTEXT / MEMCAP 的碎片形态

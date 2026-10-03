# tools —— 配套的电脑端小工具

> 串口不用手写：几个脚本都走 `serialport.py` 的 `find_port()`，按 Espressif 的
> VID `0x303A` 自动认板子——macOS 上是 `/dev/cu.usbmodem*`，Linux 上是
> `/dev/ttyACM*`，都不用管。认不出来时再用 `-p` 指定。

| 工具 | 干什么 |
|---|---|
| [`shot.py`](#shotpy--串口截屏) | 串口截屏存 PNG —— 板子插着时唯一能"真看到屏幕"的办法 |
| [`uisim/`](uisim/README.md) | 桌面 UI 模拟器：直接编译 `src/` 里那份真绘制代码，离屏渲染成 PNG |
| [`lora_view.py`](#lora_viewpy--lora-监听查看器) | LoRa 包实时表格化查看器 |
| `spec_view.py` | 声学采集：拉回 Spectrum 页的频谱流，存 ndjson + 渲染语谱图，供电脑侧分析 |
| `gen_cert_bundle.py` | 生成 `data/cert/x509_crt_bundle.bin`：系统根证书 + 少量「构建时验过」的交叉签名根，详见下面「根证书 bundle」一节 |
| `spec_analyze.py` | 声学分析：吃 `spec_view.py` 存的 ndjson，算噪声基线/窄带音/环境分类/时段切分，出文字报告，`--json`/`--png` 可选 |
| `rid_view.py` | 无人机 Remote ID 实时查看器（流式）。`--snapshot` 持续写最新状态，供随时读取 |
| [`memsweep.py`](#memsweeppy--逐个-app-进出量净堆变化) | 逐个 app 进出、量净堆变化，找内存泄漏。`--selftest` 不用插板子就能验工具本身 |
| `irtest/` | 红外编码器测试台：编译 `src/ir_proto.cpp` 那份真代码，把 mark/space 序列**反解**回比特来验时序和极性。`cd tools/irtest && ./build.sh` |
| `odidtest/` | Remote ID 解码器测试台：编译 `src/odid.cpp` 那份真代码，喂人工构造的 ODID 报文验字段偏移。`cd tools/odidtest && ./build.sh` |
| `powertest/` | 电量/充电逻辑测试台：编译 `src/power.cpp`，覆盖充放电曲线与低电告警。`cd tools/powertest && ./build.sh` |
| `ramtest/` | RAMLOG 内存日志表格试跑台：验证 ring buffer 统计与格式化输出。`cd tools/ramtest && ./build.sh` |
| [`gnsstest/`](#gnsstest--gnss-坐标转换行程统计与-gsa-解析测试台) | GNSS 算法测试台：编译 `src/gnss.cpp` 真代码，覆盖 DMS/Maidenhead/UTM 坐标换算、行程过滤计算与 GSA 卫星解析 |
| [`clocktest/`](#clocktest--文字表盘qlocktwo取词逻辑测试台) | 文字表盘取词测试台：编译 `src/clock.cpp` 真代码，遍历 24 小时 1440 分钟无死角核对英文单词高亮组合 |
| [`astrotest/`](#astrotest--天文计算测试台) | 天文计算测试台：编译 `src/astro.cpp` 和 `src/moon.cpp` 真代码，期望值出自测试台里独立实现的 Meeus 星历算法（先用书中例题校过），覆盖太阳仰角 / 日出日落 / 极昼极夜 / 日下点 / 月相 |
| [`oventest/`](#oventest--hash-oven-穷举猜测生成测试台) | Hash Oven 穷举测试台：编译 `src/hash_oven.cpp` 真代码，核对各 keyspace 空间、全域单射无碰撞与各档 solution 生成 |
| [`jsontest/`](#jsontest--http-流式-json-解析器测试台) | 流式 JSON 解析测试台：用 Stream 替身驱动 `src/http_json.h`，覆盖数组流式反序列化、截断/超时/异常分隔符及 HTTP 错误 |
| [`calctest/`](calctest/README.md) | 计算器 + 单位换算测试台：编译 `src/calc.cpp`（递归下降求值）和 `src/conv.cpp`（7 大类换算），107 项断言覆盖运算符优先级/结合性/阶乘/错误处理/格式化及各类单位往返精度 |

---

## shot.py —— 串口截屏

240×135 太小，纯靠算坐标很容易把两个东西摞在一起；拿手机拍屏幕又拍不清。
这个工具给设备发 `SHOT` 指令，把它吐回来的 RGB565 逐行 hex 拼成 PNG。
GNSS 那几页、天气那四页的布局都是靠它一轮轮对着调出来的。

```bash
python3 tools/shot.py                              # 截当前画面 -> shot.png
python3 tools/shot.py --cmd GNSSMAP --scale 3      # 先跳到地图页再截，放大 3 倍
python3 tools/shot.py --cmd GNSSMAP --wait 15      # 等 15 秒（等瓦片加载完）再截
python3 tools/shot.py -p /dev/ttyACM0 -o /tmp/x.png       # 自动认不出来时才要 -p
```

整屏 240×135 的 RGB565 hex 化之后约 130KB（实测 131,581 字节），**约 1.3 秒**就传完——
板子走原生 USB CDC，`monitor_speed = 115200` 只是给串口 API 的形式参数，实际跑在 USB
全速链路上，不受这个数字约束。默认放大 3 倍写出，
不然原图小到看不清字。

依赖 `pyserial`（`pip3 install pyserial`）。

### 跟 uisim 的分工

- **uisim** 不用插板子，但只能跑不依赖硬件的页面（网络/传感器那层是假的）
- **shot.py** 要插板子，但截的是**真实运行状态**（真定位、真瓦片、真堆内存下的表现）

调布局先用 uisim 快速迭代，最后一定要用 shot.py 在实机上确认一遍。

---

## lora_view.py —— LoRa 监听查看器

把 Cardputer 从 USB 串口吐出来的 LoRa 包，在电脑上实时表格化显示。
适合在电脑上盯着看收到哪些 Meshtastic / LoRa 包，比在 1.14" 小屏上翻列表舒服。

### 原理

进入 Cardputer 的 **LoRa app** 并处于监听（LISTEN / AUTO）时，每收到一个包，
固件会在 USB 串口打印一行（实现见 `src/lora.cpp` 的 `loraPushPkt`）：

```
LORAPKT {"ms":12345,"freq":869.525,"bw":250,"sf":11,"cr":5,"rssi":-92,"snr":-4,"crc":0,"len":40,"to":"ffffffff","from":"a1b2c3d4","id":"0f1e2d3c","hop":3,"hopStart":3,"ch":8,"ack":0,"mqtt":0,"hex":"...完整payload..."}
```

- 前缀 `LORAPKT ` 便于脚本过滤，不跟开机日志（`[boot] ...`）等混淆。
- 只有在 LoRa 监听时才会打印，别的界面不受影响。
- 屏幕上每个包只留 5 字节 hex，**串口这行给的是完整 payload**。

#### 字段说明

| 字段 | 含义 |
|---|---|
| `ms` | 固件开机以来的毫秒数（收包时刻） |
| `freq` | 接收频率（MHz） |
| `bw` / `sf` / `cr` | 当前调制参数：带宽(kHz) / 扩频因子 / 编码率(4/x 里的 x) |
| `rssi` / `snr` | 信号强度(dBm) / 信噪比(dB) |
| `crc` | 1=CRC 校验失败（包可能损坏），0=正常 |
| `len` | payload 字节数 |
| `hex` | 完整 payload 的十六进制 |
| `to` / `from` / `id` | Meshtastic 帧头：目标 / 源节点 ID（小端 4 字节）/ 包 ID |
| `hop` / `hopStart` | 剩余跳数 / 起始跳数 |
| `ch` | Meshtastic channel hash |
| `ack` / `mqtt` | want_ack 标志 / 经由 MQTT 标志 |

> `to`/`from`/`id`/`hop`… 这些帧头字段只有在**解出 Meshtastic 帧头**（CRC 正常且 ≥16 字节）时才有。
> Meshtastic 的帧头是未加密的，即使 payload 是密文也能读出这些元数据。

### 用法

先让 Cardputer 进入 LoRa app → 切到 **LISTEN**，调到有信号的频率
（EU868 = 869.525MHz，或用 SCAN 找峰值再锁定）。

然后在电脑上（**新开终端，别同时开 `pio monitor`，会抢占串口**）：

```bash
python3 tools/lora_view.py                      # 自动找串口
python3 tools/lora_view.py -p /dev/ttyACM0
python3 tools/lora_view.py -v                   # 顺便显示其它串口日志（灰色）
```

参数：
- `-p/--port`  串口设备（不给就自动探测）
- `-b/--baud`  波特率（默认 115200）
- `-v/--verbose`  也打印非 LORAPKT 的串口行（调试用）

### 依赖

需要 `pyserial`：

```bash
pip3 install pyserial
```

Linux 上装系统包也行：`sudo apt install python3-serial`。
或者直接用 PlatformIO 自带的 python（已带 pyserial）：

```bash
~/.platformio/penv/bin/python tools/lora_view.py                              # pipx / 官方脚本装的
/opt/homebrew/Cellar/platformio/*/libexec/bin/python3 tools/lora_view.py      # Homebrew 装的
```

### 输出示例

```
连上 /dev/ttyACM0 @ 115200。进入 Cardputer 的 LoRa 监听即可看到包。Ctrl-C 退出。
#1      12.3s  869.525MHz SF11/BW250  -92dBm snr -4 ok   40B  a1b2c3d4→ffffffff  ch8 hop 3/3       <hex>
#2      15.8s  869.525MHz SF11/BW250 -104dBm snr -8 ok   28B  55667788→a1b2c3d4  ch8 hop 2/3 ACK   <hex>
```

RSSI 会按强度着色（绿 ≥-80 / 黄 ≥-100 / 红 更弱），CRC 失败标红。

### 找不到端口 / 连不上

```bash
python3 -m serial.tools.list_ports -v      # 看 Cardputer 枚举成哪个设备（认 VID 303A）
```

- 如果列表里没有 `303A:1001`：检查 USB-C 线（要能传数据的），或按一下复位重新枚举。
- 如果一直连不上：确认没有别的程序（`pio monitor`、Arduino 串口监视器等）占着这个端口。
- Linux 上如果报 permission denied：把自己加进 `dialout` 组
  （`sudo usermod -aG dialout $USER`，重新登录生效）。


## 根证书 bundle（`data/cert/x509_crt_bundle.bin`）

HTTPS 那几页（Sats / GitHub / Planes 航线 / Typhoon / Quake / FX / OKX / Chat）用它做**真校验**，
替代以前的 `setInsecure()`。文件由 `platformio.ini` 的 `board_build.embed_files` 编进 flash，
链接器生成的符号是 `_binary_data_cert_x509_crt_bundle_bin_start`（`src/tls_ca.cpp` 引用它）
——**改路径就要同步改那个符号名**。

约 56KB flash、122 张证书。RAM 开销很小：只有索引常驻，证书本体留在 flash 按需读。

### 重新生成

```bash
python3 tools/gen_cert_bundle.py            # 就地更新
python3 tools/gen_cert_bundle.py --dry-run  # 只报告会加什么，不写文件
```

⚠️ **不要**再手工跑 Espressif 的 `gen_crt_bundle.py` 直接打包系统根证书了（原来的做法）。
那样生成的 bundle 会漏掉一类链，2026-09-03 一次修了三页才搞清楚，原因写在
`tools/gen_cert_bundle.py` 的文件头里，摘要：

- 证书链的**链尾**不一定是自签根。CA 换新根时会先发一张「交叉签名」版（subject 是新根，
  issuer 是老根），而 `esp_crt_bundle` 只拿链尾的 **issuer** 去查库，于是两种翻车方式：
  老根已被 Mozilla 移除（查不到，FX/OKX 撞的）、或者那次验签是 RSA-4096（堆不够，
  GitHub/Sats 撞的）。两种都报成 "Failed to verify certificate"，看着像证书不受信。
- 脚本的对策是把那次验签**挪到构建时**：在电脑上用现有受信任根验一遍链尾，通过了才把它
  并进 bundle，设备上就直接命中快路径。⚠️ 脚本里 `openssl verify -partial_chain` 那步是
  **安全边界**，验不过必须让生成失败，别绕过。
- 主机名单是从 `src/` 里扫出来的，不用手工维护；判据也很保守——老路径够用的（比如 usgs
  那种 issuer 在库中、验签只要 RSA-2048 的）一律不加，免得把轮换很勤的中间证书钉死。

配套还有一份 **打了补丁的 `lib/WiFiClientSecure/`**（framework-arduinoespressif32 里那份的
副本，靠 PlatformIO 的 `lib/` 优先级盖过上游）。补丁只改 `esp_crt_bundle.c`：链尾那张自己
要是就在 bundle 里（subject 命中 **且公钥逐字节相同**），直接放行、连签名都不用验。
⚠️ **升级 arduino core 之后要重新对一遍这个文件**，改动位置都用「本地补丁」注释标了出来。

生成完可以自校验格式（前 2 字节是证书数，之后每条是 `名字长度(2) 公钥长度(2) 名字 公钥`，
按名字排序好供二分查找；脚本末尾已经自动查过一次）：

```bash
python3 -c "
import struct; d=open('data/cert/x509_crt_bundle.bin','rb').read()
n=struct.unpack('>H',d[:2])[0]; off=2
for _ in range(n):
    nl,kl=struct.unpack('>HH',d[off:off+4]); off+=4+nl+kl
print(n,'张证书，', '✅ 结构完整' if off==len(d) else '❌ 对不上')"
```

---

## memsweep.py —— 逐个 app 进出，量净堆变化

读代码只能证明"有人写了释放"，证明不了"释放干净了"。这个脚本用 `docs/serial-sweep.md`
里那套原语（`MENU` 会走 cleanupApp、`STAT` 报 heap、`/` 当右键、`ENTER` 进 app），
把每个 app 进出 N 轮，报**每轮净堆变化与最大连续块变化的中位数**。

```bash
python3 tools/memsweep.py                      # 全部 app，3 轮（两阶段自动确认长平台期）
python3 tools/memsweep.py --rounds 5 --only Quake IR
python3 tools/memsweep.py --confirm-wait 180   # 针对慢速网络协议延长确认观察期至 3 分钟
python3 tools/memsweep.py --no-confirm         # 跳过第二阶段确认，仅输出第一阶段快速扫描结果
python3 tools/memsweep.py --selftest           # 不插板子，拿假设备验工具本身
```

### 两阶段探测机制与长平台期确认

1. **第一阶段：自适应等待快速扫描（全量 app）**
   退出 app 时，部分释放是异步延迟的（如 lwIP ARP 队列超时清理需数秒、TLS 握手收尾、ping 等任务延时 1 秒自删除）：
   - 脚本在发 `MENU` 后先等最短时间 `--settle`（默认 0.8s），然后每隔约 1 秒读一次 `STAT`，直到连续两次读数的 `heap` 与 `largest` 都不变（已稳定），或达到 `--settle-max` 上限（默认 15s）。
   - 该阶段快速测完所有 app，避免所有 app 都等两分钟导致全量扫描耗时数小时。

2. **第二阶段：长观察期确认（仅针对疑似异常 app）**
   lwIP 网络协议栈的 TCP 连接断开后会进入 `TIME_WAIT` 状态（2×MSL，约 120 秒），到期后才真正释放 PCB 缓冲区（如 DnsFuzz 退出后每轮暂扣 448 字节）。在释放之前堆是完全平整的，第一阶段短自适应探测会被这个长达 2 分钟的平台期误判为稳定泄漏。
   - 第二阶段**仅对第一阶段被判为"疑似泄漏"或"碎片化"的 app** 单独进出一次，并进入长时观察（`--confirm-wait` 默认 150 秒，每 `--confirm-step` 5 秒读一次）。
   - 观察期间只要堆和最大连续块回到进入前水平（允许 ±64 字节微小误差），即改判为`延迟释放（约 N 秒后归还，常见原因 TCP TIME_WAIT）`，不再算作问题；若观察期结束仍未归还，则维持原判定并在报告写明"已观察 150 秒仍未归还"。
   - 可用 `--no-confirm` 参数跳过第二阶段。

### 判定规则（区分 6 类状态）

判定只看**第 1 轮之后**（第 1 轮允许一次性初始化开销）：
- **ok**：首轮与后续轮次均无明显下降。
- **一次性开销**：第 1 轮有较大初始化开销（如建立 TLS 缓冲区或缓存），但之后各轮中位数归零，不算泄漏。
- **暂时下降（几秒内恢复）**：刚退出时下降，但随后在十几秒内稳定恢复（如 LAN Scan / SlowFree），不算问题。
- **延迟释放（几十秒到几分钟）**：经第二阶段确认，在长观察期内堆和最大连续块完全归还（如 DnsFuzz 的 TCP TIME_WAIT 到期），不算问题。
- **疑似泄漏（确认后仍未归还）**：经第二阶段长观察确认后总堆仍持续未还（中位数 ≤ -256 字节）。
- **碎片化（确认后仍未恢复）**：经第二阶段长观察确认后总堆已归还，但最大连续块持续缩减（中位数 ≤ -512 字节）。
- **未稳定**：在 `--settle-max` 上限内读数仍持续变动，提示加大等待上限。

⚠️ 默认跳过 BLE 三件套 / Ducky / Hotspot / Settings，理由写在脚本里的 `SKIP_DEFAULT`。
一次完整审计的结论见 [docs/memory-audit.md](../docs/memory-audit.md)。

---

## gnsstest/ —— GNSS 坐标转换、行程统计与 GSA 解析测试台

GNSS 相关的纯算法（坐标表示、行程过滤、NMEA 解析）逻辑分支繁多，但在真机调试极慢且受室外搜星条件制约。`gnsstest/` 直接引入 `src/gnss.cpp` 实现在主机端编译运行，开 ASan/UBSan，包含 58 项严格断言。

### 为什么要有它

1. **坐标换算严密性**：
   - **DMS（度分秒）**：验证正负经纬度方向字符（N/S/E/W）、0 度与 180 度极值边界，以及最容易出错的进位问题——如 `59.996"` 秒进位为 1 分，绝不能输出 `"60.0\""`。
   - **Maidenhead Grid（梅登黑德网格 6 位）**：使用全球公认的已知基准点核对（上海人民广场 `31.2304N, 121.4737E -> PM01rf`、格林尼治皇家天文台 `51.4769N, 0.0005W -> IO91wm`、悉尼歌剧院 `33.8568S, 151.2153E -> QF56od`、纽约时代广场 `40.7580N, 73.9855W -> FN20xr`）。
   - **UTM（通用横轴墨卡托投影）**：使用真实坐标点核对东坐标与北坐标（误差严格在 1 米以内）；验证南半球 10,000,000m 假北距偏移、经度跨带（如东经 120° 临界点）、极区范围外保护（纬度 >84°N 或 <80°S 返回 `false`）。
2. **行程统计过滤（`gnssTripProcessPoint`）**：
   - 模拟原地静止漂移点（速度低且距离近不累加里程）、匀速直线运动累加里程与移动时间、大距离跳点过滤（瞬时异常跳变不污染统计）、丢星中断后重新定位等时间步进场景。
3. **GSA NMEA 语句解析（`gnssProcessGSA`）**：
   - 验证多星座报文（`$GNGSA`、`$GPGSA`、`$BDGSA`）、16 字段传统格式与 18 字段带 NMEA 4.10 `systemId` 格式。
   - 检查已用卫星列表提取（去重并排除空槽位）、PDOP/HDOP/VDOP 精度因子提取。

```bash
cd tools/gnsstest && ./build.sh
# 开启 Sanitizer:
SAN=1 ./build.sh
```

---

## astrotest/ —— 天文计算测试台

编译 `src/astro.cpp`（太阳仰角 `solarElev`、日出日落 `sunCross`、日下点 `subsolarPointAt`）和 `src/moon.cpp`（月相 `moonPhaseAt`、`phaseName`）的真代码，靠 `-DHOST_TEST` 打开两个文件末尾的出口。

### 期望值从哪来

**不拿被测代码的输出当答案。** 测试台里另写了一套 Jean Meeus《Astronomical Algorithms》的算法当参考：恒星时（式 12.4）、太阳视位置（第 25 章）、真朔真望（第 49 章，含全部周期项和行星附加项）。这套参考先用书中例题 12.a / 12.b / 25.a / 49.a 校一遍，对上了才拿来当尺子。被测代码用的是另一套近似式，两边算法不同还能对上，才说明被测代码是对的。

- 太阳仰角：2020–2035 两万个随机样本，最大误差约 0.007°。
- 日出日落：7 个地点（含极圈内、日期变更线两侧）× 分点至点，跟参考差 2 s 以内；极昼 / 极夜返回 -1。
- 日下点：误差随闰年周期起伏，9 月分点附近最大约 0.55°，地图上不到半个像素。
- **月相：当前有 2 项 FAIL，是真问题**。线性外推在 2026 年最大偏差约 17 h（注释写的是"几小时级别"），北京时间下 NEXT FULL/NEW 有 9 次日期错一天。详见 `main.cpp` 第 5 节的说明。

```bash
cd tools/astrotest && ./build.sh
SAN=1 ./build.sh    # 开 ASan / UBSan
```

---

## clocktest/ —— 文字表盘（QlockTwo）取词逻辑测试台

Cardputer ADV 的时钟 app 支持 QlockTwo 风格矩阵文字表盘（10 行 11 列字符矩阵）。通过高亮特定英文单词组合表达当前时间，规则极为考究。

### 为什么要有它

- **边界分钟切换**：如 `11:58`（提前预读下一个钟头，高亮 `FIVE TO TWELVE`）、`23:58`（跨午夜回到 `MIDNIGHT`）、`12:30`（`HALF PAST TWELVE`）、`0:03`（`MIDNIGHT` + 3 个角点）。
- **单词间互斥与组合完整性**：
  - 序言始终包含 `IT IS`。
  - `PAST` 与 `TO` 互斥，整点时必须且仅包含 `O'CLOCK`（午夜/正午例外）。
  - 分钟词（`FIVE`、`TEN`、`QUARTER`、`TWENTY`、`TWENTYFIVE`、`HALF`）精准唯一命中。
  - 12 小时制与 24 小时制（AM/PM 显示）切换。
- 测试台直接引入 `src/clock.cpp` 的计算核心 `qlockCalcWords`，**穷举 24 小时全部 1440 分钟**，并对每一分钟的全部高亮单词进行不变量全量断言。

```bash
cd tools/clocktest && ./build.sh
# 开启 Sanitizer:
SAN=1 ./build.sh
```

---

## oventest/ —— Hash Oven 穷举猜测生成测试台

Hash Oven 内置的荒谬穷举（Futility Cracker）模式：根据当前选中的 keyspace 档位和尝试索引（0..total-1），生成对应的候选破解口令。

### 为什么要有它

- 索引与生成的口令必须是**双向一一映射（单射且满射）**：不能产生碰撞，不能漏掉空间中的任何一个排列组合。
- 必须保证各档预设的 solution 口令确实存在于该空间中，且能被算法精确生成。
- 测试台直接编译 `src/hash_oven.cpp`，覆盖：
  - 4-Digits 档：0..9999 全量 10,000 个索引遍历，验证严格无碰撞无遗漏。
  - 4-Lower 档：0..456,975 全量 456,976 个索引遍历验证。
  - 大 keyspace（8 字符大字典）：10 万点跨空间步进采样，双向编码反解恒等。
  - 各档 solution 索引生成命中核对，以及小缓冲区越界防护。

```bash
cd tools/oventest && ./build.sh
# 开启 Sanitizer:
SAN=1 ./build.sh
```

---

## jsontest/ —— HTTP 流式 JSON 解析器测试台

Cardputer ADV（ESP32-S3 无 PSRAM，仅约 320KB 内部 RAM，全屏画布常驻 64.8KB）在抓取 GitHub 提交记录（365 天数组）或 USGS 地震列表（上百条 GeoJSON feature）等超大 API 时，若整包解析会分配数十 KB 连续内存导致 OOM。为此专门设计了 `fetchJsonStreamArray` 流式逐项解析器（零大块堆分配）。

### 为什么要有它

网络数据流存在各种不可控异常，真机调试难以复现弱网、断流或畸形包。测试台利用轻量 Stream / HTTPClient 替身驱动 `src/http_json.h`，覆盖 113 项全量测试：

1. **流式反序列化正常场景**：多元素对象、美化带空格/缩进/换行 JSON、嵌套数组/对象、字符串列表与布尔空值。
2. **空数组处理**：纯 `[]`、空白 `[  ]`、以及带 `arrayKey` 前缀的空数组 `{"items":[]}`。
3. **前缀定位**：`arrayKey`（如 `"\"features\":["`）快速穿透外壳，键不存在时优雅报错。
4. **异常与容错**：
   - 元素内部意外截断、逗号后截断、缺少闭合括号截断（精准区分 `json error` 与 `stream timeout`）。
   - 异常分隔符拦截（如分号或缺少逗号报错 `"unexpected separator"`）。
   - 流读取超时拦截（`"stream timeout"`）。
5. **控制参数**：`maxItems` 提前截断、回调 `onItem` 返回 `false` 提前停止、ArduinoJson `Filter` 冗余字段过滤。
6. **HTTP 与配置**：HTTP 404 / 500、`http.begin` 失败、UA / Authorization / 超时等参数在 HTTPClient 中的真实传递。

```bash
cd tools/jsontest && ./build.sh
# 开启 Sanitizer:
SAN=1 ./build.sh
```


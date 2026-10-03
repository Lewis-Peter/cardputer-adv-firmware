# Cardputer ADV 硬件参考

    自己写驱动时对照用。ADV 的屏幕和老 Cardputer 一样，但**键盘和音频换了芯片**，别照老 Cardputer 的代码抄。

来源：Bruce 固件板级配置 + M5GFX 源码 (`board_M5CardputerADV`)。

## 主控 / 存储
- ESP32-S3FN8 (Stamp-S3A)，双核 240MHz，8MB Flash，**无 PSRAM**
- USB VID/PID：`0x303A / 0x1001`（原生 USB-C，无独立串口芯片 → 需 `-DARDUINO_USB_CDC_ON_BOOT`）

## 屏幕 ST7789（⚠️ 与老 Cardputer 完全相同）
M5GFX 0.2.25+ 会自动识别 `M5CardputerADV`，一般不用手配。手配时参数：

| 项 | 值 |
|---|---|
| 驱动 | ST7789，135×240（rotation=1 → 240×135 横屏）|
| MOSI / SCLK / DC / CS / RST | 35 / 36 / 34 / 37 / 33 |
| 背光 BL | 38 |
| SPI 频率 | 40 MHz |
| offset_x / offset_y | 52 / 40 |
| invert | true |

## 键盘（⚠️ ADV 变了：TCA8418 I2C 键盘控制器）
老 Cardputer 是 GPIO 矩阵直扫；**ADV 改用 TCA8418 芯片**，所以 M5Cardputer 库的键盘代码在 ADV 上用不了。

| 项 | 值 |
|---|---|
| I2C 地址 | `0x34` |
| SDA / SCL | 8 / 9（内部 I2C 总线）|
| INT 中断脚 | 11 |
| 驱动库 | `adafruit/Adafruit TCA8418` |

> v1 做键盘对时时走这颗芯片：读 INT → I2C 取按键行列 → 映射到字符。

## 音频（⚠️ ADV 新增 ES8311 codec）
| 项 | 值 |
|---|---|
| Codec | ES8311，I2C 地址 `0x18` |
| 功放 | NS4150B → 1W 喇叭 |
| 麦克风 | MEMS（高信噪比）|

> M5Unified 的 `M5.Speaker` / `M5.Mic` 直接可用。现在 Mic 给 Spectrum 页做 FFT 频谱，
> Speaker 给倒计时闹铃和音量反馈的"嘀"声。（WAV 播放器写过又删了，一直没用上，
> 而且它的双缓冲常驻吃 2×4KB —— 这块板子没 PSRAM，省下来给别的用。）

## 其它外设
| 外设 | 参数 |
|---|---|
| 电池电量 ADC | GPIO 10 |
| 六轴 IMU | BMI270，内部 I2C（`M5.Imu`）|
| 红外发射 | **GPIO44**（TX，跟老款一样；官方文档确认）。⚠️ 只有发射、**没有接收**，学不了别人遥控器的码。注：3/4/5/6/13/15 其实是 Cap LoRa/GPS 占的脚，不是红外 |
| RGB LED | 数据 GPIO 21，**SK6812**（当普通 3 字节 GRB 驱动即可，不用 RGBW）。⚠️ **电源 PWR_EN 跟屏幕背光是同一条 GPIO 38**（官方引脚表 G38 = DISP_BL + RGB LED PWR_EN 共用），而 M5GFX 用 LEDC PWM 占着 G38 控背光。**所以千万别自己 pinMode/digitalWrite G38**——会把背光钉成常亮，导致亮度调节失效、自动熄屏关不掉屏（表现为"卡死但屏不灭"）。灯珠靠背光这条电供着：屏亮时能点灯，熄屏时随背光灭（硬件同网，正常）。驱动只碰数据脚 21 |
| microSD | CS=12, SCK=40, MISO=39, MOSI=14 |
| Grove 外部 I2C（侧面口）| SDA=2, SCL=1（`M5.Ex_I2C` / `Wire`）|
| 内部 I2C | SDA=8, SCL=9（`M5.In_I2C`）——键盘 0x34、codec 0x18、IMU、电源、Cap IO 扩 0x43 都在这条 |
| **Cap 上的 4-pin（HY2.0-4P Grove 口）** | **I2C，挂在内部总线(8/9)**，用 `M5.In_I2C` 读。⚠️ 跟键盘/codec/IMU 共线——插着 LoRa Cap 也能接 I2C Unit(温湿度/RFID…)，只要地址不跟内部那几颗撞。加上侧面 Grove(外部)，一共两条可用 I2C 总线 |

## 软件侧必须注意的两条

**GNSS 串口 RX 缓冲**：Cap 上的 ATGM336H 以 115200 波特持续吐 NMEA（约 11.5KB/s），
而 HardwareSerial 默认 RX 缓冲只有 256 字节 = **22ms** 的量。主循环一帧本来就要 30~50ms，
所以不放大缓冲就是每帧都在丢字节、NMEA 语句被拦腰截断。
`GPSSerial.setRxBufferSize(4096)` 必须在 `begin()` **之前**调（之后调不生效）。

**先接天线再上电**：Cap LoRa-1262 的射频前端不能空载发射，会永久损坏。

## 参考链接
- Bruce 板级配置：https://github.com/BruceDevices/firmware/blob/main/boards/m5stack-cardputer/m5stack-cardputer.ini
- M5GFX 板子定义：https://github.com/m5stack/M5GFX/blob/master/src/M5GFX.cpp
- 官方文档：https://docs.m5stack.com/en/core/Cardputer-Adv
- Cap LoRa-1262 文档（SX1262 引脚 + HY2.0-4P）：https://docs.m5stack.com/en/cap/Cap_LoRa-1262

# 依赖版本锁定（2026-09-24）

    结论先放这儿：**平台、Arduino core 和所有库都钉死在精确版本上**，
    升级任何一个都要按文末的步骤单独做、单独测。
    起因是同一份代码在服务器上全新构建，Flash 从 43.7% 涨到 48.5%、RAM 多了 6KB，
    而代码一行没改——只是 `^3.7` 这种版本范围解析到了更新的库。

---

## 当前锁定的版本

| 依赖 | 版本 | 备注 |
|---|---|---|
| platform `espressif32` | 7.0.1 | |
| `framework-arduinoespressif32` | 3.20017.241212 | Arduino 2.0.17 / ESP-IDF 4.4.7 |
| M5Unified | 0.2.20 | 升级见下文，必须和 M5GFX 一起动 |
| M5GFX | 0.2.27 | M5Unified 只声明了范围，不单独钉会被拉到新版 |
| FastLED | 3.10.3 | **不要升**，见下文 |
| ArduinoJson | 7.4.3 | |
| RadioLib | 7.7.1 | |
| TinyGPSPlus | 1.1.0 | |
| LibSSH-ESP32 | 5.9.0 | |
| ESP8266Audio | 1.9.9（git tag） | 更新的版本要 IDF5，本工程是 IDF4，编不过 |

---

## 为什么不"越新越好"

这块板子没有 PSRAM，全屏画布常驻 64.8KB，TLS 握手、GNSS 地图这些功能本来就是
"刚好够用"。库一升，静态 RAM 多几 KB，这些功能就可能从偶尔失败变成总是失败，
而且现象跟库版本八竿子打不着，很难查。

另外，本工程在好几处依赖库的**具体行为**：ES8311 寄存器直接写（频谱页的爆音抑制）、
GPIO38 同时是背光和 LED 电源（`led.cpp`）、麦克风/喇叭反复切换。库里这些地方一改，
编译照样通过，上机才发现不对。

不锁版本还有一个问题：本机、服务器、别的机器各自编出来的固件不一样，出了问题分不清
是代码改动还是库版本造成的。

---

## 实测：升级每个库的代价

在临时 worktree 里每次只升一个库、其余保持锁定版本，`pio run` 的结果：

| 组合 | RAM（字节） | Flash（字节） | 相对基线 |
|---|---|---|---|
| 基线（当前锁定版本） | 110,980 | 3,631,405 | — |
| 只升 M5GFX → 0.2.30 | 110,972 | 3,638,853 | Flash +7.4KB |
| 只升 M5Unified → 0.2.23 | — | — | 编译失败：要求 M5GFX ≥ 0.2.30 |
| M5Unified 0.2.23 + M5GFX 0.2.30 | 111,108 | 3,644,389 | RAM +128B，Flash +13KB |
| 只升 FastLED → 3.10.5 | 117,308 | 4,006,213 | **RAM +6.3KB，Flash +375KB** |
| 全部升级 | 117,444 | 4,019,265 | RAM +6.5KB，Flash +388KB |

### FastLED 3.10.5：不升

- 新加的音频/FFT 编译单元 `fl.audio+.cpp.o`（本工程用不上）引用了 `std::ios_base::Init`，
  链接器把 libstdc++ 的整套 iostream/locale 都拉了进来（`std::locale::_Impl`、
  `time_get`/`money_get`/`num_get`、宽字符格式化等）。增量里 `.flash.rodata` +227KB、
  `.flash.text` +147KB、`.dram0.bss` +6KB、IRAM +1KB。
- 驱动方式也变了：3.10.3 在 GPIO21 上用的是 RMT 硬件（`ClocklessController<21, SK6812>`），
  3.10.5 在这套 IDF4 core 上退回 `ClocklessBlockingGeneric`，由 CPU 阻塞翻转引脚。
  LED 灯效模式每 30ms 刷一次，会跟 I2S、WiFi 抢时间。
- 更新内容里没有对单颗 SK6812 有用的：3.10.4 的发布说明原话是
  "No FastLED source code changes"，3.10.5 的 122 个提交基本是 LPC/RP2040/C2、容器、测试和 CI。

等哪个版本去掉了 iostream 依赖、并在 IDF4 上仍然走 RMT，再考虑升。

### M5Unified 0.2.23 + M5GFX 0.2.30：有收益，但要专门测

M5Unified 0.2.23 强制要求 M5GFX ≥ 0.2.30，所以两个只能一起升。

收益：

- 麦克风的 `begin()`/`end()`/`record()` 做了串行化，ES8311 需要在 I2S 时钟运行后才写的
  配置有了 post-start 回调。频谱页正好在反复启停麦克风。
- 扬声器会把最后一段不满的缓冲也播完，播放器曲尾不再被截掉。
- 新增 `setBufferReleaseCallback`，可以替掉我们自己的 `audioWaitQueueSpace` 队列轮询。
- 几处 I2C/SPI 共享总线的竞态修复。

代价：

- **屏幕刷新变慢一倍。** M5GFX 0.2.28 起（PR #260）修正了 ESP32-S3 在 Arduino 下 SPI
  实际频率是请求值两倍的问题。Cardputer 配置的是 40MHz，所以现在屏幕其实一直跑在 80MHz；
  升级后会真正降到 40MHz，整屏 `pushSprite`（64.8KB）从约 6.5ms 变成约 13ms。
  所有页面都受影响，BadApple 这类直推屏幕的最明显。
- 麦克风录音缓冲从双缓冲翻转改成了单个 `rec_info`，频谱页依赖 `record()`/`isRecording()`，
  必须上机回归。
- 板型识别探测逻辑重写，改的正好是 BMI270/ES8311 所在的内部总线。
- Flash +13KB，RAM 基本不变。

### 平台 espressif32：没必要升

最新的 7.1.x 给的 Arduino core 仍然是 2.0.17（IDF 4.4.7），新增的只是 ESP-IDF 6.1 框架
支持，对 Arduino 工程没有收益。升不升都不影响 ESP8266Audio 1.9.9。

---

## 以后要升级某个库，怎么做

1. 开一个分支，只改 `platformio.ini` 里那**一个**依赖的版本号（M5Unified/M5GFX 例外，要一起改）。
2. 删掉 `.pio/libdeps` 让它重新拉，确认 `pio run` 输出的 Dependency Graph 里版本确实变了。
3. 记下 RAM/Flash，跟上面的基线比。静态 RAM 涨了超过 1KB 就要查清楚是什么。
4. 去看这个库在两个版本之间的更新日志和提交，重点找显示、音频、I2C/SPI、RMT、电源相关的改动。
5. 上机回归，至少过一遍：开机、频谱页（麦克风）、播放器/电台（喇叭）、LED 灯效、
   Chat（TLS 握手，最吃堆）、GNSS 地图（48KB 底图缓存）、BadApple（刷新速度）。
   用串口 `STAT` 看 `heap`/`largest`/`minEver` 有没有变差。
6. 没问题再合并，并更新本文的版本表和实测数据。

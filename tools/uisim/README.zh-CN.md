[English](README.md) | **简体中文**

# UI 模拟器（在主机端渲染屏幕）

240×135 太小、板子又不一定插着，光靠脑补坐标很容易把两块东西摞在一起。
这个工具**直接编译 `src/` 里那份真正的绘制代码**（不是照抄一份，所以不会跟固件走偏），
在主机端离屏渲染成 PNG，用来核对布局有没有重叠/出界。

```bash
pio run                                    # 先跑一次：M5GFX / ArduinoJson 是从
                                           # .pio/libdeps/ 里取的，没装依赖就没有这两个库
cp src/secrets.h.example src/secrets.h     # router.cpp / sats.cpp 都 #include 它
cd tools/uisim
./build.sh          # 编译 + 渲染，PNG 落在 out/
# macOS: open out/weather_1_now.png
# Linux: xdg-open out/weather_1_now.png
```

当前渲染这些页（`out/` 下同名 PNG）：

| 文件 | 页 |
|---|---|
| `weather_1_now` / `weather_2_hourly` / `weather_4_air` / `weather_5_aqi` / `weather_6_forecast` | Weather 五页 |
| `weather_2_hourly_cold` / `weather_6_forecast_cold` | 同样两页换一份冬天数据：温度标签变成 4 个字符（`-12C`／Imperial 的 `104F` 同宽），专看会不会被屏幕边切掉、会不会跟邻列压字 |
| `astro_1_daylight` / `astro_2_moon` / `astro_3_terminator` | Astro 三页 |
| `astro_1_daylight_polar_n` / `_polar_s` | Daylight 的极昼 / 极夜分支（纬度挪到 ±85，哪张是极昼随季节换） |
| `adsb` | ADS-B 飞机雷达 |
| `sats` | 卫星过顶 |
| `typhoon_1_alert` / `typhoon_2_track` | 台风预警 / 路径 |
| `quake_1_list` / `quake_2_map` / `quake_3_scrolled` | 地震列表 / 世界地图打点 / 列表滚到底 |
| `okx` | OKX 的 USDT/CNY 换算率（假数据里的值刻意写成**字符串**，跟 OKX v5 的真实形状一致）|
| `fx_1_rate` / `fx_2_days` | 汇率现价+走势 / 逐日收盘（假数据里刻意跳过周末，跟欧洲央行的公布节奏一致）|
| `router_0_connecting` / `router` | Router "连接中" 和正常状态 |
| `stopwatch` | 秒表 |
| `clock_1_hierarchic` / `_unsynced` | 时钟表盘 1：层次生活进度条（已对时 / 未对时）|
| `clock_2_analog` / `_unsynced` | 时钟表盘 2：经典模拟指针（毫秒插值秒针平滑扫秒，已对时 / 未对时）|
| `clock_3_digital` / `_unsynced` | 时钟表盘 3：大字数码表盘（Font7 数码管铺满，已对时 / 未对时）|
| `clock_4_text` / `_unsynced` | 时钟表盘 4：QlockTwo 极简文字表盘（已对时 / 未对时）|
| `gnss_1_overview_fix` / `_nofix` / `_rfoff` | GNSS 概览：天空图方位仰角分布、经纬度高度速度、未定位与射频关断状态 |
| `gnss_2_detail_deg` / `_dms` / `_grid` / `_utm` / `_neg_coords` / `_nofix` | GNSS 详情：4 种坐标制式（DEG/DMS/GRID/UTM）、南半球负坐标、未定位状态 |
| `gnss_3_sat_sky` / `_sky_crowded` / `_sky_empty` | GNSS 卫星页默认的天空图（与 Sats 同一坐标系）：常规数据 / 五星座挤在一片方位、无 SNR、三位数 PRN 的标签避让 / 没有卫星的空状态 |
| `gnss_3_sat_p1` / `_p2` | 同一页按 m 切出的卫星表：在用星（高亮绿点+星座鲜艳色）与可见星区分、信噪比与仰角方位角、两页垂直滚动 |
| `gnss_4_config_on` / `_off` | GNSS 配置：ATGM336H 频度/星系/动态模式/NMEA语句/天线供电胶囊指示 |
| `gnss_5_speed_120` / `_360` / `_1000` / `_nofix` | GNSS 速度表：半圆运动仪表盘，自适应 120/360/1000 三档量程、光弧与数码读数 |
| `gnss_6_trip` | GNSS 行程与诊断：TTFF、重捕耗时、里程、时长、极速极值、2分钟 SNR 曲线 |
| `spectrum_0_mic_unavailable` / `_1_bars` / `_2_waterfall` / `_3_vu_led` / `_4_rec_serial` | 音频频谱：柱状图、伪彩色瀑布图、单颗 RGB 电平指示、串口录音串流 |
| `hash_oven_0_cold` / `_baking` | Hash Oven 算力烤炉：跑分待机与双核满载烧机动画 |
| `hash_oven_1_aes_enc` / `_rc4_dec` | Hash Oven 对称加解密：AES-128-CBC 与 RC4 流密码 |
| `hash_oven_2_caesar` / `_base64` | Hash Oven 古典/CTF工具：凯撒 ROT13 移位与 Base64 双向转换 |
| `hash_oven_3_sha512_standby` / `_sha512_done` / `_sha256` | Hash Oven 密码哈希：Linux Shadow $6$ 与标准哈希计算 |
| `hash_oven_4_crack_standby` / `_crack_trying` | Hash Oven 穷举破解：PIN 字典爆破尝试与进度 ETA |
| `settings_1_p1` ~ `_p4` | 设置菜单 4 页列表：Wi-Fi、亮度、音量、LED、主题、休眠、时区等 13 项参数 |
| `settings_led_off` ~ `_music` | 设置 LED 模式：6 档工作模式微调与状态胶囊显示 |
| `settings_sub_bright` ~ `_format` | 设置二级弹窗：亮度和音量条、休眠超时、时区列表、电池状态、SD 格式化 |
| `ssh_cfg_1_password` ~ `_nowifi` | SSH 配置：主机/端口/用户/密码/SD卡私钥扫描、字号选择、连接按钮与参数弹窗 |
| `menu_0_tools` ~ `menu_6_more` | 主菜单：7 大功能分组轮播导航与磁吸选框动画 |
| `menu_theme_amber` | 主菜单：琥珀金等主题配色 |
| `about_1_info` / `_2_usage` / `_3_ram` / `_ram_scrolled` / `_ram_empty` | 关于系统：硬件规格、内存与存储用量、12 阶段 RAM Profile 消耗瀑布图与滚动 |

`data/` 里存的是**真实的 JMA 报文**（targetTc / specifications / forecast 各一份）。
台风那两页的假响应从这几个文件读，而不是塞进 `sim_main.cpp` 的字符串——5.7KB 的 JSON
写成 C 字符串既难看又容易在转义上出错，留着原文件以后还能拿去核对解析结果。

## 它是怎么骗过固件代码的

- `stubs/` 里放了一层极简替身：`M5Unified.h`（M5Canvas → 无父设备的 LGFX_Sprite）、
  `WiFi.h` / `HTTPClient.h` / `Preferences.h`、以及一个够用的 `String`。
  不能 `#define ARDUINO`——那会让 LovyanGFX 去找真正的 Arduino 运行时。
- 假 `HTTPClient` 按 `begin()` 的 URL path 从 `sim_main.cpp` 的响应表取 JSON，
  同时支持 `getString()` 和流式 `getStream()`/`getStreamPtr()`；因此 weather、ADS-B、
  卫星和 router 的取数函数都是真跑、真解析、真填页面状态。
- 渲染用 M5GFX 自带的 SDL 平台层（macOS 上 `brew install sdl2`，Linux 上 `sudo apt install libsdl2-dev`），但不开窗口，只在内存里画。
- PNG 由 `sim_main.cpp` 里几十行手写编码器输出（zlib 用 stored block），不引第三方库；
  默认放大 4 倍，不然 240×135 的字根本看不清。

## 它顺带还验了菜单表

`build.sh` 在编模拟器之前，先对 `src/globals.cpp` / `pages.cpp` / `icons.cpp` 做一遍
**只查语法**的编译。这三个文件不进模拟器二进制（`globals.cpp` 会跟 `sim_main.cpp` 抢同一批
全局定义），过一遍纯粹是为了 `globals.cpp` 里那两条 `static_assert`：

- `GROUPS[]` 必须首尾相接、正好盖满 `APPS[]`，且每组不超过一屏
- 组名必须放得进 tab 条（7 组时每格只有 34px）

加 app 最容易错的就是这两张表，而错法是**静默**的（某个 app 从菜单里消失、或者两组重叠着
显示同一个）。有了这一步，不用 `pio`、不插板子也能验它改对了没有——实测把 SKY 的 count
故意写小 1，这里当场报出那条断言。

## 自检会让 build.sh 真的挂掉

模拟器现在除了渲图还跑一条自检：fx 的解析器**没对过真实响应**（出口代理拦了所有免 key
汇率接口），所以拿同一份数据的两种响应形状各跑一遍，要求解出的点数一致。
挂了会打 `FAIL` **并让 `./build.sh` 返回非零**——只打一行字是拦不住人的，
没人会在几十行 `wrote out/xxx.png` 里注意到中间那句。PNG 照旧先拷出来再传播退出码，
自检挂了也还能看图。

⚠️ 写自检的时候栽过一次，值得记：`simResponseForPath` 是**第一个匹配就返回**，
所以想换某个 key 的假响应必须用 `simSetResponse()` 覆盖，往后面 `push_back` 一条同 key 的
**永远轮不到它**。第一版就是这么写的，结果它把同一份数据解了两遍还报 ok——
把要测的那个容忍分支故意删掉都照样通过。**自检写完一定要故意弄坏一次，看它响不响。**

## ⚠️ 假数据缺一个字段，那一页就等于没测

`simResponseForPath()` 匹配不到的 URL 会掉到 `simCannedResponse`，取数函数解不出东西，
页面就画它的空状态——**不报错，只是画面上一句 "no hourly data"**，看起来还挺正常。
所以加页面时要连它依赖的每一个接口一起给假响应，然后**真的把 PNG 打开看一眼**。

已经这么栽过：`simCannedResponse` 里一直没有 `hourly`，也没有空气质量那个端点的响应，
于是 "Next 24h" 和 AQI 两页在模拟器里长期只画空状态，整页布局从来没被看过。
2026-09-02 把假数据补齐，第一帧就露出 `drawWeatherHour()` 左边留白只有 14px
——而温度标签 "34C" 要 18px，右对齐之后首字符被推到屏幕外，常年显示成 "4C"。

**光有"典型值"还不够**：夏天那份假数据里温度全是 2 位数，3 个字符，
而 Forecast 页左右分列的标签正好在 3 个字符时还剩 2px 余量——看着没问题，一入冬
（`-12C`，4 个字符）就既压邻列又掉出屏幕。所以那两页各多渲一张冬天的
（`*_cold.png`），把"最长的那个标签"也画出来。

`data/usgs_quakes.json` 是**真实形状**的 USGS 摘要源报文（9 条，含一条 `mag: null` 的
——那是刚发生还没定级的事件，解析器必须跳过它，屏幕右上角因此写的是 "8 shown"）。
⚠️ 它的时间戳由 `sim_main.cpp` 的 `restampQuakes()` 整体平移到"现在"再喂进去：
文件里存的是抓取当天的绝对毫秒值，原样用的话页面上的"多久之前"会随日子越变越大，
几个月后截图上全是 `180d`，看着像坏了。只平移时间轴，别的字段一个没动。

`/above/`（N2YO 卫星）那份也栽过同一条：少了 `info.satcount` 的话 sats.cpp 直接判
"bad response (no 'info')"，整页只画一行红字。而且**光有响应还不够**——12 颗星原来
全给在本机附近 1 度内，于是全叠在天顶那一格，天际图的纵轴等于没画。现在按中心角
1~22 度铺开（550km 轨道的可见半径只有 23 度），从天顶到贴地平线都有点。
另外 ADS-B 的假数据里**刻意留了一架正北的飞机**：横轴两端都是 N，方位 0/360 的目标
会被屏幕边劈成两半，`drawWrapped` 就是为它写的，没有这样一架那条路径永远走不到。

⚠️ 还有一条跟 secrets 有关：`N2YO_API_KEY` 为空时 Sats 页在取数之前就返回了。
模拟器不发真请求，所以给 `src/secrets.h` 里填任意非空占位串即可（比如 `"SIMULATOR"`）。

同理，`/memory` 的假响应要给**两条**：mihomo 第一条是占位零值，
`router.cpp` 靠 `clashLine(..., skipObjects=1)` 跳掉它。只给一条的话 MEM 恒为 0M，
正好把那个已经修过的 bug 在模拟器里又演一遍。

## 加新页面

在 `sim_main.cpp` 的 `main()` 里加一行 `shoot("out/xxx.png", drawXxx);`，
再把对应的 `src/xxx.cpp` 加进 `build.sh` 的编译列表即可。
如果那个页面依赖别的模块（比如 `M5.Power`、SD 卡），要么在 `sim_main.cpp` 里补个假实现，
要么就别编进来——这工具只关心"画得对不对"。

## ⚠️ 一条铁律：不要在模拟器里"抄一份"绘制代码

模拟器的全部价值在于**编译 `src/` 里那份真代码**。曾经有一次为了图省事，把
`drawSkyBg()` 复制进 `sim_main.cpp`（因为它当时在 `ui_common.cpp` 里，而那个文件
拖着 M5.Display/icons/worldmap 一堆板级依赖，桌面编不动）——结果改了 `src/` 那份，
渲染出来的图纹丝不动，白查半天。

正确做法是**把这类共用绘制件拆到一个没有板级依赖的小文件**（比如 `src/skyview.cpp`），
然后把它加进 `build.sh` 的编译列表。

`sim_main.cpp` 里唯一允许的重复是 `trunc()` / `nowHM()` / `drawPageDots()` 这三个
几行的小工具（连同注释一起标注了原因）。除此之外一律编译真文件。

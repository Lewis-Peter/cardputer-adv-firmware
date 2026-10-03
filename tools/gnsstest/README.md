# GNSS 坐标换算、行程统计与 GSA 解析测试台

`src/gnss.cpp` 里的核心数学与协议处理逻辑：
1. **坐标换算**：
   - 度分秒（`formatDms`）：正负符号、0 度、±180 度、59.96" 四舍五入进位（防止出现 60.0"）。
   - Maidenhead 6 位网格（`toMaidenhead`）：上海（PM01rf）、格林尼治（IO91xl）、悉尼歌剧院（QF56od）、时代广场（FN30as）及极值边界截断保护。
   - UTM 投影换算（`toUtm`）：WGS-84 椭球高斯投影系列展开，核对上海（51N）、悉尼（56S 南半球假北距 +10,000,000m）、格林尼治（30N），误差在 1 米以内；80°S ~ 84°N 超界保护与中央经线带号边界。
2. **行程统计与跳点过滤**（`gnssTripProcessPoint`）：
   - 静止漂移（速度 < 3.0 km/h，距离与耗时保持为 0）。
   - 匀速运动（36 km/h，里程与时间正常累加）。
   - 瞬时跳点（1000m 飞点被丢弃，不计入里程）。
   - 丢星恢复与 TTFF 首次定位耗时统计。
3. **GSA 语句解析**（`gnssProcessGSA`）：
   - GPGSA / BDGSA 在用卫星列表合并与统计。
   - NMEA 4.10 systemId（1=GPS, 4=BDS）扩展字段解析。
   - 无定位（fix=1）清空在用状态，畸变报文保护。

## 运行方法

```bash
cd tools/gnsstest && ./build.sh
# 开启 AddressSanitizer 和 UndefinedBehaviorSanitizer:
SAN=1 ./build.sh
```

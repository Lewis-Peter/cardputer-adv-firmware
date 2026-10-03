# 文字表盘 (QlockTwo) 取词测试台

`src/clock.cpp` 里的极简文字表盘（QlockTwo 风格矩阵）将时间按 5 分钟取整并组合英文单词高亮。

测试内容：
1. **关键边界时刻测试**：
   - 11:58 -> IT IS TWELVE O'CLOCK PM（正午 12:00 PM 边界）
   - 23:58 -> IT IS TWELVE O'CLOCK AM（午夜 12:00 AM 边界）
   - 12:30 -> IT IS HALF PAST TWELVE PM
   - 00:03 -> IT IS FIVE PAST TWELVE AM（四舍五入到 5 分钟）
   - 11:40 -> IT IS TWENTY TO TWELVE PM
   - 23:40 -> IT IS TWENTY TO TWELVE AM
   - 12:45 -> IT IS QUARTER TO ONE PM
   - 00:45 -> IT IS QUARTER TO ONE AM
2. **12 档 5 分钟词组合状态机验证**：
   - O'CLOCK / PAST / TO 的互斥性
   - HALF / QUARTER / TWENTY / FIVE / TEN 的精确组合
3. **24 小时 x 60 分钟（共 1440 组）全量遍历不变量验证**：
   - AM / PM 严格互斥且正确对应目标小时
   - 目标小时严格位于 1..12
   - 句子语法组合严格有效

## 运行方法

```bash
cd tools/clocktest && ./build.sh
# 开启 AddressSanitizer:
SAN=1 ./build.sh
```

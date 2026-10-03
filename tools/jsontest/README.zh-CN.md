[English](README.md) | **简体中文**

# HTTP JSON 与流式 JSON 数组解析器测试台

`src/http_json.h` 封装了固件访问网络 API 的 JSON 解析逻辑，特别是专为 Cardputer ADV（ESP32-S3 无 PSRAM，RAM 紧凑）设计的 `fetchJsonStreamArray`：逐项反序列化超大数组（如 GitHub 365天热力图、USGS 地震列表），避免整包解析产生数十 KB 堆分配导致 OOM。

本测试台使用宿主机 g++ 配合 `stubs/`（模拟 `Stream`、`HTTPClient`、时间推进与延时），在不依赖真机和真实网络的前提下对所有边界条件进行全量断言验证。

## 测试内容

1. **正常数组流式解析 (`fetchJsonStreamArray`)**：
   - 多项对象数组逐项解析，验证索引递增与字段值提取
   - 带有空格、制表符、缩进与换行的美化 JSON 兼容
   - 嵌套对象与子数组复合结构解析
   - 字符串列表与布尔/空值数组解析
2. **空数组处理**：
   - 紧凑空数组 `[]`
   - 包含空白换行的空数组 `[ \r\n\t ]`
   - 带 `arrayKey` 前缀定位的空数组 `{"items":[]}`
3. **前缀定位与键匹配 (`arrayKey`)**：
   - 包含嵌套外壳对象的数组定位提取
   - 目标键不存在时正确返回 `false` 并报错 `"array not found"`
4. **中途截断与语法错误**：
   - 元素内部意外截断（如 `[{"id":1},{"id":` EOF），捕获 `json error`
   - 尾随逗号后直接 EOF，捕获语法错误
   - 元素结束后缺少闭合中括号 `]`，正确捕获流超时
5. **分隔符异常处理**：
   - 非法分隔符（如分号 `[{"a":1};{"b":2}]`），正确捕获 `"unexpected separator"`
   - 缺少分隔符（两个对象间仅有空格），正确捕获 `"unexpected separator"`
6. **流超时与无响应处理**：
   - 首字符读取超时（空数据流直到超时），正确返回 `"stream timeout"`
   - 全空白字符耗尽超时
7. **数据量控制与提前中止**：
   - `maxItems` 参数截断（读满指定数量后正常返回 `true`，后续流不读取）
   - 回调函数 `onItem` 返回 `false` 提前中止，正常返回 `true`
8. **ArduinoJson 过滤机制 (`Filter`)**：
   - 配合 `JsonDocument filter`，验证只保留指定字段，过滤丢弃冗余字段
9. **普通 JSON 解析 (`fetchJsonHttp`)**：
   - `options.stream = true` 流式单对象解析
   - `options.stream = false` 内存整包（getString）解析
   - `filter` 字段过滤
   - 语法错误及 `includeJsonDetail` / 自定义 `jsonError` 错误信息格式
10. **HTTP 客户端状态与配置传递**：
    - HTTP 404 / 500 状态码返回及错误信息
    - `http.begin` 失败场景
    - User-Agent、Authorization Header、连接超时、读取超时及 HTTP 1.0 协议配置验证

## 运行方法

```bash
cd tools/jsontest && ./build.sh
# 开启 AddressSanitizer 和 UndefinedBehaviorSanitizer:
SAN=1 ./build.sh
```

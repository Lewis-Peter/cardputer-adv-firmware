# Hash Oven 穷举模式猜测生成测试台

`src/hash_oven.cpp` 里的荒谬穷举（Futility Cracker）模式：根据当前选中的 keyspace 档位和尝试索引（0..total-1），生成对应的暴力破解候选口令。

测试内容：
1. **Keyspace 预设字符集与长度数学核对**：
   - 字符集无重复字符
   - 总空间大小 `total == base^len`（如 10^4、26^4、26^6、26^8、62^8）
   - 各档预设的 solution 字符均在对应字符集中
2. **索引全空间覆盖与单射无碰撞验证**：
   - 4-Digits 档：0..9999 全量 10,000 个索引遍历，验证严格一一映射、无重复、无遗漏
   - 4-Lower 档：0..456,975 全量 456,976 个索引遍历，验证全覆盖
   - 大 keyspace 档：10 万点跨空间步进采样，双向编码反解 100% 恒等
3. **各档 solution 命中生成验证**：
   - 验证每一档的预设 solution 均能在对应的索引处精确生成，且索引严格小于 `total`
4. **缓冲区截断与入参异常保护**：
   - 小缓冲区安全截断，不越界写
   - 非法 keyspace 下标和空缓冲区安全保护

## 运行方法

```bash
cd tools/oventest && ./build.sh
# 开启 AddressSanitizer:
SAN=1 ./build.sh
```

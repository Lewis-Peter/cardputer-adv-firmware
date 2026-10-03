#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// 计算标准 glibc 兼容的 SHA-512-crypt ($6$) 密码哈希
// key: 密码明文
// salt: 盐值字符串（最多截取 16 字节，或到 '$' 为止）
// rounds: 迭代轮数（默认 5000，合法范围 1000 ~ 999999999）
// out: 输出缓冲区（建议至少 128 字节）
// out_max: 输出缓冲区大小
// progress_rounds: 可选指针，每完成若干轮写入当前轮数 (0..rounds)
// cancel_flag: 可选指针，若被置为 true 则提前中止计算
// 返回值: 成功返回 true，失败返回 false
bool sha512_crypt_calc(const char* key, const char* salt, int rounds,
                       char* out, size_t out_max,
                       volatile int* progress_rounds = nullptr,
                       volatile bool* cancel_flag = nullptr);

#ifdef __cplusplus
}
#endif

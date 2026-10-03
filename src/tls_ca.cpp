#include "tls_ca.h"
#include <ctime>
#include <mbedtls/aes.h>
#include <mbedtls/bignum.h>

// 链接器给嵌入文件生成的符号：路径 data/cert/x509_crt_bundle.bin
// -> _binary_data_cert_x509_crt_bundle_bin_start（把 / 和 . 换成 _）
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

void tlsUseCaBundle(WiFiClientSecure& client) {
  client.setCACertBundle(rootca_crt_bundle_start);
}

bool tlsClockReady() {
  // 只要不是 1970 附近就算对上了。用 timeSynced 也行，但这里更直接：证书校验关心的
  // 就是"当前时间是不是一个真实的年份"。2020-01-01 = 1577836800。
  return time(nullptr) > 1577836800;
}

// 第一次 TLS 握手会在 IDF 4.4 的硬件加速层里懒建几样**永不释放**的小东西：
//   - s_crypto_mpi_lock：newlib 静态锁，第一次 _lock_acquire 时才 malloc 一个互斥量（ECDHE/RSA 用 MPI）
//   - s_crypto_sha_aes_hmac_ds_lock：同上（Wi-Fi 的 WPA2 PBKDF2 多半已经建过，但不保证）
//   - esp_crypto_shared_gdma 的 rx/tx_channel：gdma_new_channel 各 calloc 一块，
//     esp_crypto_shared_gdma_free() 全固件没人调，建了就一直在
//   - AES 中断：单次 DMA 超过 AES_DMA_INTR_TRIG_LEN(2000B) 才走中断，第一次会 esp_intr_alloc
//     （vector_desc / intr handle 都是 malloc 的，永不释放）
// 这些都发生在 CanvasLease 里（Sats 通常是开机后第一个 HTTPS 页），于是落进画布腾出的洞里
// 把画布钉住——同 lwipSockLockWarmup 那个坑。开机堆还完整时先建好，就集中在一起了。
void tlsCryptoWarmup() {
  // AES-CBC 走 DMA，长度 > 2000 触发中断路径：一次把锁、GDMA 通道、中断都建好
  const size_t LEN = 2048 + 16;
  uint8_t* buf = (uint8_t*)calloc(1, LEN);
  if (buf) {
    uint8_t key[16] = {0}, iv[16] = {0};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, LEN, iv, buf, buf);
    mbedtls_aes_free(&aes);
    free(buf);
  }
  // MPI 硬件锁：exp_mod 在 esp_bignum 里一定走硬件
  mbedtls_mpi a, e, n, x;
  mbedtls_mpi_init(&a); mbedtls_mpi_init(&e); mbedtls_mpi_init(&n); mbedtls_mpi_init(&x);
  mbedtls_mpi_lset(&a, 3); mbedtls_mpi_lset(&e, 5); mbedtls_mpi_lset(&n, 7);
  mbedtls_mpi_exp_mod(&x, &a, &e, &n, nullptr);
  mbedtls_mpi_free(&a); mbedtls_mpi_free(&e); mbedtls_mpi_free(&n); mbedtls_mpi_free(&x);
}

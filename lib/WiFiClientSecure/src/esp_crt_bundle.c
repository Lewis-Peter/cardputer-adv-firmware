// Copyright 2018-2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include <string.h>
#include <esp_system.h>
#include <esp32-hal-log.h>
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include <mbedtls/pk.h>          // 本地补丁：mbedtls_pk_write_pubkey_der（比对公钥）
#include <stdlib.h>               // malloc/free

#define BUNDLE_HEADER_OFFSET 2
#define CRT_HEADER_OFFSET 4

/* a dummy certificate so that
 * cacert_ptr passes non-NULL check during handshake */
static mbedtls_x509_crt s_dummy_crt;


typedef struct crt_bundle_t {
    const uint8_t **crts;
    uint16_t num_certs;
    size_t x509_crt_bundle_len;
} crt_bundle_t;

static crt_bundle_t s_crt_bundle;

static int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int data, uint32_t *flags);
static int esp_crt_check_signature(mbedtls_x509_crt *child, const uint8_t *pub_key_buf, size_t pub_key_len);


static int esp_crt_check_signature(mbedtls_x509_crt *child, const uint8_t *pub_key_buf, size_t pub_key_len)
{
    int ret = 0;
    mbedtls_x509_crt parent;
    const mbedtls_md_info_t *md_info;
    unsigned char hash[MBEDTLS_MD_MAX_SIZE];

    mbedtls_x509_crt_init(&parent);

    if ( (ret = mbedtls_pk_parse_public_key(&parent.pk, pub_key_buf, pub_key_len) ) != 0) {
        log_e("PK parse failed with error %X", ret);
        goto cleanup;
    }


    // Fast check to avoid expensive computations when not necessary
    if (!mbedtls_pk_can_do(&parent.pk, child->sig_pk)) {
        log_e("Simple compare failed");
        ret = -1;
        goto cleanup;
    }

    md_info = mbedtls_md_info_from_type(child->sig_md);
    if ( (ret = mbedtls_md( md_info, child->tbs.p, child->tbs.len, hash )) != 0 ) {
        log_e("Internal mbedTLS error %X", ret);
        goto cleanup;
    }

    if ( (ret = mbedtls_pk_verify_ext( child->sig_pk, child->sig_opts, &parent.pk,
                                       child->sig_md, hash, mbedtls_md_get_size( md_info ),
                                       child->sig.p, child->sig.len )) != 0 ) {

        log_e("PK verify failed with error %X", ret);
        goto cleanup;
    }
cleanup:
    mbedtls_x509_crt_free(&parent);

    return ret;
}


/* This callback is called for every certificate in the chain. If the chain
 * is proper each intermediate certificate is validated through its parent
 * in the x509_crt_verify_chain() function. So this callback should
 * only verify the first untrusted link in the chain is signed by the
 * root certificate in the trusted bundle
*/

/* ===================== 本地补丁开始（上游 WiFiClientSecure 2.0.0 没有） =====================
 *
 * 上游的 esp_crt_verify_callback 只做一件事：拿**链尾**那张证书的 issuer 去 bundle 里查根。
 * 这对"服务端不发根"或"服务端发自签根"的链都成立，但对**交叉签名的根**是错的：
 *
 *   api.frankfurter.app / www.okx.com 发的链是
 *       leaf <- GTS Root R4(WE1) <- GTS Root R4
 *   而这张 GTS Root R4 是交叉签名版：
 *       subject = C=US, O=Google Trust Services LLC, CN=GTS Root R4
 *       issuer  = C=BE, O=GlobalSign nv-sa, OU=Root CA, CN=GlobalSign Root CA   <- 不是自签
 *   于是上游拿 "GlobalSign Root CA"(R1) 去查——那张根 Mozilla 已经移除了，bundle 里没有
 *   （本机 /etc/ssl/certs 同样只剩 E46/R46/R3/R6/ECC-R4/ECC-R5），查不到就走到函数末尾
 *   报 "Failed to verify certificate"，**一个内层错误都不打**，看着就像证书不受信。
 *   Google 至今仍发交叉签名版是为了兼容老设备，这条链在浏览器里是好的。
 *
 * 正确的语义是浏览器的做法：**受信任的根出现在链中间就该停下来**。所以这里补一条路径——
 * 链上任意一张证书，只要它的 subject 命中 bundle **且公钥逐字节相同**，就认为链锚定在了
 * 一个受信任的密钥上，放行。
 *
 * 为什么这样是安全的：mbedTLS 在调用本回调之前，已经把链上相邻证书之间的签名逐段验过了
 * （回调是带着 NOT_TRUSTED 标志、从链尾往下调的）。所以"链尾这张的公钥 == 某个受信任根的
 * 公钥"就等于"整条链锚定在那个受信任根上"。⚠️ 名字相同是不够的，必须比公钥——只比名字
 * 等于让任何人自签一张 CN 相同的证书就能冒充。
 *
 * 同步提醒：这份 WiFiClientSecure 是 framework-arduinoespressif32 里那份的副本（2.0.0），
 * 靠 PlatformIO 的 lib/ 优先级盖过上游。升级 core 之后要重新对一遍这个文件。
 */

/* 按 subject DER 在 bundle 里二分查找，命中返回下标，否则 -1。
   bundle 由 gen_crt_bundle.py 生成时就按名字排好序了。 */
static int esp_crt_bundle_lookup(const unsigned char *name_der)
{
    int start = 0;
    int end = s_crt_bundle.num_certs - 1;
    int middle = (end - start) / 2;

    while (start <= end) {
        size_t nlen = s_crt_bundle.crts[middle][0] << 8 | s_crt_bundle.crts[middle][1];
        const uint8_t *nm = s_crt_bundle.crts[middle] + CRT_HEADER_OFFSET;

        int cmp_res = memcmp(name_der, nm, nlen);
        if (cmp_res == 0) {
            return middle;
        } else if (cmp_res < 0) {
            end = middle - 1;
        } else {
            start = middle + 1;
        }
        middle = (start + end) / 2;
    }
    return -1;
}

/* crt 自己是不是 bundle 里的某个根（subject 命中 + 公钥逐字节一致）。 */
static bool esp_crt_bundle_anchor_in_chain(const mbedtls_x509_crt *crt)
{
    int idx = esp_crt_bundle_lookup(crt->subject_raw.p);
    if (idx < 0) {
        return false;
    }

    size_t name_len = s_crt_bundle.crts[idx][0] << 8 | s_crt_bundle.crts[idx][1];
    size_t key_len  = s_crt_bundle.crts[idx][2] << 8 | s_crt_bundle.crts[idx][3];
    const uint8_t *bundle_key = s_crt_bundle.crts[idx] + CRT_HEADER_OFFSET + name_len;

    /* ⚠️ 放堆上不放栈上：这条路径跑在 loop 任务里，STAT 量到 loopStackFree 只有 ~2.4KB，
       而 RSA-4096 的 SubjectPublicKeyInfo 就要 550 多字节，栈上开缓冲是在赌。 */
    const size_t buf_len = 800;
    unsigned char *buf = malloc(buf_len);
    if (buf == NULL) {
        log_e("anchor check: out of memory");
        return false;
    }

    /* mbedtls_pk_write_pubkey_der 从缓冲区**末尾往前**写，返回写入长度。 */
    int written = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)&crt->pk, buf, buf_len);
    bool match = (written > 0) && ((size_t)written == key_len) &&
                 (memcmp(buf + buf_len - written, bundle_key, key_len) == 0);

    free(buf);
    if (match) {
        log_i("Chain anchored on a trusted root present mid-chain");
    }
    return match;
}
/* ====================== 本地补丁结束 ====================== */

int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    mbedtls_x509_crt *child = crt;

    /* It's OK for a trusted cert to have a weak signature hash alg.
       as we already trust this certificate */
    uint32_t flags_filtered = *flags & ~(MBEDTLS_X509_BADCERT_BAD_MD);

    if (flags_filtered != MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        return 0;
    }


    if (s_crt_bundle.crts == NULL) {
        log_e("No certificates in bundle");
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }

    log_d("%d certificates in bundle", s_crt_bundle.num_certs);

    size_t name_len = 0;
    const uint8_t *crt_name;

    /* ⚠️ 本地补丁（上游 2.0.0 没有这段）——详见 esp_crt_bundle_anchor_in_chain 上面那段。
       **先**看这张证书自己是不是 bundle 里的某个根：命中就直接放行，连签名都不用验。

       放在最前面有两个好处：
        1) 交叉签名的根从此能过。服务端发的 GTS Root R4 的 issuer 是早被 Mozilla 移除的
           GlobalSign Root CA (R1)，按 issuer 查永远查不到，但 R4 本人就在 bundle 里。
        2) 顺手把堆峰值削掉了。原来的路径要对链尾那张根做一次完整的签名验证，
           jogruber 那条链的 ISRG Root YR 是 RSA-4096，握手期就靠这一下把堆抽干——
           串口报的是 `PK verify failed with error FFFFBD70`，而 0xFFFFBD70 =
           RSA_PUBLIC_FAILED + **MPI_ALLOC_FAILED**，本质是 malloc 失败被当成了校验失败。
           改成比公钥字节之后，这一步只要一次 memcmp 加一个 800 字节的临时缓冲。
       安全性没有放松：这等价于"链锚定在一个受信任的公钥上"，理由见下面那段。 */
    if (esp_crt_bundle_anchor_in_chain(child)) {
        *flags = 0;
        return 0;
    }

    /* 链尾不是根（服务端没发根，只发到中间证书）时，还是走上游那条按 issuer 查根、
       再验签名的老路。usgs 那种 chain=2 的链就走这里。 */
    bool crt_found = false;
    int middle = esp_crt_bundle_lookup(child->issuer_raw.p);
    if (middle >= 0) {
        crt_found = true;
        name_len = s_crt_bundle.crts[middle][0] << 8 | s_crt_bundle.crts[middle][1];
    }

    int ret = MBEDTLS_ERR_X509_FATAL_ERROR;
    if (crt_found) {
        size_t key_len = s_crt_bundle.crts[middle][2] << 8 | s_crt_bundle.crts[middle][3];

        ret = esp_crt_check_signature(child, s_crt_bundle.crts[middle] + CRT_HEADER_OFFSET + name_len, key_len);
    }

    if (ret == 0) {
        log_i("Certificate validated");
        *flags = 0;
        return 0;
    }

    log_e("Failed to verify certificate");
    return MBEDTLS_ERR_X509_FATAL_ERROR;
}


/* Initialize the bundle into an array so we can do binary search for certs,
   the bundle generated by the python utility is already presorted by subject name
 */
static esp_err_t esp_crt_bundle_init(const uint8_t *x509_bundle)
{
    s_crt_bundle.num_certs = (x509_bundle[0] << 8) | x509_bundle[1];
    s_crt_bundle.crts = calloc(s_crt_bundle.num_certs, sizeof(x509_bundle));

    if (s_crt_bundle.crts == NULL) {
        log_e("Unable to allocate memory for bundle");
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *cur_crt;
    cur_crt = x509_bundle + BUNDLE_HEADER_OFFSET;

    for (int i = 0; i < s_crt_bundle.num_certs; i++) {
        s_crt_bundle.crts[i] = cur_crt;

        size_t name_len = cur_crt[0] << 8 | cur_crt[1];
        size_t key_len = cur_crt[2] << 8 | cur_crt[3];
        cur_crt = cur_crt + CRT_HEADER_OFFSET + name_len + key_len;
    }

    return ESP_OK;
}

esp_err_t arduino_esp_crt_bundle_attach(void *conf)
{
    esp_err_t ret = ESP_OK;
    // If no bundle has been set by the user then use the bundle embedded in the binary
    if (s_crt_bundle.crts == NULL) {
        log_e("Failed to attach bundle");
        return ret;
    }

    if (conf) {
        /* point to a dummy certificate
         * This is only required so that the
         * cacert_ptr passes non-NULL check during handshake
         */
        mbedtls_ssl_config *ssl_conf = (mbedtls_ssl_config *)conf;
        mbedtls_x509_crt_init(&s_dummy_crt);
        mbedtls_ssl_conf_ca_chain(ssl_conf, &s_dummy_crt, NULL);
        mbedtls_ssl_conf_verify(ssl_conf, esp_crt_verify_callback, NULL);
    }

    return ret;
}

void arduino_esp_crt_bundle_detach(mbedtls_ssl_config *conf)
{
    free(s_crt_bundle.crts);
    s_crt_bundle.crts = NULL;
    if (conf) {
        mbedtls_ssl_conf_verify(conf, NULL, NULL);
    }
}

void arduino_esp_crt_bundle_set(const uint8_t *x509_bundle)
{
    // Free any previously used bundle
    free(s_crt_bundle.crts);
    esp_crt_bundle_init(x509_bundle);
}

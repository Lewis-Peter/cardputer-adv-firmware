#!/usr/bin/env python3
"""生成 data/cert/x509_crt_bundle.bin —— 系统根证书 + 少量「构建时验过」的交叉签名根。

为什么不是直接把 Mozilla 根证书集打包了事（原来就是那么做的，2026-09-03 被撞破）：

  证书链的**链尾**不一定是自签根。CA 换新根时会先发一张「交叉签名」版：subject 是新根，
  issuer 是老根。服务端把它一起发下来，好让还不认识新根的客户端能顺着老根验下去。

  而 esp_crt_bundle 的校验回调只干一件事：拿链尾那张的 **issuer** 去 bundle 里查根，
  查到就验一次签名。于是交叉签名根有两种翻车方式，我们两种都撞上了：

    1. 老根已经被 Mozilla 移除 —— 查都查不到。
       api.frankfurter.app / www.okx.com 的链尾是交叉签名版 GTS Root R4，
       issuer 是 GlobalSign Root CA (R1)，那张根早被移除了，于是直接报
       "Failed to verify certificate"，一个内层错误都不打，看着像证书不受信。

    2. 老根还在，但那次验签太贵 —— 堆不够。
       github-contributions-api.jogruber.de / api.n2yo.com 的链尾是交叉签名版
       `C=US, O=ISRG, CN=Root YR`，issuer 是 ISRG Root X1（在库里），但 Root YR 是
       **RSA-4096**，验它要几十 KB 的 MPI 临时空间。这块板子握手期只剩几 KB，
       于是 malloc 失败被 mbedTLS 一路上报成 `PK verify failed with error FFFFBD70`
       （= RSA_PUBLIC_FAILED + MPI_ALLOC_FAILED），又一次看着像证书问题。

  配套的 lib/WiFiClientSecure 补丁加了条快路径：**链尾那张自己**要是就在 bundle 里
  （subject 命中 + 公钥逐字节相同），直接放行，连签名都不用验。这个脚本负责喂饱那条快
  路径——把这些交叉签名根**在电脑上验一次**，通过了才并进 bundle。

  也就是把那次 RSA-4096 验签从设备的运行时挪到了这里的构建时。密码学上是等价的：
  我们没有凭空信任任何新根，只是缓存了一条「已被受信任根背书」的结论。⚠️ 所以下面
  `openssl verify` 那一步是这个脚本的**安全边界**，验不过就必须让整个生成失败。

用法：
    python3 tools/gen_cert_bundle.py            # 就地更新 data/cert/x509_crt_bundle.bin
    python3 tools/gen_cert_bundle.py --dry-run  # 只报告会加哪些锚，不写文件

依赖：python 的 cryptography 包、openssl 命令、能联网（要去各主机取一次证书链）。
"""

import argparse
import os
import re
import shutil
import subprocess
import struct
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "cert", "x509_crt_bundle.bin")
SYSTEM_CA = "/etc/ssl/certs/ca-certificates.crt"

# ⚠️ 用 v5.1.2 那版：master 版依赖 rich_click，这版是 argparse、没有额外依赖。
GEN_URL = ("https://raw.githubusercontent.com/espressif/esp-idf/v5.1.2/"
           "components/mbedtls/esp_crt_bundle/gen_crt_bundle.py")


def hosts_from_sources():
    """从 src/ 里把项目真正访问的 https 主机名抠出来。

    写死一份名单迟早会和代码漂移——加了新页却忘了更新名单，就又是一次"只有这一页连不上"
    的排查。直接从源码扫，漏不掉。
    """
    hosts = set()
    src = os.path.join(ROOT, "src")
    for name in sorted(os.listdir(src)):
        if not name.endswith((".cpp", ".h")):
            continue
        text = open(os.path.join(src, name), encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"https://([A-Za-z0-9.\-]+)", text):
            hosts.add(m.group(1))
    return sorted(hosts)


def fetch_chain_tail(host):
    """取一条链的**最后一张**证书（PEM）。取不到就返回 None（主机可能已经下线）。"""
    try:
        out = subprocess.run(
            ["openssl", "s_client", "-connect", f"{host}:443",
             "-servername", host, "-showcerts"],
            input=b"", capture_output=True, timeout=20).stdout.decode("utf-8", "replace")
    except (subprocess.TimeoutExpired, OSError):
        return None
    certs = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                       out, re.S)
    return certs[-1] if certs else None


def cert_facts(pem_path):
    """返回 (subject DER, issuer DER, SubjectPublicKeyInfo DER)——和 bundle 里存的对齐。"""
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    cert = x509.load_pem_x509_certificate(open(pem_path, "rb").read())
    return (cert.subject.public_bytes(),
            cert.issuer.public_bytes(),
            cert.public_key().public_bytes(
                serialization.Encoding.DER,
                serialization.PublicFormat.SubjectPublicKeyInfo))


def rsa_bits(spki_der):
    """SPKI 是 RSA 就返回位数，否则返回 0（EC 的验签开销小得多，不在考虑之列）。"""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    try:
        key = serialization.load_der_public_key(spki_der)
    except Exception:
        return 0
    return key.key_size if isinstance(key, rsa.RSAPublicKey) else 0


# 设备上那次验签要多大的 MPI 临时空间，主要由**签发者公钥**的位数决定。这块板子握手期
# 只剩几 KB，RSA-4096 那一下必然 malloc 失败。2048 位的实测没问题（usgs 那条链一直是好的），
# 所以门槛划在中间。
EXPENSIVE_RSA_BITS = 3072


def parse_bundle(path):
    """把 bundle 拆成 [(subject DER, pubkey DER)]，顺带自检结构完整。"""
    d = open(path, "rb").read()
    n = struct.unpack(">H", d[:2])[0]
    off, ents = 2, []
    for _ in range(n):
        nl, kl = struct.unpack(">HH", d[off:off + 4])
        off += 4
        ents.append((d[off:off + nl], d[off + nl:off + nl + kl]))
        off += nl + kl
    if off != len(d):
        raise SystemExit(f"bundle 结构对不上：解析到 {off}，文件 {len(d)}")
    return ents


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="只报告，不写 bundle")
    ap.add_argument("--ca", default=SYSTEM_CA, help=f"根证书来源（默认 {SYSTEM_CA}）")
    args = ap.parse_args()

    if not os.path.exists(args.ca):
        raise SystemExit(f"找不到根证书来源：{args.ca}")

    tmp = tempfile.mkdtemp(prefix="crtbundle-")
    gen = os.path.join(tmp, "gen_crt_bundle.py")
    print(f"[1/4] 取 gen_crt_bundle.py（esp-idf v5.1.2）")
    urllib.request.urlretrieve(GEN_URL, gen)

    base_pem = os.path.join(tmp, "base.pem")
    shutil.copy(args.ca, base_pem)

    print(f"[2/4] 扫源码里的 https 主机，逐个看链尾是不是已经受信任的根")
    extra = []            # [(host, pem_path, subject_cn, reason)]
    seen_keys = set()
    bundle_by_subject = {}
    if os.path.exists(OUT):
        for subj, key in parse_bundle(OUT):
            bundle_by_subject[subj] = key

    for host in hosts_from_sources():
        pem = fetch_chain_tail(host)
        if pem is None:
            print(f"    - {host:45s} 连不上，跳过")
            continue
        tail = os.path.join(tmp, f"tail_{host}.pem")
        open(tail, "w").write(pem + "\n")
        subj, issuer, key = cert_facts(tail)

        if key in seen_keys:
            print(f"    - {host:45s} 链尾本轮已并入过")
            continue
        # 链尾本身就是库里的根：设备上直接命中 anchor 快路径，什么都不用做。
        if key in bundle_by_subject.values():
            print(f"    - {host:45s} 链尾已是受信任的根")
            continue

        # 老路径（按 issuer 查根 + 验签）能不能走通？走得通就别往 bundle 里塞东西——
        # 尤其别塞中间证书：它们轮换很勤，钉住只会让 bundle 隔三差五就过期。
        issuer_key = bundle_by_subject.get(issuer)
        if issuer_key is None:
            reason = "issuer 不在库中（多半是已被移除的老根）"
        else:
            bits = rsa_bits(issuer_key)
            if bits >= EXPENSIVE_RSA_BITS:
                reason = f"验签太贵：issuer 是 RSA-{bits}，设备握手期分配不出来"
            else:
                print(f"    - {host:45s} 老路径够用（issuer 在库中，验签不贵）")
                continue

        # ⚠️ 安全边界：只有能被**现有受信任根**验过的链尾才允许并入。
        # -partial_chain 让 openssl 接受"以某张中间/交叉证书为终点"的验证。
        r = subprocess.run(
            ["openssl", "verify", "-partial_chain", "-CAfile", base_pem, tail],
            capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(
                f"✗ {host} 的链尾无法用系统根证书验证，拒绝并入：\n"
                f"  {r.stdout.strip()} {r.stderr.strip()}\n"
                f"  这要么是主机换了 CA、要么是有人在中间。别绕过这一步。")

        cn = subprocess.run(["openssl", "x509", "-in", tail, "-noout", "-subject"],
                            capture_output=True, text=True).stdout.strip()
        print(f"    + {host:45s} {reason}")
        print(f"      {cn}  -> 本机验过，并入")
        extra.append((host, tail, cn, reason))
        seen_keys.add(key)

    if args.dry_run:
        print(f"\n[dry-run] 会并入 {len(extra)} 张，未写文件")
        return

    print(f"[3/4] 合并证书源（系统库 + {len(extra)} 张已验证的交叉签名根）")
    all_pem = os.path.join(tmp, "all.pem")
    with open(all_pem, "w") as out:
        out.write(open(base_pem).read())
        for host, tail, cn, reason in extra:
            out.write(f"\n# 构建时用系统根证书验过（{host}）：{reason}\n# {cn}\n")
            out.write(open(tail).read())

    print(f"[4/4] 生成 bundle")
    r = subprocess.run([sys.executable, gen, "-i", all_pem], cwd=tmp,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"gen_crt_bundle.py 失败：\n{r.stdout}\n{r.stderr}")
    shutil.copy(os.path.join(tmp, "x509_crt_bundle"), OUT)

    ents = parse_bundle(OUT)
    names = [e[0] for e in ents]
    if names != sorted(names):
        raise SystemExit("✗ bundle 没按名字排序——设备上的二分查找会漏，生成有问题")
    print(f"\n✅ {OUT}")
    print(f"   {len(ents)} 张证书，{os.path.getsize(OUT)} 字节，排序自检通过")


if __name__ == "__main__":
    main()

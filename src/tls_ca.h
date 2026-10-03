// 用根证书 bundle 做真校验，替代满地的 setInsecure()。
//
// 背景：这个项目原来对 HTTPS 有两种做法，都不理想——
//   1. sats / github / adsb 航线 / typhoon 直接 setInsecure()：能挡被动窃听，挡不住中间人
//   2. chat 要求在 secrets.h 里手贴一份 CHAT_ROOT_CA，否则失败关闭。安全，但麻烦，
//      而且 README 早就担心过"根证书会过期，到期后设备就连不上"
//
// bundle 把这两个问题一起解决：121 张 Mozilla 根证书编进 flash（约 56KB），
// 任意公网站点都能验，某一张过期也还有其余的。
// ⚠️ RAM 开销很小——只有索引常驻，证书本体留在 flash 里按需读（Espressif 的实现）。
//
// bundle 文件由 platformio.ini 的 board_build.embed_files 嵌入，符号名是链接器按路径生成的，
// 所以**改了那个路径就要同步改下面的 asm 名字**。
#pragma once
#include <WiFiClientSecure.h>

// 给这个连接挂上 bundle。之后 connect() 会真的校验服务端证书链。
void tlsUseCaBundle(WiFiClientSecure& client);

// ⚠️ 校验证书链要比对有效期，所以**必须先对上时间**。开机后 NTP 是后台异步跑的，
// 在那之前系统时钟停在 1970，任何证书都会被判成"还没生效"，握手必然失败。
// 换 bundle 之前不存在这个问题（setInsecure 根本不看时间），所以这是新引入的失败模式——
// 调用方在发请求前先问一句，能把"时钟还没对上"和"网络/证书真有问题"分开报。
bool tlsClockReady();

// 开机把 mbedTLS 硬件加速那几样"第一次用才建、永不释放"的东西提前摸一遍（见 tls_ca.cpp）。
// setup() 里、任何 CanvasLease 之前调一次。
void tlsCryptoWarmup();

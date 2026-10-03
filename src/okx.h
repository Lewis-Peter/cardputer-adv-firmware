// OKX 的 USDT → 人民币实时汇率（双源高可用：OKX 实时 C2C + CoinGecko 自动 Fallback）。
//
// 1. 主源：OKX C2C 实时行情接口（免 key、免签名）
//     https://www.okx.com/v3/c2c/otc-ticker/quotedPrice?side=buy&quoteCurrency=CNY&baseCurrency=USDT
//     -> {"code":0,"data":[{"bestOption":true,"price":"7.28",...}]}
//
// 2. 备选源：CoinGecko USDT/CNY 接口（国内免翻墙、极简轻量、稳定可靠）
//     https://api.coingecko.com/api/v3/simple/price?ids=tether&vs_currencies=cny
//     -> {"tether":{"cny":7.27}}
//
// ⚠️ 行为策略：
//   优先拉取 OKX 实时 C2C 买入行情（带 OTC 实时溢价），若遇超时、GFW 封锁或解析失败，
//   自动无缝降级拉取 CoinGecko 汇率，并在页面顶部/底部明确标注当前数据源，杜绝死锁与空页面。
#pragma once
#include "globals.h"

void okxEnter();
void okxUpdate();
void okxKey(char k);
void drawOkx();

// 拼出请求 URL。串口的 OKXDUMP / OKXDUMP FB 要打对应的 URL。
void okxBuildUrl(char* out, size_t n);
void okxBuildFallbackUrl(char* out, size_t n);


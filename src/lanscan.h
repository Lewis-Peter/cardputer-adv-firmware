// 局域网设备扫描：连上 WiFi 后对当前子网逐个探测，找到的设备显示 IP + 名字。
//
// 名字的来源按优先级：
//  1. **反向 DNS(PTR)**——问 DHCP 发下来的 DNS 服务器（家用环境基本就是路由器）要
//     w.z.y.x.in-addr.arpa 的 PTR 记录。dnsmasq/ASUS/OpenWrt 都会给 DHCP 客户端登记，
//     所以能拿到 "iPhone" / "Mac" / "RT-AX86U-F738" 这种真名字。Arduino 层只有正向的
//     hostByName()，没有 PTR 接口，所以查询包是自己拼的裸 UDP。
//  2. 查不到 PTR 就退回 MAC 前三字节(OUI)查一份内置的常见厂商表——那只是"猜厂商"，
//     不是名字，查不到就是 Unknown。
//  3. 连 MAC 都没有（ARP 缓存里被挤掉了）就显示 "?"。
// 本机固定作为第一条列出（标 *），名字取 WiFi.getHostname()——它 ping 不到自己、
// ARP 缓存里也永远没有自己，走正常流程反而是唯一扫不到的设备。
//
// 在线判定：ping 通**或**回了 ARP 都算。手机/手表普遍带防火墙不回 ICMP，但 ARP 是
// 链路层的、同网段内没法不回。另外每轮会提前给下一个 IP 发 ARP 请求预热缓存，
// 否则 lwIP 秒级的 ARP 重试会让 250ms 的 ping 超时先到期，把回 ping 的设备也整片漏掉。
#pragma once
#include "globals.h"

void lanscanEnter();
void lanscanExit();      // 离开时停掉还在跑的ping、按需关WiFi（跟其它WiFi工具一样收尾）
void lanscanUpdate();    // 每帧调用：推进异步ping扫描的状态机
void lanscanKey(char k);
void drawLanscan();

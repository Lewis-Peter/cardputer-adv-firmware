// 蓝牙（ESP32-S3 只有 BLE，没有经典蓝牙音频那套 A2DP）。
// 两个功能：
//   1) BLE 扫描器——列出附近广播中的 BLE 设备（名称/RSSI）
//   2) BLE HID 键盘——把设备伪装成一个 BLE 键盘，配对后 Cardputer 打的字直接发给电脑/手机
// BLE 协议栈只在第一次进任意蓝牙子屏时才初始化（懒加载），不影响没用到蓝牙时的功耗/内存。
#pragma once
#include "globals.h"
#include "odid.h"

// ---- 子菜单：Scan devices / Keyboard mode / Media remote ----

// ---- BLE 扫描器 ----
struct BtDevice {
  String name;
  String addr;
  int rssi;
  uint8_t addrType;                    // esp_ble_addr_type_t：0=public 1=random ...
  bool haveTXPower;   int8_t txPower;
  bool haveAppearance; uint16_t appearance;
  bool haveService;   String serviceUUID;
  bool haveManuf;     String manufHex;
  // Service Data（AD type 0x16）跟上面的 Service UUID 是两个不同的字段，很多协议把
  // 载荷放在这里。认 Remote ID（OpenDroneID）就靠它：UUID 0xFFFA + 首字节 0x0D。
  bool haveSvcData;   String svcDataUUID; String svcDataHex;
  // 解出来的 Remote ID。BLE 是 ASTM F3411 三条传输路之一，而且实测比 Wi-Fi 常见——
  // 载荷就在上面这个 Service Data 里（UUID 0xFFFA、首字节 0x0D 应用码）。
  bool haveOdid;      OdidResult odid;
};
extern const int BT_SCAN_MAX;
extern const int BT_SCAN_VIS;
extern BtDevice* btDevList;
extern int btDevCount;
extern int btDevIdx, btDevTop;
void btScan();          // 阻塞扫描 ~3s
void drawBtScan();
void drawBtDeviceDetail();   // 扫描列表里按 Enter 看到的单个设备详情

// ---- BLE HID 键盘 ----
void btKeyboardStart();               // 开始广播，等待主机连接
void btKeyboardStop();                // 停止广播（不销毁服务，下次进来复用）
void btKeyboardSendKey(char k);       // 转发一次按键（按下+松开）给已连接的主机
// 键盘页专用：带修饰键转发一个键（kbd::readKey() 的返回码 + kbd::modMask() 的掩码），
// 顺带更新屏幕上的回显/计数。Ducky 那种脚本回放不走这条，它自己拼 modifier。
void btKeyboardType(char k, uint8_t mods);
void btKeyboardResetEcho();           // 进键盘页时清空回显和计数
// IME 模式：主机上开着中文输入法时，回显框没法再假装知道主机那一行是什么（拼音 vs 汉字，
// 退格删的是候选缓冲区），开了它回显就退化成老实的"本机键流"。调用方还要据此关掉长按连发。
bool btKeyboardImeMode();
void btKeyboardToggleIme();
bool btIsConnected();                 // 当前是否已有主机连上（给 Ducky 判断能不能开跑）
// 原始 HID modifier+usage（给 Ducky 那种组合键/方向键/ESC 用，ASCII 表达不了的走这条）
void btKeyboardSendRaw(uint8_t modifier, uint8_t usage);
// 单个 ASCII 字符对应的 HID usage（不含 modifier）——Ducky 拼 CTRL/ALT/GUI+字母组合键时要用
uint8_t btKeyUsageFor(char c);
void drawBtKeyboard();

// ---- 媒体遥控（HID Consumer Control）----
// 跟键盘共用同一个 BLEHIDDevice / 同一次配对，只是多挂了一个 report ID。
enum BtMediaKey { BT_MEDIA_NEXT, BT_MEDIA_PREV, BT_MEDIA_STOP, BT_MEDIA_PLAYPAUSE,
                  BT_MEDIA_MUTE, BT_MEDIA_VOL_UP, BT_MEDIA_VOL_DOWN };
void btMediaStart();                  // 开始广播（跟键盘模式是同一套服务）
void btMediaSend(BtMediaKey k);
void drawBtMedia();

// ---- 找物雷达：持续扫描单个设备的 RSSI，画曲线 + 越近响得越急 ----
// ⚠️ 按 MAC 地址跟踪。用随机地址（RPA）的设备（AirPods、各家防丢标签、多数手机）
// 每十几分钟会换一次地址，换掉之后这里就跟丢了——重新扫一遍选新地址即可。
void btRadarStart(int devIndex);      // 用 btDevList[devIndex] 当目标
void btRadarStop();
void btRadarUpdate();                 // 每帧调：采样进历史曲线 + 驱动蜂鸣
void btRadarToggleBeep();
void drawBtRadar();

// 退出蓝牙功能：彻底关掉 BLE 控制器并释放。离开所有蓝牙子屏回主菜单时调用——
// 否则 BLE 栈会常驻到重启，还会挡着之后要开 WiFi 的 app（Wi-Fi/BT 共存容易崩重启）。
void btExit();

// 离开蓝牙进了别的 app 时调（loop 里判断）。btExit 只是把 BLE 挂起——对象全留着，
// 再进蓝牙零开销、零泄漏；但挂起状态下内存不够 WiFi 用，所以真去用别的 app 时
// 必须在这儿彻底释放。这一步会付一次库泄漏，因此故意拖到最后一刻才做。
void btReleaseForOtherApps();

// BLE 协议栈现在还占着堆吗（init 过、且还没 deinit——挂起状态也算，disable 不还内存）。
// 给 Wi-Fi 看门狗用：这时候去拉 STA 是注定失败的，见 main.cpp 那段实测。
bool btHoldsHeap();

// 内存余量提示。每次蓝牙↔其它 app 来回都会永久漏掉一套 GATT 对象树（库没有拆除路径），
// 漏到一定程度 BLEDevice::deinit() 会永久阻塞、整机卡死。修库的尝试见分支
// wip/ble-destructor-leak-fix（内存能修好，但 deinit 的阻塞没解决，故未合入）。
// 这几个函数用来在蓝牙菜单上提前警告，别让人在卡死时莫名其妙。
int  btAlternations();
bool btMemoryLow();
bool btMemoryCritical();

// BLE 取样（串口 BTDUMP 指令）：扫一遍把每个设备的 Service Data / Manufacturer Data
// 原样打到串口。屏幕一次只看得到一个设备，横扫一片时用这个。
void btDumpRun(int seconds);

// BLE 扩展广播取样（串口 BTEXT 指令）：Arduino 的 BLEScan 只扫 legacy/1M PHY，
// 而 ASTM F3411 的 Bluetooth 那条走 BT5 Long Range + 扩展广播（Coded PHY）。
// 这条直接调 Bluedroid 的 ext scan，1M 和 Coded 一起收。
void btExtScanRun(int seconds);

// ---- PC 主导模式 BLE 流（BLE ON / BLE OFF，协议 §8.6）----
void btStreamStart();                 // 冲突/内存不足时回 BLE {"t":"err"} 并返回
void btStreamStop();                  // 幂等，总是回 BLE {"t":"end"}
bool btStreamIsActive();
void btStreamPump();                  // 主循环每帧调：从队列取报告、限频、出 BLE 行
int  btStreamDevCount();
uint32_t btStreamPktCount();

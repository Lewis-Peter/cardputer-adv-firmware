#pragma once
#include <stdint.h>
// ---------------------------------------------------------------------------
// OpenDroneID / ASTM F3411 Remote ID 解码器
//
// 纯字节运算，不碰任何硬件，所以 Wi-Fi(vendor IE / NAN action) 和 BLE(Service Data)
// 两条传输路径能共用同一份——它们载的本来就是同一套 25 字节消息。
//
// 结构（小端）：
//   每条消息 25 字节，byte0 高 4 位是消息类型、低 4 位是协议版本。
//   类型 0xF 是 Message Pack：byte1=单条长度(25)、byte2=条数、byte3 起是各条消息。
//
// ⚠️ 载荷第一个字节是**消息计数器**，真正的消息从第 2 字节才开始——beacon 和 NAN
//    两条路都有这一字节（对着 sxjack/unix_rid_capture 的 parse_odid() 核过：它
//    `counter = payload[0]; index = payload[1] >> 4;`，所有解码器都传 &payload[1]）。
//    但真机上偶尔能见到不带计数字节的实现，所以 odidDecode() 仍然把 offset 1 和 0
//    各试一遍、按解出来的内容打分取优，不去赌发送方守不守规矩。
//
// 参考：ASTM F3411 / opendroneid-core-c 的 ODID_*_encoded 位域定义。
// ---------------------------------------------------------------------------

static const uint8_t ODID_OUI_0 = 0xFA, ODID_OUI_1 = 0x0B, ODID_OUI_2 = 0xBC;
static const uint8_t ODID_OUI_TYPE = 0x0D;   // vendor IE 里 OUI 后面那一字节必须是它

// ⚠️ 经纬度一律是 **WGS-84**（ASTM 和国标都这么规定，国标载荷里还有个"坐标系类型"
// 字段可以复核，实测就是 0=WGS-84）。要画到本项目的 GNSS 地图页（高德卫星影像，
// GCJ-02 火星坐标）上，**必须先调 gnss.h 的 wgs2gcj()**，否则会稳定偏出几百米。
struct OdidResult {
  bool haveBasic = false;
  char uasId[21] = {0};      // 机身编号（20 字节 ASCII）
  uint8_t idType = 0;        // 0 None / 1 序列号 / 2 CAA / 3 UTM / 4 Session
  uint8_t uaType = 0;        // 机型（固定翼/多旋翼/…）

  bool haveLoc = false;
  double lat = 0, lon = 0;
  float altGeo = 0;          // 几何高（拿不到就退气压高），米
  bool  haveAlt = false;
  float height = 0;          // 相对起飞点/地面的高度，米——看无人机时这个比海拔有用得多
  bool  haveHeight = false;
  bool  heightIsAgl = false; // ASTM byte1 bit2：0=相对起飞点 1=真AGL。国标这个字段定义就是相对起飞点，不用这个标志（留 false）
  float speed = 0;           // 水平速度 m/s
  bool  haveSpeed = false;
  float vspeed = 0;          // 垂直速度 m/s，正=上升
  int   heading = -1;        // 航向 0~359，-1 = 无效

  bool haveSys = false;
  double pilotLat = 0, pilotLon = 0;   // 飞手/起飞点位置
  uint8_t pilotLocType = 0;  // 0 未知 / 1 实时GNSS / 2 起飞点 / 3 固定

  bool haveSelfId = false;
  char selfId[24] = {0};     // 运行描述（23 字节 ASCII），飞手自己填的一句话
  uint8_t selfIdType = 0;

  bool haveOperatorId = false;
  char operatorId[21] = {0}; // 运营人编号 / 国标的实名登记号（20 字节 ASCII）

  // 运行状态。**两套标准共用这一个字段**：ASTM 的 ODID_status_t 和国标的"运行状态"
  // 取值恰好同义（0 未申报 / 1 地面 / 2 空中 / 3 紧急 / 4 RID 失效），所以显示代码
  // 不用分两套。ASTM 在 Location 消息 byte1 的高 4 位，国标在数据项 015。
  bool haveStatus = false;
  uint8_t opStatus = 0;      // 0未报告/1地面/2空中/3紧急/4-5识别失效

  // 数据新鲜度。两套标准的时间基准不一样，所以**不能合并成一个字段**：
  //   ASTM  Location[21..22]，整点后的十分之一秒，0xFFFF 无效 —— 只有"整点后多少秒"，
  //         要判新旧得拿本机时钟的分秒去比（跨整点时会绕回 0，比的时候留意）。
  //   国标  数据项 020，Unix 毫秒绝对时间，信息量更大。
  bool haveAstmTime = false;
  float astmTimeSec = 0;     // 整点后秒数 0~3600

  // ---- 下面几项只有国标(GB)那条路会填 ----
  bool isGb = false;         // true = 由国标解析器解出，false = 标准 ASTM ODID
  uint8_t gbVersion = 0;     // 国标版本号（如 0x20 = V2.0）
  bool haveCoordSys = false;
  uint8_t coordSys = 0;      // 坐标系 0=WGS-84 / 1=CGCS2000 / 2=GLONASS-PZ90
  bool haveUaClass = false;
  uint8_t uaClass = 0;       // 国标分类 0微型/1轻型/2小型/3其他/4中型/5大型
  bool haveGbTime = false;
  uint64_t gbTimeMs = 0;     // 数据项 020：Unix 毫秒，48 位小端。用来判数据新鲜度
  uint8_t gbTimeAcc = 0;     // 数据项 021：时间戳精度等级 0~5
};

// 把一次新解出的结果并进累积结果。
//
// ⚠️ 必须合并，不能覆盖：不打包的发送方是**轮流**播 Basic ID / Location / System 的，
// 一包只带一种（BLE 尤其如此，一条广播就 25 字节，装不下 Pack）。直接覆盖的话永远
// 只看得到最后收到的那一种，编号和坐标凑不齐。
// 位置类字段取最新（飞行中一直在变），身份类字段保留首次解出的。
void odidMerge(OdidResult& dst, const OdidResult& src);

// 从一整帧 802.11 公共动作帧里找出 NAN 承载的 ODID 载荷。
// 返回载荷偏移（含计数字节），找不到返回 -1；outLen 给出剩余长度。
//
// ⚠️ 这个偏移不是"OUI 后面接着就是"。NAN 的 Service Discovery Frame 在 OUI 之后还有
// 一整串 NAN 属性，ODID 载荷藏在 Service Descriptor Attribute 的 service info 里，
// 从帧头算起要跳到第 43 字节。照着 OUI+4 去解只会解到 NAN 属性头上，一条都解不出来。
int odidFindNanPayload(const uint8_t* frame, int len, int& outLen);

// payload = vendor IE 里 OUI+type 之后的全部字节（或 BLE Service Data 里首字节之后）。
// 解出任意一种消息就返回 true。
bool odidDecode(const uint8_t* payload, int len, OdidResult& out);

// 国标(GB)《民用无人驾驶航空器系统运行识别规范》的载荷解码。
//
// 跟 ASTM 那套 25 字节定长消息完全不是一回事：它是**位图驱动的变长包**——
//   [数据类型=0xFF][版本][数据长度][数据标识位图 3+N 字节][数据内容项...]
// 位图每一位对应一个数据项，置 1 才发送；内容项按位图顺序紧密排列，长度各不相同。
// 所以**偏移是算出来的，不是查表查出来的**：换一台只发必选项的设备，所有偏移都会变。
// （之前照抄别人的固定偏移表解不出来，根因就在这里。）
bool gbDecode(const uint8_t* payload, int len, OdidResult& out);

// DJI 私有 DroneID (OUI 26:37:12) 载荷解码（遥测/机身编号）
bool djiDecode(const uint8_t* payload, int len, OdidResult& out);

// 统一入口：先按国标试，不是再按 ASTM 试。四条传输路径（Wi-Fi beacon / Wi-Fi NAN /
// BLE 广播 / BLE 扩展广播）都用它，省得每处各判一次是哪种标准。
bool ridDecode(const uint8_t* payload, int len, OdidResult& out);

// 从一整帧 802.11 管理帧里解出 Remote ID：beacon(8)/probe-resp(5) 走 vendor IE(221)，
// action(13) 走 NAN。纯字节运算，所以串口取样、扫描 app、测试台可以共用同一份。
// outIsRidCandidate 可选输出：若检测到 SSID 以 "RID-" 开头，置 true（供信道快速预锁频使用）。
bool ridFromMgmtFrame(const uint8_t* fr, int len, OdidResult& out, bool* outIsRidCandidate = nullptr);

// 依据机身编号前缀识别厂商名称（DJI / Autel / EHang / FIMI / XAG / Skydio 等）
const char* odidVendorName(const char* uasId);

const char* odidIdTypeName(uint8_t t);
const char* odidUaTypeName(uint8_t t);
const char* odidPilotLocName(uint8_t t);
const char* gbStatusName(uint8_t t);
const char* gbCoordSysName(uint8_t t);
const char* gbClassName(uint8_t t);

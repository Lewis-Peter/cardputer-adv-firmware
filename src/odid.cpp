#include "odid.h"
#include <cstring>
#include <cmath>

// 25 字节定长。Pack 里每一条也是这个长度。
static const int MSG_SIZE = 25;
static const uint8_t MSG_BASIC_ID   = 0x0;
static const uint8_t MSG_LOCATION   = 0x1;
static const uint8_t MSG_SELF_ID    = 0x3;
static const uint8_t MSG_SYSTEM     = 0x4;
static const uint8_t MSG_OPERATOR_ID = 0x5;
static const uint8_t MSG_PACK       = 0xF;
// 规范里 Pack 最多 9 条（ODID_PACK_MAX_MESSAGES）。卡死这个上限不只是照本宣科——
// odidDecode 要在两个偏移之间打分选一个，条数放宽会让随机字节更容易冒充成 Pack。
static const int PACK_MAX_MSGS = 9;

static inline int32_t  rd32(const uint8_t* p) {
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// 经纬度哨兵值。设备没定位时会播这几个值，直接当有效数据用会在地图上画出鬼点。
static bool coordRawBad(int32_t v) {
  return v == -1 || v == 0x7FFFFFFF || v == (int32_t)0x80000000;
}
// Null Island（0,0）附近整片丢掉：真机没定位时经常输出接近 0 的值，
// 而赤道几内亚外海那片海面上不会有无人机在飞。
static bool coordPairOk(double lat, double lon) {
  if (!std::isfinite(lat) || !std::isfinite(lon)) return false;
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
  if (lat > -5 && lat < 5 && lon > -5 && lon < 5) return false;
  return true;
}

// 从消息里抠一段定长 ASCII 字段（编号/描述都是这个套路：尾部补 0，只收可打印字符）。
// 全空/全乱的一律拒掉——没播这个字段的机器留下的是一片 00 或 FF，当成内容会往列表里
// 塞一行垃圾，而 odidDecode 还要靠"解出了什么"来给偏移打分，噪声会直接带偏判断。
static bool asciiField(const uint8_t* src, int cap, char* dst, int minLen) {
  int n = 0;
  for (int i = 0; i < cap; i++) {
    uint8_t c = src[i];
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) return false;
    dst[n++] = (char)c;
  }
  dst[n] = 0;
  return n >= minLen;
}

static bool decodeBasicId(const uint8_t* m, OdidResult& out) {
  // byte1 是 [IDType(高4位)][UAType(低4位)]（位域是 LSb-first 定义的，所以高位那个是 IDType）
  out.idType = (uint8_t)(m[1] >> 4);
  out.uaType = (uint8_t)(m[1] & 0x0F);
  if (!asciiField(m + 2, 20, out.uasId, 4)) return false;
  out.haveBasic = true;
  return true;
}

static bool decodeLocation(const uint8_t* m, OdidResult& out) {
  int32_t latRaw = rd32(m + 5), lonRaw = rd32(m + 9);
  if (coordRawBad(latRaw) || coordRawBad(lonRaw)) return false;
  double lat = latRaw * 1e-7, lon = lonRaw * 1e-7;
  if (!coordPairOk(lat, lon)) return false;
  out.lat = lat; out.lon = lon;

  // byte1: bit0=速度倍率 bit1=东西向 bit2=高度类型 bit4~7=飞行状态
  const uint8_t b1 = m[1];

  // 航向：byte2 存 0~179，加上 bit1 的东西向标志才凑出整圈 0~359。
  // （上游那个 Python 实现直接把航向丢了，这里补上——判断无人机往哪飞比速度更有用）
  if (m[2] <= 179) out.heading = m[2] + ((b1 & 0x02) ? 180 : 0);

  // 水平速度是分段编码的：低速段 0.25 m/s 一格，高速段换成 0.75 一格并抬一个基准
  uint8_t sEnc = m[3];
  float sp = (b1 & 0x01) ? (sEnc * 0.75f + 255.0f * 0.25f) : (sEnc * 0.25f);
  if (sp < 255.0f) { out.speed = sp; out.haveSpeed = true; }

  out.vspeed = (float)((int8_t)m[4]) * 0.5f;

  // byte1 高 4 位是运行状态，跟国标数据项 015 同义，共用 opStatus。
  // 跟国标那边一样**不拿它当"解出来了"的证据**——这个函数靠经纬度过关，状态只是搭车。
  if ((b1 >> 4) <= 5) { out.opStatus = (uint8_t)(b1 >> 4); out.haveStatus = true; }

  // 高度：uint16 半米一格、偏移 -1000。等于 -1000 是"没有这个值"的哨兵。
  float altBaro = rd16(m + 13) * 0.5f - 1000.0f;
  float altGeo  = rd16(m + 15) * 0.5f - 1000.0f;
  if (altGeo > -999.9f)       { out.altGeo = altGeo;  out.haveAlt = true; }
  else if (altBaro > -999.9f) { out.altGeo = altBaro; out.haveAlt = true; }

  // 相对起飞点的高度，同样的编码。抬头找无人机时这个数比海拔直观得多
  float h = rd16(m + 17) * 0.5f - 1000.0f;
  if (h > -999.9f) {
    out.height = h; out.haveHeight = true;
    out.heightIsAgl = (b1 & 0x04) != 0;   // bit2：0=相对起飞点 1=真AGL
    // 依据：ASTM F3411-22a Location/Vector 报文 byte1 = [7..4]Status [3]Reserved [2]HeightType
    //       [1]E/W方向 [0]速度倍率；HeightType 0=Takeoff 1=Ground(AGL)。与 opendroneid-core-c
    //       opendroneid.h 中 ODID_Location_encoded 的位域顺序（SpeedMult:1, EWDirection:1,
    //       HeightType:1, Reserved:1, Status:4，LSB 起）及 ODID_HEIGHT_REF_OVER_TAKEOFF=0 /
    //       ODID_HEIGHT_REF_OVER_GROUND=1 一致。国标 011 字段本身就是"相对起飞地"，不走这里
  }

  // byte21~22：整点后的十分之一秒。0xFFFF 是"没有这个值"，另外超过 36000（整点后
  // 3600 秒）的也只能是垃圾——不设上界的话跨帧比对会算出负的"数据年龄"。
  uint16_t ts = rd16(m + 21);
  if (ts != 0xFFFF && ts <= 36000) { out.astmTimeSec = ts * 0.1f; out.haveAstmTime = true; }

  out.haveLoc = true;
  return true;
}

static bool decodeSelfId(const uint8_t* m, OdidResult& out) {
  out.selfIdType = m[1];
  if (!asciiField(m + 2, 23, out.selfId, 1)) return false;
  out.haveSelfId = true;
  return true;
}

static bool decodeOperatorId(const uint8_t* m, OdidResult& out) {
  if (!asciiField(m + 2, 20, out.operatorId, 4)) return false;
  out.haveOperatorId = true;
  return true;
}

static bool decodeSystem(const uint8_t* m, OdidResult& out) {
  int32_t latRaw = rd32(m + 2), lonRaw = rd32(m + 6);
  if (coordRawBad(latRaw) || coordRawBad(lonRaw)) return false;
  double lat = latRaw * 1e-7, lon = lonRaw * 1e-7;
  if (!coordPairOk(lat, lon)) return false;
  out.pilotLat = lat; out.pilotLon = lon;
  out.pilotLocType = (uint8_t)(m[1] & 0x03);   // bit0~1，bit2~4 是机器分类
  out.haveSys = true;
  return true;
}

// 解一段"以某条消息开头"的载荷。Pack 就把里面每条都过一遍。
static bool decodeAt(const uint8_t* p, int len, OdidResult& out) {
  if (len < 2) return false;
  uint8_t type = (uint8_t)(p[0] >> 4);

  if (type == MSG_PACK) {
    // byte1 必须是单条长度 25，byte2 是条数——这两个约束顺便当成了 Pack 的校验
    if (len < 3 || p[1] != MSG_SIZE) return false;
    int qty = p[2];
    if (qty < 1 || qty > PACK_MAX_MSGS) return false;
    if (3 + qty * MSG_SIZE > len) return false;
    bool any = false;
    for (int i = 0; i < qty; i++) {
      const uint8_t* m = p + 3 + i * MSG_SIZE;
      switch (m[0] >> 4) {
        case MSG_BASIC_ID:   if (!out.haveBasic)      any |= decodeBasicId(m, out); break;
        case MSG_LOCATION:   if (!out.haveLoc)        any |= decodeLocation(m, out); break;
        case MSG_SELF_ID:    if (!out.haveSelfId)     any |= decodeSelfId(m, out); break;
        case MSG_SYSTEM:     if (!out.haveSys)        any |= decodeSystem(m, out); break;
        case MSG_OPERATOR_ID:if (!out.haveOperatorId) any |= decodeOperatorId(m, out); break;
        default: break;   // 类型 2 是 Auth，分页签名，看无人机用不上
      }
    }
    return any;
  }

  if (len < MSG_SIZE) return false;
  switch (type) {
    case MSG_BASIC_ID:    return decodeBasicId(p, out);
    case MSG_LOCATION:    return decodeLocation(p, out);
    case MSG_SELF_ID:     return decodeSelfId(p, out);
    case MSG_SYSTEM:      return decodeSystem(p, out);
    case MSG_OPERATOR_ID: return decodeOperatorId(p, out);
    default: return false;
  }
}

// 解出来的东西越硬，分越高。用来在"跳过计数字节"和"不跳"之间选一个。
static int score(const OdidResult& r) {
  int s = 0;
  if (r.haveBasic)      s += 2;
  if (r.haveLoc)        s += 3;   // 经纬度最难碰巧解对，权重给最高
  if (r.haveSys)        s += 2;
  if (r.haveSelfId)     s += 1;   // 纯 ASCII，碰巧成立的概率比坐标高，权重压低
  if (r.haveOperatorId) s += 1;
  return s;
}

// 国标(GB)报文特征检查：数据类型恒为 0xFF，后接版本号及长度。
// 实测：GB 载荷若被标准 ODID 解码器当成单条 Location 误解析（当计数字节落在 0x10~0x1F 时
// 会被当成 MSG_LOCATION，将序列号 ASCII 码误认为经纬度），会产生北纬 82+ 度、离地 7 公里、
// 117 m/s 的假坐标。因此必须在 ASTM 解码前识别并拦截，交给 GB 解析器处理。
static bool isGbPayload(const uint8_t* p, int len) {
  if (!p) return false;
  for (int off = 0; off <= 1 && off + 4 <= len; off++) {
    if (p[off] == 0xFF) {
      uint8_t ver = p[off + 1];
      uint8_t dlen = p[off + 2];
      // 版本号只认实测过的 0x20——tools/odidtest/samples_gb.md 里记录过这个字节跟
      // 征求意见稿写的"1=V1.0"对不上，但没有第二台设备验证过别的版本号实际发的是什么，
      // 猜一个塞进来风险是跟 ASTM 报文的 type/version 字节撞车（撞上了就会误吞真实的
      // ASTM 帧）。见到不同版本号的真机再按实测加。
      if (ver == 0x20 && dlen >= 1 && dlen <= 200) {
        return true;
      }
    }
  }
  return false;
}

bool odidDecode(const uint8_t* payload, int len, OdidResult& out) {
  if (!payload || len < 3) return false;
  if (isGbPayload(payload, len)) return false;

  // beacon 的 vendor IE 在消息前面多一字节消息计数器，NAN action 帧没有。
  // 与其去猜这一包是从哪条路来的，不如两种都解一遍，谁解出的东西多就用谁。
  OdidResult a, b;
  bool okA = decodeAt(payload + 1, len - 1, a);   // 跳过计数字节（beacon 的常态）
  bool okB = decodeAt(payload, len, b);           // 不跳

  if (okA && (!okB || score(a) >= score(b))) { out = a; return true; }
  if (okB) { out = b; return true; }
  return false;
}

// =============================================================================
// 国标(GB)《民用无人驾驶航空器系统运行识别规范》
// =============================================================================
// 21 个数据项的字节长度，下标 = 标准里的数据项序号。位图里某一位置 1，内容区就
// 按这张表的长度往前推进一格。⚠️ 这是唯一的长度真相来源，改动前先对标准原文。
static const uint8_t GB_LEN[22] = {
  0,        // 占位，序号从 1 开始
  20,       // 001 唯一产品识别码
  8,        // 002 实名登记信息
  1,        // 003 运行类别
  1,        // 004 无人驾驶航空器分类
  1,        // 005 遥控站位置类型
  8,        // 006 遥控站位置（32位经度 + 32位纬度）
  2,        // 007 遥控站高度
  8,        // 008 无人驾驶航空器位置
  2,        // 009 航迹角
  2,        // 010 地速
  2,        // 011 相对高度
  1,        // 012 垂直速度
  2,        // 013 大地高度
  2,        // 014 气压高度
  1,        // 015 运行状态
  1,        // 016 坐标系类型
  1,        // 017 水平精度
  1,        // 018 垂直精度
  1,        // 019 速度精度
  6,        // 020 时间戳 —— **6 字节不是 4**，Unix 毫秒 48 位小端。
            // 曾经写 4，症状是"末尾多出 2 字节没归属"+"时间戳精度 159 超出 0~5"。
            // 三条证据咬合见 tools/odidtest/samples_gb.md。
  1,        // 021 时间戳精度
};

// 高度统一编码：编码值 =（实际值×2＋2000），分辨率 0.5 m。0 视为无效。
static bool gbAlt(uint16_t enc, float& out) {
  if (enc == 0 || enc == 0xFFFF) return false;
  out = (enc - 2000) / 2.0f;
  return true;
}

static bool gbDecodeAt(const uint8_t* p, int len, OdidResult& out) {
  // [0]数据类型 [1]版本 [2]数据长度 [3..]数据标识位图
  if (len < 5 || p[0] != 0xFF) return false;
  out.gbVersion = p[1];
  const int dataLen = p[2];
  if (dataLen < 1 || dataLen > 200) return false;

  // 位图：每字节最低位是扩展标志，1=下一字节还是位图
  uint8_t bits[8];
  int nbits = 0;
  int i = 3;
  while (i < len && nbits < (int)sizeof(bits)) {
    bits[nbits++] = p[i];
    if (!(p[i] & 0x01)) { i++; break; }   // 扩展位为 0 ⇒ 位图到此为止
    i++;
  }
  if (nbits == 0 || (bits[nbits - 1] & 0x01)) return false;   // 位图没正常收尾

  // 整包长度自校验：3(头) + 位图 + 数据长度 必须装得下。允许末尾有多余字节
  // （实测那台机器的数据内容里就有 2 字节没归属到任何数据项），但装不下一定是
  // 认错了包。这一条是区分"真国标包"和"碰巧 0xFF 开头的垃圾"的主要依据之一。
  const int content = i;
  if (content + dataLen > len) return false;

  int c = content;
  const int end = content + dataLen;
  bool any = false;

  for (int b = 0; b < nbits; b++) {
    for (int bit = 7; bit >= 1; bit--) {          // bit0 是扩展标志，不是数据项
      if (!(bits[b] & (1 << bit))) continue;
      const int item = b * 7 + (8 - bit);         // 每字节承载 7 个数据项
      // 位图里出现我们不认识的数据项：不知道它多长，后面的偏移就全废了，只能停。
      // 但**不要整包丢弃**——前面已经解出来的编号/位置是好的。标准明说版本升级会
      // 扩展数据项，将来的固件一定会走到这里，整包丢等于一升级就全瞎。
      if (item < 1 || item > 21) { b = nbits; break; }
      const int L = GB_LEN[item];
      if (c + L > end) return false;              // 内容区放不下 ⇒ 这包不合法
      const uint8_t* v = p + c;

      switch (item) {
        case 1:                                    // 唯一产品识别码
          if (asciiField(v, 20, out.uasId, 4)) { out.haveBasic = true; any = true; }
          break;
        case 2:                                    // 实名登记信息（登记号后 8 位）
          if (asciiField(v, 8, out.operatorId, 4)) { out.haveOperatorId = true; any = true; }
          break;
        case 4:
          out.uaClass = v[0]; out.haveUaClass = true;
          break;
        case 5:
          out.pilotLocType = v[0];
          break;
        case 20: {                                 // 时间戳：Unix 毫秒，48 位小端
          uint64_t ms = 0;
          for (int k = 5; k >= 0; k--) ms = (ms << 8) | v[k];
          // 全 0 = 没授时（真机上电量耗尽那帧就是这样），别拿它当 1970 年
          if (ms > 1000000000000ULL) { out.gbTimeMs = ms; out.haveGbTime = true; any = true; }
          break;
        }
        case 21:
          out.gbTimeAcc = v[0];
          break;
        case 6: {                                  // 遥控站位置：经度在前，纬度在后
          int32_t lon = rd32(v), lat = rd32(v + 4);
          if (!coordRawBad(lat) && !coordRawBad(lon)) {
            double la = lat * 1e-7, lo = lon * 1e-7;
            if (coordPairOk(la, lo)) { out.pilotLat = la; out.pilotLon = lo; out.haveSys = true; any = true; }
          }
          break;
        }
        case 8: {                                  // 无人驾驶航空器位置
          int32_t lon = rd32(v), lat = rd32(v + 4);
          if (!coordRawBad(lat) && !coordRawBad(lon)) {
            double la = lat * 1e-7, lo = lon * 1e-7;
            if (coordPairOk(la, lo)) { out.lat = la; out.lon = lo; out.haveLoc = true; any = true; }
          }
          break;
        }
        case 9: {                                  // 航迹角
          // ⚠️ 草案写的是"0~179 + 一个标志位表示是否≥180°，分辨率 1°"，但实测这版固件
          // 发的是**0.1° 分辨率的整圈值**（0~3599）。2026-08-09 户外实飞取证：
          //   raw 135 -> 13.5°，同一时段相邻两帧的经纬度算出的实际轨迹是 14.3°
          //   raw 1830 -> 183.0°，正是南北往返的回程（跟 13.5° 相差 169.5°≈180°）
          // 按草案解的话 135 以外的值全都 >179 会被判无效，等于航向永远出不来。
          // 0xFFFF = 无效（低速悬停时固件就发这个，实测过）。
          const uint16_t e = rd16(v);
          if (e <= 3599) out.heading = (int)(e / 10);
          break;
        }
        case 10: {                                 // 地速：分段编码，标志位区分快慢档
          uint16_t e = rd16(v);
          uint16_t mag = e & 0x7FFF;
          if (mag <= 255) {
            out.speed = (e & 0x8000) ? (mag * 0.75f + 64.0f) : (mag * 0.25f);
            out.haveSpeed = true;
          }
          break;
        }
        case 11: {                                 // 相对高度（基于起飞地）
          // ⚠️ 大疆这版固件的基准常数是错的：标准写的是（实际值×2＋2000），
          // 它实际发的是（实际值×2＋18000）。两点实测坐实（现场报数 vs 解出值）：
          //     贴地 0.1 m → raw 18000     30 m → raw 18060
          //     (18000-18000)/2 = 0.0      (18060-18000)/2 = 30.0
          // 按标准基准算会得到 8000 m——正好差 (18000-2000)/2。同一个包里的
          // 大地高度(013) 用的却是标准的 2000 并且实测吻合，所以这是 DJI 单独把这个
          // 字段的常数写错了，不是标准有两套。
          // 处理办法：若是 DJI 设备且落在 18000 范围则直接以 18000 为基准；
          // 否则先按标准基准算，算出来离谱就换 DJI 那个基准再试一次。
          const uint16_t enc = rd16(v);
          float h;
          const bool isDji = (strncmp(out.uasId, "1581", 4) == 0 ||
                              strncmp(out.uasId, "0T8", 3) == 0 ||
                              strncmp(out.uasId, "146", 3) == 0);
          if (isDji && enc >= 17000 && enc <= 24000) {
            const float dh = (enc - 18000) / 2.0f;
            if (dh > -500.0f && dh < 3000.0f) { out.height = dh; out.haveHeight = true; }
          } else if (gbAlt(enc, h) && h > -500.0f && h < 3000.0f) {
            out.height = h; out.haveHeight = true;                    // 合规设备
          } else if (enc >= 17000 && enc <= 24000) {                  // 大疆基准 18000 兜底
            const float dh = (enc - 18000) / 2.0f;
            if (dh > -500.0f && dh < 3000.0f) { out.height = dh; out.haveHeight = true; }
          }
          break;
        }
        case 12: {                                 // 垂直速度：最高位 1=上升，0xFF=无效/未知
          uint8_t e = v[0];
          if (e != 0xFF) {
            out.vspeed = ((e & 0x7F) / 2.0f) * ((e & 0x80) ? 1.0f : -1.0f);
          }
          break;
        }
        case 13: {                                 // 大地高度
          float h;
          if (gbAlt(rd16(v), h)) { out.altGeo = h; out.haveAlt = true; }
          break;
        }
        case 14: {                                 // 气压高度，只在没有大地高度时兜底
          float h;
          if (!out.haveAlt && gbAlt(rd16(v), h)) { out.altGeo = h; out.haveAlt = true; }
          break;
        }
        case 15:
          // ⚠️ 这里**不置 any**。运行状态只是一个 0~5 的字节，随机数据 6/256 就能蒙对，
          // 拿它当"解码成功"的证据会凭空造出一台没有编号也没有坐标的幽灵无人机。
          // 只有编号、登记号、经纬度这类强字段才算数。
          if (v[0] <= 5) { out.opStatus = v[0]; out.haveStatus = true; }
          break;
        case 16:
          if (v[0] <= 2) { out.coordSys = v[0]; out.haveCoordSys = true; }
          break;
        default: break;                            // 运行类别/精度/时间戳暂不展示
      }
      c += L;
    }
  }

  if (any) out.isGb = true;
  return any;
}

bool gbDecode(const uint8_t* payload, int len, OdidResult& out) {
  if (!payload || len < 6) return false;
  // 跟 ASTM 那条一样，载荷首字节可能是消息计数器，跳/不跳各试一次
  OdidResult a, b;
  if (gbDecodeAt(payload + 1, len - 1, a)) { out = a; return true; }
  if (gbDecodeAt(payload, len, b))         { out = b; return true; }
  return false;
}

bool ridDecode(const uint8_t* payload, int len, OdidResult& out) {
  if (gbDecode(payload, len, out)) return true;
  return odidDecode(payload, len, out);
}

void odidMerge(OdidResult& dst, const OdidResult& src) {
  if (src.haveBasic) {
    if (!dst.haveBasic) {
      dst.haveBasic = true;
      memcpy(dst.uasId, src.uasId, sizeof(dst.uasId));
    }
    if (src.idType && !dst.idType) dst.idType = src.idType;
    if (src.uaType && !dst.uaType) dst.uaType = src.uaType;
  }
  if (src.haveLoc) {                    // 位置取最新的
    dst.haveLoc = true; dst.lat = src.lat; dst.lon = src.lon;
  }
  if (src.haveAlt) {
    dst.haveAlt = true; dst.altGeo = src.altGeo;
  }
  if (src.haveHeight) {
    dst.haveHeight = true; dst.height = src.height;
    dst.heightIsAgl = src.heightIsAgl;
  }
  if (src.haveSpeed) {
    dst.haveSpeed = true; dst.speed = src.speed;
  }
  if (src.heading >= 0) {
    dst.heading = src.heading;
  }
  if (src.vspeed != 0 || src.haveLoc) {
    dst.vspeed = src.vspeed;
  }
  if (src.haveSys) {
    dst.haveSys = true; dst.pilotLat = src.pilotLat; dst.pilotLon = src.pilotLon;
    dst.pilotLocType = src.pilotLocType;
  }
  if (src.haveSelfId && !dst.haveSelfId) {
    dst.haveSelfId = true; dst.selfIdType = src.selfIdType;
    memcpy(dst.selfId, src.selfId, sizeof(dst.selfId));
  }
  if (src.haveOperatorId && !dst.haveOperatorId) {
    dst.haveOperatorId = true;
    memcpy(dst.operatorId, src.operatorId, sizeof(dst.operatorId));
  }
  if (src.isGb) dst.isGb = true;
  if (src.gbVersion) dst.gbVersion = src.gbVersion;
  if (src.haveStatus)   { dst.haveStatus = true; dst.opStatus = src.opStatus; }   // 状态取最新
  // 时间戳一律取最新——它本来就是用来判"这条记录有多旧"的，保留首次的等于自废武功。
  if (src.haveAstmTime) { dst.haveAstmTime = true; dst.astmTimeSec = src.astmTimeSec; }
  if (src.haveGbTime)   { dst.haveGbTime = true; dst.gbTimeMs = src.gbTimeMs;
                          dst.gbTimeAcc = src.gbTimeAcc; }
  if (src.haveCoordSys) { dst.haveCoordSys = true; dst.coordSys = src.coordSys; }
  if (src.haveUaClass && !dst.haveUaClass) { dst.haveUaClass = true; dst.uaClass = src.uaClass; }
}

// NAN(Wi-Fi Aware) 承载的 Remote ID 在帧里的位置。
//
// ⚠️ 这里踩过一次坑：ODID 载荷**不是**跟在 OUI 后面。NAN 的 Service Discovery Frame
// 在 OUI 之后还有一整串 NAN 属性，载荷藏在 Service Descriptor Attribute 的
// service info 字段里。按 OUI+4 去解会停在属性头上，一条都解不出来。
//
// 帧内布局（偏移从帧头算起，对着 sxjack/unix_rid_capture 的 NAN 分支核过）：
//   [16] addr3(BSSID) = 50:6F:9A:01:00:FF   NAN 的固定 cluster ID
//   [24] 04 09                              公共动作 / vendor specific
//   [26] 50:6F:9A                           Wi-Fi Alliance OUI
//   [29] 13                                 OUI type = NAN
//   [30] attr_id(1) + attr_len(2)           Service Descriptor Attribute
//   [33] 88 69 19 9D 92 09                  service ID = "org.opendroneid.remoteid" 的哈希
//   [39] instance_id / requestor_id / control / service_info_len
//   [43] ← ODID 载荷（首字节是消息计数器）
int odidFindNanPayload(const uint8_t* frame, int len, int& outLen) {
  outLen = 0;
  if (!frame || len <= 44) return -1;

  static const uint8_t NAN_CLUSTER[6] = {0x50, 0x6F, 0x9A, 0x01, 0x00, 0xFF};
  static const uint8_t WFA_OUI[3]     = {0x50, 0x6F, 0x9A};
  // "org.opendroneid.remoteid" 的服务哈希。校验它才分得清这是 RID 的 NAN 服务还是别的，
  // 光看 OUI 的话任何一个 NAN 服务都会被当成 Remote ID。
  static const uint8_t NAN_SERVICE[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

  for (int i = 0; i < 6; i++) if (frame[16 + i] != NAN_CLUSTER[i]) return -1;
  if (frame[24] != 0x04 || frame[25] != 0x09) return -1;
  for (int i = 0; i < 3; i++) if (frame[26 + i] != WFA_OUI[i]) return -1;
  for (int i = 0; i < 6; i++) if (frame[33 + i] != NAN_SERVICE[i]) return -1;

  const int off = 43;
  outLen = len - off;
  return outLen > 0 ? off : -1;
}

bool djiDecode(const uint8_t* payload, int len, OdidResult& out) {
  if (!payload || len < 10) return false;
  const uint8_t subtype = payload[0];
  if (subtype != 0x10 && subtype != 0x11) return false;
  bool any = false;

  // 检索 14~18 字节序列号（常以 1581 / 0T8 / 146 等 DJI 特征开头）
  for (int off = 1; off + 16 <= len && off < 32; off++) {
    char idBuf[21];
    if (asciiField(payload + off, 16, idBuf, 10)) {
      if (odidVendorName(idBuf)[0] != 0 || (idBuf[0] >= '0' && idBuf[0] <= '9')) {
        memcpy(out.uasId, idBuf, sizeof(out.uasId));
        out.haveBasic = true;
        out.idType = 1;
        out.uaType = 2; // 多旋翼
        any = true;
        break;
      }
    }
  }

  // 坐标解析：扫描 1e-7 度格式的 32 位经纬度对
  if (subtype == 0x10 && len >= 24) {
    for (int off = 8; off + 8 <= len; off += 2) {
      int32_t loRaw = rd32(payload + off);
      int32_t laRaw = rd32(payload + off + 4);
      if (!coordRawBad(loRaw) && !coordRawBad(laRaw)) {
        double lo = loRaw * 1e-7, la = laRaw * 1e-7;
        if (coordPairOk(la, lo)) {
          out.lat = la; out.lon = lo; out.haveLoc = true;
          any = true;
          break;
        }
      }
    }
  }
  return any;
}

bool ridFromMgmtFrame(const uint8_t* fr, int len, OdidResult& out, bool* outIsRidCandidate) {
  if (outIsRidCandidate) *outIsRidCandidate = false;
  if (!fr || len < 36) return false;
  if ((fr[0] & 0x0C) != 0) return false;       // 802.11 Frame Control: 仅处理管理帧 (Type 00)
  const uint8_t fsub = (uint8_t)((fr[0] >> 4) & 0x0F);

  if (fsub == 13) {                       // action 帧 = NAN 那条路
    int n = 0;
    const int off = odidFindNanPayload(fr, len, n);
    return off > 0 && ridDecode(fr + off, n, out);
  }
  if (fsub != 8 && fsub != 5) return false;   // 只看 beacon 和 probe response

  // 24 字节 MAC 头 + 12 字节固定参数之后才是 tagged params
  int o = 36;
  while (o + 2 <= len) {
    const uint8_t tag = fr[o], tlen = fr[o + 1];
    if (o + 2 + tlen > len) break;
    if (tag == 0 && tlen >= 4 && outIsRidCandidate) {
      if (memcmp(fr + o + 2, "RID-", 4) == 0) {
        *outIsRidCandidate = true;
      }
    }
    if (tag == 221 && tlen >= 4) {        // vendor specific
      const uint8_t* v = fr + o + 2;
      if (tlen >= 5 && v[0] == ODID_OUI_0 && v[1] == ODID_OUI_1 &&
          v[2] == ODID_OUI_2 && v[3] == ODID_OUI_TYPE) {
        if (ridDecode(v + 4, tlen - 4, out)) return true;
      } else if (v[0] == 0x26 && v[1] == 0x37 && v[2] == 0x12) {
        if (djiDecode(v + 3, tlen - 3, out)) return true;
      }
    }
    o += 2 + tlen;
  }
  return false;
}

const char* odidVendorName(const char* uasId) {
  if (!uasId || !uasId[0]) return "";
  if (strncmp(uasId, "1581F", 5) == 0 || strncmp(uasId, "1581E", 5) == 0 ||
      strncmp(uasId, "0T8", 3) == 0   || strncmp(uasId, "146", 3) == 0) {
    return "DJI";
  }
  if (strncmp(uasId, "1198", 4) == 0) return "Autel";
  if (strncmp(uasId, "1688", 4) == 0) return "EHang";
  if (strncmp(uasId, "1345", 4) == 0) return "FIMI";
  if (strncmp(uasId, "1583", 4) == 0) return "XAG";
  if (strncmp(uasId, "1096", 4) == 0) return "Skydio";
  if (strncmp(uasId, "0518", 4) == 0) return "Parrot";
  if (strncmp(uasId, "1248", 4) == 0) return "Yuneec";
  return "";
}

const char* odidIdTypeName(uint8_t t) {
  switch (t) {
    case 0: return "none";
    case 1: return "serial";
    case 2: return "CAA";
    case 3: return "UTM";
    case 4: return "session";
    default: return "?";
  }
}

const char* odidUaTypeName(uint8_t t) {
  switch (t) {
    case 0:  return "none";
    case 1:  return "aeroplane";
    case 2:  return "multirotor";
    case 3:  return "gyroplane";
    case 4:  return "vtol";
    case 5:  return "ornithopter";
    case 6:  return "glider";
    case 7:  return "kite";
    case 8:  return "free-balloon";
    case 9:  return "captive-balloon";
    case 10: return "airship";
    case 11: return "parachute";
    case 12: return "rocket";
    case 13: return "tethered";
    case 14: return "ground-obstacle";
    default: return "other";
  }
}

const char* odidPilotLocName(uint8_t t) {
  switch (t) {
    case 1:  return "live-gnss";
    case 2:  return "takeoff";
    case 3:  return "fixed";
    default: return "unknown";
  }
}

const char* gbStatusName(uint8_t t) {
  switch (t) {
    case 0:  return "未报告";
    case 1:  return "ground";
    case 2:  return "airborne";
    case 3:  return "EMERGENCY";
    case 4:  return "RID-failed";
    case 5:  return "RID-failed+EMG";
    default: return "?";
  }
}

const char* gbCoordSysName(uint8_t t) {
  switch (t) {
    case 0:  return "WGS-84";
    case 1:  return "CGCS2000";
    case 2:  return "GLONASS-PZ90";
    default: return "?";
  }
}

const char* gbClassName(uint8_t t) {
  switch (t) {
    case 0:  return "micro";
    case 1:  return "light";
    case 2:  return "small";
    case 3:  return "other-RID";
    case 4:  return "medium";
    case 5:  return "large";
    default: return "?";
  }
}

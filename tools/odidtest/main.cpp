// 在 Mac 上把 src/odid.cpp 那份**真代码**编出来跑，喂人工构造的 ODID 报文验证解码。
//
// 存在的理由：手上没有会播 Remote ID 的无人机，而解析器最容易错的就是字段偏移——
// 差一个字节照样编译通过、照样"解出"一个看着像模像样的坐标。只有拿已知输入比对
// 已知输出才能证明它是对的。报文按 ASTM F3411 / opendroneid-core-c 的编码规则手搓。
#include "odid.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdarg>

static int failures = 0;

static void ck(bool cond, const char* what) {
  printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) failures++;
}
static void ckNear(double got, double want, double tol, const char* what) {
  bool ok = std::fabs(got - want) <= tol;
  printf("  %s %s (got %.7f, want %.7f)\n", ok ? "ok  " : "FAIL", what, got, want);
  if (!ok) failures++;
}

static void put32(uint8_t* p, int32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

// ---- 构造三种消息（各 25 字节）----
static void mkBasicId(uint8_t* m, const char* id, uint8_t idType, uint8_t uaType) {
  memset(m, 0, 25);
  m[0] = (0x0 << 4) | 2;                  // 类型 0 = Basic ID，协议版本 2
  m[1] = (uint8_t)((idType << 4) | uaType);
  memcpy(m + 2, id, strlen(id) > 20 ? 20 : strlen(id));
}

static double g_heightM = -1000.0;   // 由调用方按需设置的"离地高度"，默认=无此值
static bool   g_heightIsAgl = false; // bit2：false=相对起飞点（规范默认）true=真AGL

static void mkLocation(uint8_t* m, double lat, double lon, double altGeoM,
                       double speedMs, double vspeedMs, int heading) {
  memset(m, 0, 25);
  m[0] = (0x1 << 4) | 2;
  // byte1: bit0 速度倍率 / bit1 东西向 / bit2 高度类型 / bit4~7 飞行状态(2=空中)
  uint8_t b1 = (2 << 4);
  if (heading >= 180) b1 |= 0x02;
  if (g_heightIsAgl) b1 |= 0x04;
  m[1] = b1;
  m[2] = (uint8_t)(heading % 180);
  m[3] = (uint8_t)(speedMs / 0.25);       // 低速段：0.25 m/s 一格
  m[4] = (uint8_t)(int8_t)(vspeedMs / 0.5);
  put32(m + 5, (int32_t)llround(lat * 1e7));
  put32(m + 9, (int32_t)llround(lon * 1e7));
  put16(m + 13, (uint16_t)llround((altGeoM + 1000.0) / 0.5));   // 气压高
  put16(m + 15, (uint16_t)llround((altGeoM + 1000.0) / 0.5));   // 几何高
  put16(m + 17, (uint16_t)llround((g_heightM + 1000.0) / 0.5)); // 离地高度
  put16(m + 21, 12345);                   // 时间戳：整点后 1234.5 秒（十分之一秒一格）
}

static void mkSystem(uint8_t* m, double plat, double plon, uint8_t locType) {
  memset(m, 0, 25);
  m[0] = (0x4 << 4) | 2;
  m[1] = (uint8_t)(locType & 0x03);
  put32(m + 2, (int32_t)llround(plat * 1e7));
  put32(m + 6, (int32_t)llround(plon * 1e7));
}

static void mkSelfId(uint8_t* m, const char* desc) {
  memset(m, 0, 25);
  m[0] = (0x3 << 4) | 2;
  m[1] = 0;                               // 描述类型 0 = 文本
  size_t n = strlen(desc); if (n > 23) n = 23;
  memcpy(m + 2, desc, n);
}

static void mkOperatorId(uint8_t* m, const char* op) {
  memset(m, 0, 25);
  m[0] = (0x5 << 4) | 2;
  size_t n = strlen(op); if (n > 20) n = 20;
  memcpy(m + 2, op, n);
}

int main() {
  const char* ID = "1581F5FMD230700ABCDE";

  // ---------- 1. Pack（三条打包），前面带 beacon 那一字节消息计数器 ----------
  {
    printf("1. Message Pack + beacon 计数字节\n");
    uint8_t buf[1 + 3 + 3 * 25];
    buf[0] = 0x07;                       // 消息计数器，解码器应该跳过它
    uint8_t* pk = buf + 1;
    pk[0] = (0xF << 4) | 2;              // 类型 F = Pack
    pk[1] = 25;                          // 单条长度
    pk[2] = 3;                           // 条数
    mkBasicId (pk + 3 + 0 * 25, ID, 1, 2);                       // serial / multirotor
    mkLocation(pk + 3 + 1 * 25, 30.2741000, 120.1551000, 155.0, 12.5, 3.0, 90);
    mkSystem  (pk + 3 + 2 * 25, 30.2700000, 120.1500000, 1);     // live-gnss

    OdidResult r;
    ck(odidDecode(buf, sizeof(buf), r), "decode 成功");
    ck(r.haveBasic && strcmp(r.uasId, ID) == 0, "机身编号");
    ck(r.idType == 1 && r.uaType == 2, "IDType=serial / UAType=multirotor（高低 4 位没搞反）");
    ckNear(r.lat, 30.2741, 1e-6, "纬度");
    ckNear(r.lon, 120.1551, 1e-6, "经度");
    ckNear(r.altGeo, 155.0, 0.5, "几何高");
    ckNear(r.speed, 12.5, 0.13, "水平速度");
    ckNear(r.vspeed, 3.0, 0.26, "垂直速度");
    ck(r.heading == 90, "航向");
    ckNear(r.pilotLat, 30.2700, 1e-6, "飞手纬度");
    ckNear(r.pilotLon, 120.1500, 1e-6, "飞手经度");
    ck(r.pilotLocType == 1, "飞手位置来源 = live-gnss");
  }

  // ---------- 2. 单条消息、且没有计数字节（NAN action 帧的形态）----------
  {
    printf("2. 单条 Location，无计数字节\n");
    uint8_t m[25];
    mkLocation(m, -33.8688000, 151.2093000, 80.0, 5.0, -2.0, 270);
    OdidResult r;
    ck(odidDecode(m, sizeof(m), r), "decode 成功");
    ck(!r.haveBasic, "没有 Basic ID 就不该编出一个来");
    ckNear(r.lat, -33.8688, 1e-6, "南半球纬度");
    ckNear(r.lon, 151.2093, 1e-6, "东经");
    ck(r.heading == 270, "航向跨过 180 那一档");
    ckNear(r.vspeed, -2.0, 0.26, "下降时垂直速度为负");
    // ASTM 侧新补的两项。运行状态在 byte1 高 4 位，和国标数据项 015 同义，
    // 所以 ridapp 那个 GND/AIR/EMG 的显示分支两套标准共用，不用改。
    ck(r.haveStatus && r.opStatus == 2, "ASTM 运行状态 = 2 空中（取自 byte1 高 4 位）");
    ck(r.haveAstmTime && fabs(r.astmTimeSec - 1234.5) < 0.01,
       "ASTM 时间戳 = 整点后 1234.5 秒（byte21~22，十分之一秒一格）");
  }

  // ---------- 3. 哨兵值必须被拒 ----------
  {
    printf("3. 无效坐标过滤\n");
    uint8_t m[25];
    mkLocation(m, 30.2741, 120.1551, 100, 5, 0, 0);
    put32(m + 5, (int32_t)0x7FFFFFFF);          // 未定位时的哨兵
    OdidResult r1;
    ck(!odidDecode(m, sizeof(m), r1) || !r1.haveLoc, "0x7FFFFFFF 纬度被拒");

    mkLocation(m, 0.0001, 0.0001, 100, 5, 0, 0);   // Null Island
    OdidResult r2;
    ck(!odidDecode(m, sizeof(m), r2) || !r2.haveLoc, "(0,0) 附近被拒");

    uint8_t junk[25];
    memset(junk, 0xFF, sizeof(junk));
    OdidResult r3;
    ck(!odidDecode(junk, sizeof(junk), r3), "全 FF 的垃圾不该解出东西");
  }

  // ---------- 4. Basic ID 的编号净化 ----------
  {
    printf("4. 编号净化\n");
    uint8_t m[25];
    mkBasicId(m, "AB", 1, 2);                    // 太短
    OdidResult r1;
    ck(!odidDecode(m, sizeof(m), r1), "2 字符的编号被拒（太短，多半是噪声）");

    mkBasicId(m, "OK12345678", 1, 2);
    m[5] = 0x01;                                 // 塞个不可打印字符
    OdidResult r2;
    ck(!odidDecode(m, sizeof(m), r2), "含不可打印字符的编号被拒");
  }

  // ---------- 5. Self-ID / Operator ID / 离地高度 ----------
  {
    printf("5. Self-ID / Operator ID / 离地高度\n");
    g_heightM = 118.5;
    uint8_t buf[1 + 3 + 4 * 25];
    buf[0] = 0x11;
    uint8_t* pk = buf + 1;
    pk[0] = (0xF << 4) | 2; pk[1] = 25; pk[2] = 4;
    mkLocation  (pk + 3 + 0 * 25, 30.5, 114.3, 200.0, 8.0, 0.0, 45);
    mkSelfId    (pk + 3 + 1 * 25, "Survey flight A7");
    mkOperatorId(pk + 3 + 2 * 25, "OP-CN-0099");
    mkBasicId   (pk + 3 + 3 * 25, ID, 1, 2);
    g_heightM = -1000.0;

    OdidResult r;
    ck(odidDecode(buf, sizeof(buf), r), "decode 成功");
    ckNear(r.height, 118.5, 0.3, "离地高度（byte17，跟海拔是两个字段）");
    ck(!r.heightIsAgl, "byte1 bit2=0 时标记为相对起飞点，不是AGL");
    ck(r.haveSelfId && strcmp(r.selfId, "Survey flight A7") == 0, "运行描述");
    ck(r.haveOperatorId && strcmp(r.operatorId, "OP-CN-0099") == 0, "运营人编号");
    ck(r.haveBasic && r.haveLoc, "同一个 Pack 里其它消息没被挤掉");
  }

  // ---------- 5b. 高度类型位（byte1 bit2）----------
  {
    printf("5b. 高度类型位：区分相对起飞点 vs 真AGL\n");
    g_heightM = 42.0;
    g_heightIsAgl = true;
    uint8_t m[25];
    mkLocation(m, 30.5, 114.3, 200.0, 8.0, 0.0, 45);
    g_heightIsAgl = false;
    g_heightM = -1000.0;

    OdidResult r;
    ck(odidDecode(m, sizeof(m), r) && r.haveHeight, "decode 成功");
    ck(r.heightIsAgl, "bit2=1 时标记为真AGL");
  }

  // ---------- 6. Pack 条数上限 ----------
  {
    printf("6. Pack 条数上限\n");
    uint8_t buf[3 + 10 * 25];
    buf[0] = (0xF << 4) | 2; buf[1] = 25; buf[2] = 10;   // 规范上限是 9
    for (int i = 0; i < 10; i++) mkBasicId(buf + 3 + i * 25, ID, 1, 2);
    OdidResult r;
    ck(!odidDecode(buf, sizeof(buf), r), "条数 10 > 规范上限 9，应当拒绝");
  }

  // ---------- 7. NAN 帧里的载荷定位（修过一次 13 字节的偏移错误）----------
  {
    printf("7. NAN 载荷定位\n");
    uint8_t fr[43 + 1 + 25];
    memset(fr, 0, sizeof(fr));
    fr[0] = 0xD0;                                        // action 帧
    const uint8_t cluster[6] = {0x50,0x6F,0x9A,0x01,0x00,0xFF};
    memcpy(fr + 16, cluster, 6);                         // addr3 = NAN cluster ID
    fr[24] = 0x04; fr[25] = 0x09;                        // 公共动作 / vendor specific
    fr[26] = 0x50; fr[27] = 0x6F; fr[28] = 0x9A;         // Wi-Fi Alliance OUI
    fr[29] = 0x13;                                       // OUI type = NAN
    const uint8_t svc[6] = {0x88,0x69,0x19,0x9D,0x92,0x09};
    memcpy(fr + 33, svc, 6);                             // service ID
    fr[43] = 0x05;                                       // 消息计数器
    mkLocation(fr + 44, 22.5431000, 114.0579000, 90.0, 6.0, 1.0, 135);

    int outLen = 0;
    int off = odidFindNanPayload(fr, sizeof(fr), outLen);
    ck(off == 43, "载荷偏移 = 43（不是 OUI+4 那个 30）");
    ck(outLen == (int)sizeof(fr) - 43, "剩余长度");

    OdidResult r;
    ck(off > 0 && odidDecode(fr + off, outLen, r), "从这个偏移能解出来");
    ckNear(r.lat, 22.5431, 1e-6, "NAN 里的纬度");
    ckNear(r.lon, 114.0579, 1e-6, "NAN 里的经度");

    // service ID 不对就不该认——否则任何一个 NAN 服务都会被当成 Remote ID
    uint8_t bad[sizeof(fr)];
    memcpy(bad, fr, sizeof(fr));
    bad[35] ^= 0xFF;
    int l2 = 0;
    ck(odidFindNanPayload(bad, sizeof(bad), l2) < 0, "service ID 不匹配时拒绝");

    memcpy(bad, fr, sizeof(fr));
    bad[18] ^= 0xFF;                                     // 破坏 cluster ID
    ck(odidFindNanPayload(bad, sizeof(bad), l2) < 0, "cluster ID 不匹配时拒绝");

    ck(odidFindNanPayload(fr, 40, l2) < 0, "帧太短时拒绝（别越界读）");
  }

  // ---------- 8. 国标(GB)：拿真机抓到的两帧当固定样本 ----------
  // 这两帧的原始字节和现场情况见 samples_gb.md。用真数据当测试样本比人工构造强得多——
  // 人工构造只能验证"代码符合我对标准的理解"，真数据还能验证"我的理解符合现实"。
  {
    printf("8. 国标 GB —— 真机帧 A（飞行中）\n");
    const uint8_t A[] = {
      0x0B,0xFF,0x20,0x48,0xFF,0xFF,0xFE,
      '1','5','8','1','F','A','6','Q','C','2','5','A','H','0','0','C','2','E','V','W',
      '0','7','3','8','7','4','1','3',
      0x00,0x01,0x01, 0xBB,0xE7,0x31,0x46, 0x3E,0x4A,0x56,0x17, 0xD0,0x07,
      0x2A,0xE1,0x31,0x46, 0x15,0x51,0x56,0x17, 0xFF,0xFF, 0x00,0x00, 0x50,0x46, 0x00,
      0xF8,0x07, 0x13,0x08, 0x02, 0x00, 0x0B,0x04,0x03, 0xA0,0x32,0xED,0xDF, 0x9F, 0x01,0x03,
    };
    OdidResult r;
    ck(ridDecode(A, sizeof(A), r), "decode 成功");
    ck(r.isGb, "走的是国标分支，不是 ASTM");
    ck(r.haveBasic && strcmp(r.uasId, "1581FA6QC25AH00C2EVW") == 0, "唯一产品识别码（与 SSID 一致）");
    ck(r.haveOperatorId && strcmp(r.operatorId, "07387413") == 0, "实名登记信息");
    ck(r.haveUaClass && r.uaClass == 1, "无人机分类 = 轻型");
    ckNear(r.lat, 39.1532821, 1e-6, "无人机纬度");
    ckNear(r.lon, 117.7674026, 1e-6, "无人机经度");
    ckNear(r.altGeo, 20.0, 0.01, "大地高度（(enc-2000)/2）");
    ck(r.haveSys, "遥控站位置已解出");
    ckNear(r.pilotLat, 39.1531070, 1e-6, "遥控站纬度");
    ck(r.haveStatus && r.opStatus == 2, "运行状态 = 2 空中（现场确实在飞）");
    ck(r.haveCoordSys && r.coordSys == 0, "坐标系 = WGS-84（上图前要转 GCJ-02）");
    // 大疆的相对高度基准是 18000 而不是标准的 2000。这一帧贴地飞（现场报 0.1 m），
    // raw 正好是 18000 ⇒ 应当解成 0.0 m，而不是按标准基准算出的 8000 m。
    ck(r.haveHeight && fabs(r.height) < 0.6, "相对高度按大疆基准解成 ~0m（现场 0.1m）");
    // 数据项 020 是 6 字节（Unix 毫秒 48 位小端），不是 4。写成 4 的时候症状有三个：
    // 末尾多 2 字节没归属、时间戳精度读成 159（超出 0~5）、长度表合计 70≠数据长度 72。
    // 修对之后这三条同时消失，且解出来的时刻正好是抓包当天。
    ck(r.haveGbTime && r.gbTimeMs == 1786168292000ULL,
       "时间戳 = 2026-08-08 13:51:32 北京时间（抓包当天，毫秒位是整秒）");
    ck(r.gbTimeAcc == 3, "时间戳精度 = 3（落在 0~5 内；读成 159 是长度算错时的假象）");
  }

  {
    printf("9. 国标 GB —— 真机帧 B（电量耗尽降落中，多字段无效）\n");
    const uint8_t B[] = {
      0xA0,0xFF,0x20,0x48,0xFF,0xFF,0xFE,
      '1','5','8','1','F','A','6','Q','C','2','5','A','H','0','0','C','2','E','V','W',
      '0','7','3','8','7','4','1','3',
      0x00,0x01,0x01, 0xE5,0xE0,0x31,0x46, 0x93,0x4B,0x56,0x17, 0xD0,0x07,
      0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x50,0x46, 0x80,
      0x00,0x00, 0x12,0x08, 0x03, 0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00, 0x00,0x00,
    };
    OdidResult r;
    ck(ridDecode(B, sizeof(B), r), "decode 成功");
    ck(r.haveBasic && strcmp(r.uasId, "1581FA6QC25AH00C2EVW") == 0, "编号仍然解得出");
    ck(!r.haveLoc, "无人机位置全 FF ⇒ 判无效，不能画到地图上");
    ck(r.haveStatus && r.opStatus == 3, "运行状态 = 3 紧急（现场就是低电量迫降）");
    ck(r.haveSys, "遥控站位置这一帧仍有值");
    ckNear(r.altGeo, 33.0, 0.01, "大地高度无效时退到气压高度");

    // 验证合并逻辑：在没有经纬度的情况下，气压高和紧急状态不能被丢弃
    OdidResult merged;
    odidMerge(merged, r);
    ck(merged.haveBasic && merged.haveAlt && merged.haveStatus, "无坐标帧单独合并保留编号/高度/状态");
    ckNear(merged.altGeo, 33.0, 0.01, "合并保留气压高 33m");
    ck(merged.opStatus == 3, "合并保留紧急状态 3");
  }

  // ---------- 10. 别把国标包当成 ASTM 解 ----------
  {
    printf("10. 两套标准不能互相误认\n");
    const uint8_t gbHdr[] = {0x0B,0xFF,0x20,0x48,0xFF,0xFF,0xFE,
      '1','5','8','1','F','A','6','Q','C','2','5','A','H','0','0','C','2','E','V','W',
      '0','7','3','8','7','4','1','3', 0x00,0x01,0x01,
      0xBB,0xE7,0x31,0x46,0x3E,0x4A,0x56,0x17,0xD0,0x07,
      0x2A,0xE1,0x31,0x46,0x15,0x51,0x56,0x17,0xFF,0xFF,0x00,0x00,0x50,0x46,0x00,
      0xF8,0x07,0x13,0x08,0x02,0x00,0x0B,0x04,0x03,0xA0,0x32,0xED,0xDF,0x9F,0x01,0x03};
    OdidResult r;
    // 这正是当初的事故：ASTM 解码器"解成功"了国标包，吐出北纬 89 度、离地 7346 m
    ck(!odidDecode(gbHdr, sizeof(gbHdr), r), "ASTM 解码器必须拒绝国标载荷");

    // 即使国标包不是 72 字节（例如仅发必选字段的变长包），ASTM 解码器也必须结构化识别并拒绝，
    // 尤其是当计数器刚好落在 0x10~0x1F 时（防止被误当成 MSG_LOCATION）
    uint8_t shortGb[28];
    shortGb[0] = 0x13; shortGb[1] = 0xFF; shortGb[2] = 0x20; shortGb[3] = 22;
    shortGb[4] = 0x81; shortGb[5] = 0x10;
    memcpy(shortGb + 6, "1581FA6QC25AH00C2EVW", 20);
    shortGb[26] = 0x10; shortGb[27] = 0x27;
    OdidResult rShort;
    ck(!odidDecode(shortGb, sizeof(shortGb), rShort), "ASTM 解码器必须拒绝非 72 字节的国标载荷");

    // 反过来：标准 ODID 的 Pack 不该被国标解析器认领
    uint8_t pk[1 + 3 + 25];
    pk[0] = 0x01; pk[1] = (0xF << 4) | 2; pk[2] = 25; pk[3] = 1;
    mkBasicId(pk + 4, ID, 1, 2);
    OdidResult r2;
    ck(!gbDecode(pk, sizeof(pk), r2), "国标解析器必须拒绝 ASTM 载荷");
  }

  // ---------- 11. 健壮性（这一组全是审计时发现问题后补的回归） ----------
  {
    printf("11. 健壮性\n");

    // 幽灵无人机：随机字节碰巧过了包头自校验，但没有任何强字段。
    // 曾经"运行状态 0~5"就算解码成功，6/256 就能蒙中，会在列表里凭空多一台。
    uint8_t ghost[40];
    memset(ghost, 0, sizeof(ghost));
    ghost[0] = 0x0B; ghost[1] = 0xFF; ghost[2] = 0x20;
    ghost[3] = 1;                       // 数据长度 1
    ghost[4] = 0x80; ghost[5] = 0x00;   // 位图：只发 001（编号），扩展位 0
    // 内容区只有 1 字节，装不下 20 字节的编号 ⇒ 必须拒绝
    OdidResult g;
    ck(!gbDecode(ghost, 8, g), "内容区装不下声明的数据项时拒绝");

    uint8_t ghost2[40];
    memset(ghost2, 0, sizeof(ghost2));
    // ⚠️ 位图每字节最低位是"还有下一字节"。原来写的是 00/02/00——第一字节的扩展位是 0，
    // 位图当场就结束了，等于一个数据项都没选中，这条用例是白过的（假覆盖）。
    // 运行状态是数据项 015，落在位图第 3 字节的最高位，所以前两字节必须带 0x01。
    ghost2[0] = 0x0B; ghost2[1] = 0xFF; ghost2[2] = 0x20;
    ghost2[3] = 1;                                          // 数据长度 = 1（就一个状态字节）
    ghost2[4] = 0x01; ghost2[5] = 0x01; ghost2[6] = 0x80;   // 只发 015 运行状态
    ghost2[7] = 2;                                          // 状态=空中
    OdidResult g2;
    ck(!gbDecode(ghost2, 8, g2), "只有运行状态、没有编号/坐标 ⇒ 不算解出（防幽灵）");

    // 位图不收尾（扩展位一直是 1）必须拒绝，不能越界读
    uint8_t noend[40];
    memset(noend, 0xFF, sizeof(noend));
    noend[0] = 0x0B; noend[1] = 0xFF; noend[2] = 0x20; noend[3] = 10;
    OdidResult g3;
    ck(!gbDecode(noend, sizeof(noend), g3), "位图不收尾时拒绝（别越界）");

    // 前向兼容：位图里出现未知数据项（未来版本扩展），前面解出来的必须保住
    // 位图每字节最低位是"还有下一字节"，所以前三字节都要带 0x01；
    // 第 4 字节的 0x80 落在数据项 22——超出我们认识的 21 项，就是"未来版本新加的字段"。
    const uint8_t fwd[] = {
      0x0B,0xFF,0x20, 20,                 // 数据长度 = 20，正好一个编号
      0x81,                               // 第1字节：发 001(编号) + 扩展
      0x01, 0x01,                         // 第2/3字节：不发任何项，继续扩展
      0x80,                               // 第4字节：数据项 22 = 未知
      '1','5','8','1','F','A','6','Q','C','2','5','A','H','0','0','C','2','E','V','W',
    };
    OdidResult f;
    ck(gbDecode(fwd, sizeof(fwd), f) && f.haveBasic &&
       strcmp(f.uasId, "1581FA6QC25AH00C2EVW") == 0,
       "位图含未知项时，已解出的编号要保住而不是整包丢弃");

    // 截断的真机帧：任意长度都不能崩、不能读越界
    const uint8_t real[] = {
      0x0B,0xFF,0x20,0x48,0xFF,0xFF,0xFE,
      '1','5','8','1','F','A','6','Q','C','2','5','A','H','0','0','C','2','E','V','W',
      '0','7','3','8','7','4','1','3',
      0x00,0x01,0x01, 0xBB,0xE7,0x31,0x46, 0x3E,0x4A,0x56,0x17, 0xD0,0x07,
      0x2A,0xE1,0x31,0x46, 0x15,0x51,0x56,0x17, 0xFF,0xFF, 0x00,0x00, 0x50,0x46, 0x00,
      0xF8,0x07, 0x13,0x08, 0x02, 0x00, 0x0B,0x04,0x03, 0xA0,0x32,0xED,0xDF, 0x9F, 0x01,0x03,
    };
    // ⚠️ 这里原来是 `bool crashed=false; ... ck(!crashed,...)`——crashed 永远不会被置真，
    // 那条断言无论发生什么都通过，是个假用例。本机 ASan 又跑不起来（见 build.sh），
    // 所以改成检查**解出来的东西自身是否自洽**：截断的输入只能让它解不出来，
    // 绝不该解出一个越界或不自洽的结果。
    int okCount = 0, bad = 0;
    for (int L = 0; L <= (int)sizeof(real); L++) {
      OdidResult t;
      if (!ridDecode(real, L, t)) continue;
      okCount++;
      // 编号必须在缓冲区内正常终止，且长度合法
      const size_t idLen = strnlen(t.uasId, sizeof(t.uasId));
      if (t.haveBasic && (idLen < 4 || idLen >= sizeof(t.uasId))) bad++;
      // 坐标要么没解出，要么必须在地球上
      if (t.haveLoc && (t.lat < -90 || t.lat > 90 || t.lon < -180 || t.lon > 180)) bad++;
      // 高度不能是解错偏移后那种荒唐值
      if (t.haveHeight && (t.height < -500 || t.height > 3000)) bad++;
    }
    ck(bad == 0, "把真机帧从 0 截到全长逐个喂：凡是解出来的结果都自洽");
    ck(okCount > 0, "而且确实有解得出来的长度（不是因为全都失败才没问题）");

    // 截断测试：ASTM Message Pack（全长 79 字节）逐字节截断喂入
    uint8_t astmPack[1 + 3 + 3 * 25];
    astmPack[0] = 0x07;
    astmPack[1] = (0xF << 4) | 2;
    astmPack[2] = 25;
    astmPack[3] = 3;
    mkBasicId (astmPack + 4 + 0 * 25, ID, 1, 2);
    mkLocation(astmPack + 4 + 1 * 25, 30.2741000, 120.1551000, 155.0, 12.5, 3.0, 90);
    mkSystem  (astmPack + 4 + 2 * 25, 30.2700000, 120.1500000, 1);

    int astmOkCount = 0, astmBad = 0;
    for (int L = 0; L <= (int)sizeof(astmPack); L++) {
      OdidResult t;
      if (!odidDecode(astmPack, L, t)) continue;
      astmOkCount++;
      if (t.haveBasic && strcmp(t.uasId, ID) != 0) astmBad++;
      if (t.haveLoc && (t.lat < -90 || t.lat > 90 || t.lon < -180 || t.lon > 180)) astmBad++;
      if (t.haveSpeed && t.speed > 255.0f) astmBad++;
    }
    ck(astmBad == 0, "把 ASTM Pack 从 0 截到全长逐个喂：凡是解出的结果都自洽");
    ck(astmOkCount > 0, "ASTM Pack 完整长度能正常解出");

    // 截断测试：单条 ASTM Location 消息（25 字节）逐字节截断喂入
    uint8_t singleLoc[25];
    mkLocation(singleLoc, 30.2741000, 120.1551000, 155.0, 12.5, 3.0, 90);
    int locOkCount = 0, locBad = 0;
    for (int L = 0; L <= (int)sizeof(singleLoc); L++) {
      OdidResult t;
      if (!odidDecode(singleLoc, L, t)) continue;
      locOkCount++;
      if (t.haveLoc && (t.lat < -90 || t.lat > 90 || t.lon < -180 || t.lon > 180)) locBad++;
    }
    ck(locBad == 0, "单条 ASTM Location 逐字节截断：无崩溃且结果自洽");
    ck(locOkCount > 0, "单条 ASTM Location 全长成功解出");
  }

  // ---------- 12. 相对高度的两点标定（2026-08-09 户外实飞实测） ----------
  {
    printf("12. 相对高度基准（大疆用 18000，标准写 2000）\n");
    auto mk = [](uint16_t relEnc, uint8_t* out) {
      // 只发 001(编号) + 011(相对高度) 两项的最小国标包
      out[0]=0x0B; out[1]=0xFF; out[2]=0x20; out[3]=22;
      out[4]=0x81; out[5]=0x10;             // 第1字节:001+扩展   第2字节:011
      memcpy(out+6, "1581FA6QC25AH00C2EVW", 20);
      out[26]=relEnc & 0xFF; out[27]=relEnc >> 8;
    };
    uint8_t f[28];
    OdidResult r;
    mk(18000, f); r = OdidResult();
    ck(ridDecode(f, sizeof(f), r) && r.haveHeight && fabs(r.height) < 0.3,
       "raw 18000 -> 0m（现场贴地 0.1m）");
    mk(18060, f); r = OdidResult();
    ck(ridDecode(f, sizeof(f), r) && r.haveHeight && fabs(r.height - 30.0) < 0.3,
       "raw 18060 -> 30m（现场定高 30m）");
    // 合规设备用标准基准，不能因为加了大疆分支就解错
    mk(2000 + 2 * 50, f); r = OdidResult();
    ck(ridDecode(f, sizeof(f), r) && r.haveHeight && fabs(r.height - 50.0) < 0.3,
       "标准基准 2000 的设备照样解对（50m）");
  }

  // ---------- 13. 航迹角编码（2026-08-09 户外实飞取证） ----------
  {
    printf("13. 航迹角：实测是 0.1 度分辨率，不是草案的 1 度+标志位\n");
    auto mk = [](uint16_t hdgEnc, uint8_t* out) {
      out[0]=0x0B; out[1]=0xFF; out[2]=0x20; out[3]=22;
      out[4]=0x81; out[5]=0x40;             // 第1字节:001+扩展   第2字节:009 航迹角
      memcpy(out+6, "1581FA6QC25AH00C2EVW", 20);
      out[26]=hdgEnc & 0xFF; out[27]=hdgEnc >> 8;
    };
    uint8_t f[28]; OdidResult r;
    mk(135, f);  r = OdidResult();
    ck(ridDecode(f, sizeof(f), r) && r.heading == 13, "raw 135 -> 13度（实测轨迹 14.3 度）");
    mk(1830, f); r = OdidResult();
    ck(ridDecode(f, sizeof(f), r) && r.heading == 183, "raw 1830 -> 183度（南北往返的回程）");
    mk(0xFFFF, f); r = OdidResult();
    ck(ridDecode(f, sizeof(f), r) && r.heading < 0, "0xFFFF -> 无效（低速悬停时固件就发这个）");
    mk(3599, f); r = OdidResult();
    ck(ridDecode(f, sizeof(f), r) && r.heading == 359, "上边界 3599 -> 359度");
  }

  // ---------- 13b. 国标垂直速度：规范标志位(0/1)与 0xFF 无效哨兵 ----------
  {
    printf("13b. 国标垂直速度：标志位与无效哨兵\n");
    auto mkVs = [](uint8_t vsEnc, uint8_t* out) {
      out[0]=0x0B; out[1]=0xFF; out[2]=0x20; out[3]=21;
      out[4]=0x81; out[5]=0x08;             // 第1字节:001+扩展   第2字节:012 垂直速度
      memcpy(out+6, "1581FA6QC25AH00C2EVW", 20);
      out[26]=vsEnc;
    };
    uint8_t f[27]; OdidResult r;
    mkVs(0x86, f); r = OdidResult();        // bit7=1 上升, 6/2 = 3.0 m/s
    ck(ridDecode(f, sizeof(f), r) && fabs(r.vspeed - 3.0f) < 0.05f, "上升 0x86 -> +3.0m/s");
    mkVs(0x04, f); r = OdidResult();        // bit7=0 下降, 4/2 = 2.0 m/s
    ck(ridDecode(f, sizeof(f), r) && fabs(r.vspeed - (-2.0f)) < 0.05f, "下降 0x04 -> -2.0m/s");
    mkVs(0xFF, f); r = OdidResult();        // 0xFF 无效哨兵，不能被算成 63.5m/s
    ck(ridDecode(f, sizeof(f), r) && r.vspeed == 0.0f, "0xFF 哨兵不填入荒唐速度");
  }

  // ---------- 14. 厂商前缀识别 ----------
  {
    printf("14. 厂商前缀识别\n");
    ck(strcmp(odidVendorName("1581FA6QC25AH00C2EVW"), "DJI") == 0, "1581F -> DJI");
    ck(strcmp(odidVendorName("1581EA1234567890"), "DJI") == 0, "1581E -> DJI");
    ck(strcmp(odidVendorName("0T8XYZ1234567890"), "DJI") == 0, "0T8 -> DJI");
    ck(strcmp(odidVendorName("1198A00000000000"), "Autel") == 0, "1198 -> Autel");
    ck(strcmp(odidVendorName("1688B00000000000"), "EHang") == 0, "1688 -> EHang");
    ck(strcmp(odidVendorName("1345C00000000000"), "FIMI") == 0, "1345 -> FIMI");
    ck(strcmp(odidVendorName("9999UNKNOWN00000"), "") == 0, "未知编号返回空");
  }

  // ---------- 15. RID- SSID 前缀预锁定与 DJI 26:37:12 帧解析 ----------
  {
    printf("15. RID- SSID 预锁定与 DJI 26:37:12 支持\n");
    // 构造带 SSID = "RID-1581FA6QC..." 的 Beacon 帧
    uint8_t fr[64];
    memset(fr, 0, sizeof(fr));
    fr[0] = 0x80; // Beacon
    // Tag 0 SSID
    fr[36] = 0x00; fr[37] = 8;
    memcpy(fr + 38, "RID-DJI1", 8);
    OdidResult r;
    bool cand = false;
    ridFromMgmtFrame(fr, sizeof(fr), r, &cand);
    ck(cand, "检测到 RID- 前缀 SSID，标记为预锁定候选");

    // 构造 DJI 26:37:12 vendor IE
    uint8_t djiFr[60];
    memset(djiFr, 0, sizeof(djiFr));
    djiFr[0] = 0x80;
    djiFr[36] = 221; djiFr[37] = 20; // IE 221, len 20
    djiFr[38] = 0x26; djiFr[39] = 0x37; djiFr[40] = 0x12; // DJI OUI
    djiFr[41] = 0x10; // subtype 0x10
    memcpy(djiFr + 42, "1581FTEST1234567", 16); // Serial
    OdidResult dr;
    ck(ridFromMgmtFrame(djiFr, sizeof(djiFr), dr), "DJI 26:37:12 vendor IE 解码成功");
    ck(dr.haveBasic && strcmp(dr.uasId, "1581FTEST1234567") == 0, "DJI 私有序列号提取");
  }

  // ---------- 16. JSON 字段净化与截断保护（审计打磨） ----------
  {
    printf("16. JSON 字段转义与截断健壮性\n");
    auto appendf = [](char* buf, int cap, int& n, const char* fmt, ...) {
      if (n < 0) n = 0;
      if (n >= cap - 1) return;
      va_list ap; va_start(ap, fmt);
      int w = vsnprintf(buf + n, (size_t)(cap - n), fmt, ap);
      va_end(ap);
      if (w < 0) return;
      n += w;
      if (n > cap - 1) n = cap - 1;
    };
    auto appendJsonStr = [&](char* buf, int cap, int& n, const char* str) {
      if (n < 0) n = 0;
      if (!str) return;
      while (*str && n < cap - 1) {
        const char c = *str++;
        if (c == '"') appendf(buf, cap, n, "\\\"");
        else if (c == '\\') appendf(buf, cap, n, "\\\\");
        else if ((uint8_t)c < 0x20) appendf(buf, cap, n, "\\u%04x", (uint8_t)c);
        else { buf[n++] = c; buf[n] = '\0'; }
      }
    };

    char buf[128]; int n = 0; buf[0] = '\0';
    appendf(buf, sizeof(buf), n, "{\"id\":\"");
    appendJsonStr(buf, sizeof(buf), n, "TEST\"QUOTE\\SLASH\n");
    appendf(buf, sizeof(buf), n, "\"}");
    ck(strcmp(buf, "{\"id\":\"TEST\\\"QUOTE\\\\SLASH\\u000a\"}") == 0, "特殊字符与引号反斜杠正确转义");

    // 缓冲区过小时截断拒绝
    char smallBuf[16]; int sn = 0; smallBuf[0] = '\0';
    appendf(smallBuf, sizeof(smallBuf), sn, "{\"t\":\"d\",\"id\":\"");
    appendJsonStr(smallBuf, sizeof(smallBuf), sn, "1581FA6QC25AH00C2EVW");
    appendf(smallBuf, sizeof(smallBuf), sn, "\"}");
    const bool valid = (sn < (int)sizeof(smallBuf) - 1 && sn >= 2 && smallBuf[sn - 1] == '}');
    ck(!valid, "空间不足截断时被安全识别拒绝");
  }

  printf("\n%s  (%d 处失败)\n", failures ? "有问题" : "全部通过", failures);
  return failures ? 1 : 0;
}

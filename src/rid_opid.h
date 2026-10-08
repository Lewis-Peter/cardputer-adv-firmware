#pragma once
#include <stdint.h>
#include <string.h>

// 运营人 ID / 实名登记号的"是否播了、看着像不像正经内容"提示。
// ⚠️ 这是观察结果，不是合规判定：各国对编号格式、是否必须广播的规定不同，这里不做任何
// 法域相关的格式校验，只回答三件事——观察够久了没播（NONE）、播了但内容像占位符（SUSPECT）、
// 看着正常（OK）。观察时间太短时一律 PENDING，免得刚发现目标就报"没播"。
enum OpIdState : uint8_t { OPID_PENDING = 0, OPID_OK, OPID_NONE, OPID_SUSPECT };

static const uint32_t OPID_WINDOW_MS = 15000;   // 至少观察这么久才敢说"没播"
static const uint32_t OPID_MIN_PKTS  = 8;       // 且至少收到这么多包

inline OpIdState opidCheck(bool haveOpId, const char* opId, const char* uasId, bool haveUasId,
                           uint32_t observedMs, uint32_t packets) {
  if (!haveOpId) {
    return (observedMs >= OPID_WINDOW_MS && packets >= OPID_MIN_PKTS) ? OPID_NONE : OPID_PENDING;
  }
  // 去掉首尾空格后看内容
  const char* s = opId;
  while (*s == ' ') ++s;
  int len = (int)strlen(s);
  while (len > 0 && s[len - 1] == ' ') --len;
  if (len == 0) return OPID_SUSPECT;                       // 全空格
  bool same = true;
  for (int i = 1; i < len; ++i) if (s[i] != s[0]) { same = false; break; }
  if (same && len >= 3) return OPID_SUSPECT;               // 0000000 / AAAAAAA / ------
  if (haveUasId && uasId && (int)strlen(uasId) == len && strncmp(uasId, s, len) == 0)
    return OPID_SUSPECT;                                   // 直接抄机身序列号
  return OPID_OK;
}

inline const char* opidName(OpIdState s) {
  switch (s) {
    case OPID_OK:      return "ok";
    case OPID_NONE:    return "none";
    case OPID_SUSPECT: return "suspect";
    default:           return "pending";
  }
}

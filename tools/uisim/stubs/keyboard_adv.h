#pragma once
#include <cstdint>

namespace kbd {
  inline void begin() {}
  inline char readKey() { return 0; }
  inline uint8_t modMask() { return 0; }
  inline uint8_t modSticky() { return 0; }
  inline void consumeSticky() {}
  inline void setAutoRepeat(bool) {}
}

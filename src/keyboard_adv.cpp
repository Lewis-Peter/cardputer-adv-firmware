#include "keyboard_adv.h"
#include <M5Unified.h>
#include <cstring>

namespace {

constexpr uint8_t TCA_ADDR = 0x34;
constexpr uint32_t I2C_FREQ = 100000;

constexpr uint8_t REG_INT_STAT   = 0x02;
constexpr uint8_t REG_KEY_LCK_EC = 0x03;
constexpr uint8_t REG_KEY_EVENT_A = 0x04;
constexpr uint8_t REG_KP_GPIO1 = 0x1D;
constexpr uint8_t REG_KP_GPIO2 = 0x1E;
constexpr uint8_t REG_KP_GPIO3 = 0x1F;

// 4 行 × 14 列，未按 Shift（value_first）。特殊键用 HID 码占位。
const char KEYMAP[4][14] = {
  {'`','1','2','3','4','5','6','7','8','9','0','-','=', 0x2a},
  {0x2b,'q','w','e','r','t','y','u','i','o','p','[',']','\\'},
  {(char)0xff,(char)0x81,'a','s','d','f','g','h','j','k','l',';','\'', 0x28},
  {(char)0x80,(char)0x83,(char)0x82,'z','x','c','v','b','n','m',',','.','/',' '},
};
// 按住 Shift 时（value_second）
const char KEYMAP2[4][14] = {
  {'~','!','@','#','$','%','^','&','*','(',')','_','+', 0x2a},
  {0x2b,'Q','W','E','R','T','Y','U','I','O','P','{','}','|'},
  {(char)0xff,(char)0x81,'A','S','D','F','G','H','J','K','L',':','"', 0x28},
  {(char)0x80,(char)0x83,(char)0x82,'Z','X','C','V','B','N','M','<','>','?',' '},
};

uint8_t heldMods = 0;     // 正按住的修饰键（kbd::MOD_* 位掩码，readKey 里按下/松开维护）
uint8_t stickyMods = 0;   // 单击锁定的修饰键
uint8_t tapCandidate = 0; // 按下但还没碰过别的键的修饰键；松开时它才算"单击"
bool fnHeld = false;      // Fn 是否按住（第2行第0列，键值 0xff）

// 长按连发。TCA8418 只在按下/松开时各给一个事件，中间不重复报，所以"还按着"这件事
// 得自己记：记住最后一个产生字符的键位，收到它的松开事件才清掉。
bool     autoRepeat = false;
char     repeatChar = 0;
uint8_t  repeatRow = 0xFF, repeatCol = 0xFF;
uint32_t repeatAtMs = 0;
constexpr uint32_t REPEAT_DELAY_MS = 420;   // 首次重复前的等待，别太短否则正常打字会连击
constexpr uint32_t REPEAT_RATE_MS  = 55;    // 之后每次重复的间隔（≈18 键/秒）

// 修饰键的键位 → 位掩码。不比对 KEYMAP 里的 0x80/0x82/0x83（char 有符号，比着别扭），
// 直接认行列：第2行 col0=Fn col1=Shift，第3行 col0=Ctrl col1=Opt(GUI) col2=Alt。
inline uint8_t modBitAt(uint8_t row, uint8_t col) {
  if (row == 2 && col == 1) return kbd::MOD_SHIFT;
  if (row == 3 && col == 0) return kbd::MOD_CTRL;
  if (row == 3 && col == 1) return kbd::MOD_GUI;
  if (row == 3 && col == 2) return kbd::MOD_ALT;
  return 0;
}

inline void mapRawKey(uint8_t value, uint8_t& row, uint8_t& col) {
  const uint8_t u = value % 10;
  const uint8_t t = value / 10;
  if (u >= 1 && u <= 8 && t <= 6) {
    const uint8_t u0 = u - 1;
    row = u0 & 0x03;
    col = (uint8_t)((t << 1) | (u0 >> 2));
  } else { row = 0xFF; col = 0xFF; }
}
inline uint8_t rd(uint8_t reg) { return M5.In_I2C.readRegister8(TCA_ADDR, reg, I2C_FREQ); }
inline void wr(uint8_t reg, uint8_t val) { M5.In_I2C.writeRegister8(TCA_ADDR, reg, val, I2C_FREQ); }

}  // namespace

namespace kbd {

void begin() {
  wr(REG_KP_GPIO1, 0x7F);
  wr(REG_KP_GPIO2, 0xFF);
  wr(REG_KP_GPIO3, 0x00);
  uint8_t n = rd(REG_KEY_LCK_EC) & 0x0F;
  while (n-- > 0) rd(REG_KEY_EVENT_A);
  wr(REG_INT_STAT, 0x03);
  heldMods = stickyMods = tapCandidate = 0;
  fnHeld = false;
  repeatChar = 0;
  repeatRow = repeatCol = 0xFF;
}

uint8_t modMask() { return heldMods | stickyMods; }
uint8_t modSticky() { return stickyMods; }
void consumeSticky() { stickyMods = 0; }
void setAutoRepeat(bool on) {
  autoRepeat = on;
  if (!on) { repeatChar = 0; repeatRow = repeatCol = 0xFF; }
}

void flushEvents() {
  // 排空 TCA8418 FIFO：把所有待处理事件全部读出丢弃
  uint8_t n = rd(REG_KEY_LCK_EC) & 0x0F;
  while (n-- > 0) rd(REG_KEY_EVENT_A);
  wr(REG_INT_STAT, 0x03);   // 清中断标志
  // 同时重置连发状态，防止阻塞操作结束后残留的连发仍然触发
  repeatChar = 0; repeatRow = repeatCol = 0xFF;
  // 丢掉的事件里可能有修饰键/Fn 的松开，不清掉就会一直"按着"。stickyMods 是用户主动锁的，保留
  heldMods = tapCandidate = 0;
  fnHeld = false;
}

char readKey() {
  uint8_t n = rd(REG_KEY_LCK_EC) & 0x0F;
  while (n-- > 0) {
    uint8_t ev = rd(REG_KEY_EVENT_A);
    bool pressed = ev & 0x80;
    uint8_t value = ev & 0x7F;

    uint8_t row, col;
    mapRawKey(value, row, col);
    if (row >= 4 || col >= 14) continue;

    // 修饰键（Shift/Ctrl/Opt/Alt）：只更新状态，不产生字符。
    // 松开时如果整个按住期间没碰过别的键，就当成"单击"，切换粘滞位。
    if (uint8_t mb = modBitAt(row, col)) {
      if (pressed) { heldMods |= mb; tapCandidate |= mb; }
      else {
        heldMods &= (uint8_t)~mb;
        if (tapCandidate & mb) { stickyMods ^= mb; tapCandidate &= (uint8_t)~mb; }
      }
      continue;
    }
    // Fn 键（第2行第0列）：同上，只更新按住状态
    if (row == 2 && col == 0) { fnHeld = pressed; continue; }

    if (!pressed) {                           // 其余键只处理按下；松开只用来停连发
      if (row == repeatRow && col == repeatCol) { repeatChar = 0; repeatRow = repeatCol = 0xFF; }
      continue;
    }
    tapCandidate = 0;   // 按了实键 ⇒ 同时按住的修饰键是"组合键"，不是单击，不该锁定

    char out = 0;
    if (row == 2 && col == 13)      out = fnHeld ? (char)kbd::KX_FN_ENTER : '\n'; // Enter
    else if (row == 0 && col == 13) out = fnHeld ? (char)kbd::KX_DEL : '\b';     // Backspace / Fn=Delete
    else if (row == 1 && col == 0)  out = fnHeld ? (char)kbd::KX_ESC : '\t';     // Tab / Fn=Esc
    else {
      bool isShift = ((heldMods | stickyMods) & MOD_SHIFT) != 0;
      char c = isShift ? KEYMAP2[row][col] : KEYMAP[row][col];
      if (stickyMods & MOD_SHIFT) stickyMods &= (uint8_t)~MOD_SHIFT;

      // Fn 层：键帽上印的副功能。返回值跟 arduino-esp32 自带 BLE/HIDKeyboardTypes.h 的
      // FUNCTION_KEY 枚举对齐（F1..F12=128.., RIGHT/LEFT/DOWN/UP=148/149/150/151，HOME=145），
      // BLE 键盘转发时能直接查 keymap 表；表里没有的（Esc/End）走 KX_* 私有码。
      // 应用自己的菜单导航只认裸 ; . , /（不按 Fn），所以这里不影响正常导航。
      if (fnHeld) {
        switch (c) {
          case ';': out = (char)151; break;   // UP_ARROW
          case ',': out = (char)149; break;   // LEFT_ARROW
          case '.': out = (char)150; break;   // DOWN_ARROW
          case '/': out = (char)148; break;   // RIGHT_ARROW
          case '`': out = (char)kbd::KX_BACK; break;   // 本机返回，不转发
          case '[': out = (char)145; break;   // HOME
          case ']': out = (char)kbd::KX_END; break;
          default: {
            // 数字行 1234567890-= ⇒ F1..F12，顺序跟键帽一致
            static const char FROW[] = "1234567890-=";
            const char* p = strchr(FROW, c);
            if (p && c) out = (char)(128 + (p - FROW));   // KEY_F1 = 128
            break;
          }
        }
      }

      if (!out) {
        uint8_t uc = (uint8_t)c;
        if (uc < 0x20 || uc > 0x7e) continue;   // 修饰键等非可打印，忽略
        out = c;
      }
    }

    // 记下"还按着的那个键"，供长按连发使用
    repeatRow = row; repeatCol = col; repeatChar = out;
    repeatAtMs = millis() + REPEAT_DELAY_MS;
    return out;
  }

  // FIFO 空了：该不该补一次连发
  if (autoRepeat && repeatChar && (int32_t)(millis() - repeatAtMs) >= 0) {
    repeatAtMs = millis() + REPEAT_RATE_MS;
    return repeatChar;
  }
  return 0;
}

}  // namespace kbd

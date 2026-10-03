#!/usr/bin/env bash
# 在 Mac 上把固件的绘制代码编译成本机程序，渲染出 PNG 用来核对布局。
# 用法： cd tools/uisim && ./build.sh && open out/weather_1_now.png
set -e
cd "$(dirname "$0")"

ROOT=../..
if [ -f "$ROOT/.pio/libdeps/cardputer-adv/M5GFX@0.2.27/src/lgfx/v1/LGFXBase.cpp" ]; then
  GFX=$ROOT/.pio/libdeps/cardputer-adv/M5GFX@0.2.27/src
else
  GFX=$ROOT/.pio/libdeps/cardputer-adv/M5GFX/src
fi
AJSON=$ROOT/.pio/libdeps/cardputer-adv/ArduinoJson/src
TINYGPS=$ROOT/.pio/libdeps/cardputer-adv/TinyGPSPlus/src
SSH=$ROOT/.pio/libdeps/cardputer-adv/LibSSH-ESP32/src
# SDL2 的头/库位置：Mac 上走 brew，Linux 上走 pkg-config（brew 根本不存在）。
# 原来只写了 brew --prefix sdl2，在 Linux 上第一行就挂。
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
  SDL_CFLAGS=$(pkg-config --cflags sdl2)
  SDL_LIBS=$(pkg-config --libs sdl2)
elif command -v brew >/dev/null 2>&1; then
  SDL=$(brew --prefix sdl2)
  SDL_CFLAGS="-I$SDL/include -I$SDL/include/SDL2"
  SDL_LIBS="-L$SDL/lib -lSDL2"
else
  echo "找不到 SDL2：Mac 装 'brew install sdl2'，Linux 装发行版的 sdl2 开发包" >&2
  exit 1
fi

mkdir -p out build

# LovyanGFX 的核心 + SDL 平台层（只要能建 sprite、量字宽、画图元就够，不开窗口）
GFX_SRC="
$GFX/lgfx/v1/LGFXBase.cpp
$GFX/lgfx/v1/LGFX_Sprite.cpp
$GFX/lgfx/v1/misc/common_function.cpp
$GFX/lgfx/v1/misc/pixelcopy.cpp
$GFX/lgfx/v1/misc/SpriteBuffer.cpp
$GFX/lgfx/v1/misc/DividedFrameBuffer.cpp
$GFX/lgfx/v1/lgfx_fonts.cpp
$GFX/lgfx/utility/lgfx_pngle.c
$GFX/lgfx/utility/lgfx_qoi.c
$GFX/lgfx/utility/lgfx_qrcode.c
$GFX/lgfx/utility/lgfx_miniz.c
$GFX/lgfx/Fonts/IPA/lgfx_font_japan.c
$GFX/lgfx/Fonts/efont/lgfx_efont_cn.c
$GFX/lgfx/Fonts/efont/lgfx_efont_ja.c
$GFX/lgfx/Fonts/efont/lgfx_efont_kr.c
$GFX/lgfx/Fonts/efont/lgfx_efont_tw.c
$GFX/lgfx/utility/lgfx_tjpgd.c
$GFX/lgfx/v1/panel/Panel_Device.cpp
$GFX/lgfx/v1/platforms/sdl/common.cpp
"

# CXX 可覆盖：Mac 上默认 clang++，Linux 上是 g++。原来写死 /usr/bin/clang++。
: "${CXX:=c++}"

# ⚠️ ARDUINOJSON_ENABLE_ARDUINO_STREAM 是给 quake.cpp 那种**逐元素流式解析**的页面开的：
# 它们不走 fetchJsonHttp，而是拿 http.getStream() 一条一条喂给 ArduinoJson
# （见 github.cpp 顶上那笔 NoMemory 的账）。桩里的 WiFiClient 因此继承了 Stream。
# 顺带说一句：ArduinoJson 7.4.x 把 Printable 的转换器也挂在这个宏下面，所以
# stubs/sim_arduino.h 里还得陪一份 Print/Printable，固件自己并不用它们。
DEFS="-DLGFX_USE_V1 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=1"
INC="-I stubs -I $GFX -I $AJSON -I $TINYGPS -I $SSH $SDL_CFLAGS"

# ---- 先验菜单表 ----
# globals.cpp / pages.cpp / icons.cpp 不进模拟器二进制（globals.cpp 会跟 sim_main 抢
# 同一批全局定义），但**只做语法检查**是能过的。这么做是为了 globals.cpp 里那两条
# static_assert：GROUPS[] 必须首尾相接盖满 APPS[]、组名必须放得进 tab 条。
# 加 app 最容易错的就是这两张表，而错法是静默的（某个 app 从菜单里消失、
# 或者两组重叠着显示同一个）。在这儿过一遍 = 不用 pio、不插板子也能验。
# ir.cpp / ir_proto.cpp 也放进来：ir.cpp 的硬件那一半拆去了 ir_tx.cpp（要 RMT，桌面上编不了），
# 剩下的界面 + 码表解析是纯逻辑，语法检查能挡住"函数名写错、参数对不上"这类错。
# ram_profile.cpp 进来是为了另一件事：它整段输出都是格式串，而格式串跟参数类型对不上
# 在板子上是**静默**的（照样烧得进去，打出来的数是垃圾）。-Werror=format 把它变成硬错。
for f in globals pages icons ir ir_proto ram_profile clock settings_ui gnss spectrum hash_oven sha512_crypt crypto_engines ssh_app; do
  "$CXX" -std=c++17 -fsyntax-only -Wformat -Werror=format $DEFS $INC "$ROOT/src/$f.cpp" || {
    echo "!! src/$f.cpp 语法检查没过（加了 app 的话，多半是 APPS[]/GROUPS[] 对不上）" >&2
    exit 1
  }
done

"$CXX" -std=c++17 -O1 -w \
  $DEFS \
  $INC \
  sim_main.cpp stubs/mbedtls_stub.cpp stubs/libssh_stub.cpp stubs/tinygps_wrapper.cpp \
  $ROOT/src/globals.cpp $ROOT/src/ui_common.cpp $ROOT/src/clock.cpp $ROOT/src/weather.cpp $ROOT/src/moon.cpp $ROOT/src/astro.cpp $ROOT/src/pages.cpp \
  $ROOT/src/skyview.cpp $ROOT/src/adsb.cpp $ROOT/src/typhoon.cpp $ROOT/src/worldmap.cpp $ROOT/src/sats.cpp $ROOT/src/router.cpp $ROOT/src/stopwatch.cpp $ROOT/src/quake.cpp $ROOT/src/fx.cpp $ROOT/src/okx.cpp $ROOT/src/icons.cpp $ROOT/src/settings_ui.cpp $ROOT/src/ram_profile.cpp \
  $ROOT/src/gnss.cpp $ROOT/src/spectrum.cpp $ROOT/src/hash_oven.cpp $ROOT/src/sha512_crypt.cpp $ROOT/src/crypto_engines.cpp $ROOT/src/ssh_app.cpp \
  $GFX_SRC \
  $SDL_LIBS \
  -o build/uisim

# ⚠️ 先把 PNG 拷出来再传播退出码：自检挂了照样要能看图，否则"哪儿画错了"反而没法查。
cd build && mkdir -p out
./uisim; rc=$?
cp out/*.png ../out/ 2>/dev/null || true
exit $rc

[English](linux-setup.md) | **简体中文**

# Linux 上从零把工具链跑通

    README 的"烧录"一节在 macOS 上够用，但在 Linux（尤其 Arch）上会卡在两个地方，
    而且两个的报错都指向错误的方向。这篇记录实际踩过并验证过的过程。

实测环境：Arch Linux / kernel 7.1.8 / PlatformIO Core 6.1.19 / pipx 1.15.0。
板子通过原生 USB-C 直连，枚举为 `303a:1001 Espressif USB JTAG/serial debug unit`。

---

## 坑 1：pipx 装的 PlatformIO 没有 pip，装不上 esptool

[README.zh-CN.md](../README.zh-CN.md) 提示 `pipx install platformio`。装完 `pio --version` 正常，但第一次 `pio run`
拉工具链时会在 `tool-esptoolpy` 上炸掉：

```
Tool Manager: Installing platformio/tool-esptoolpy @ ~2.41100.0
Downloading 0% 10% 20% 30% 40%
/home/you/.local/share/pipx/venvs/platformio/bin/python: No module named pip

Please ensure that the following packages are installed:

sudo apt install python3-dev libffi-dev libssl-dev

MissingPackageManifestError: Could not find one of 'package.json' manifest files in the package
```

**这两条提示都是误导。**

- `sudo apt install python3-dev ...` —— Arch 上根本没有 apt，而且就算在 Debian 上装了
  这三个包也修不好，因为真正的问题是下一行。
- `MissingPackageManifestError` 是**后果不是原因**：PlatformIO 解包 esptool 后要用 pip
  装它的 Python 依赖，pip 不存在 → 安装中断 → `~/.platformio/packages/tool-esptoolpy/`
  留下一个没有 `package.json` 的半成品目录 → 下次再跑直接报清单缺失。

根因是 pipx 从某个版本起**不再往应用 venv 里放 pip**（用共享的那份），而 PlatformIO
把 `python -m pip` 当成能用的东西。

### 修

```bash
# 1. 给 PlatformIO 的 venv 补上 pip
~/.local/share/pipx/venvs/platformio/bin/python -m ensurepip --upgrade

# 2. 删掉上一次留下的半成品，否则 PlatformIO 认为它已装好、不会重装
rm -rf ~/.platformio/packages/tool-esptoolpy

# 3. 重新编译，这次会完整装上
pio run
```

> 只做第 1 步不够——第 2 步是关键，那个残缺目录会一直让构建失败。

---

## 坑 2：Arch 上没有 `dialout` 组，是 `uucp`

[README.zh-CN.md](../README.zh-CN.md) 写的是 `sudo usermod -aG dialout $USER`。这条在 Debian/Ubuntu 上对，但
**Arch 系没有 `dialout` 组**，串口节点属于 `uucp`：

```console
$ getent group dialout
                          # 空
$ stat -c "%U:%G %a" /dev/ttyACM0
root:uucp 660
```

### 修

```bash
sudo usermod -aG uucp $USER      # 重新登录后生效
```

不想重新登录（比如正在一个远程会话里）可以临时切组。注意 Arch 的 shadow 包**不带 `sg`**，
只有 `newgrp`，而它是交互式的，得从 stdin 喂命令：

```bash
newgrp uucp <<'EOF'
pio run -t upload
EOF
```

### 想用内置 JTAG 调试再加一条 udev 规则

```
SUBSYSTEM=="usb", ATTR{idVendor}=="303a", MODE="0660", GROUP="uucp", TAG+="uaccess"
```

（[README.zh-CN.md](../README.zh-CN.md) 里那条写的是 `GROUP="plugdev"`，Arch 上同样不存在这个组。）

---

## 验证：跑通之后应该看到什么

两个坑都填掉之后，完整链路（编译 → 烧录 → 启动 → 截图）实测如下。

```console
$ pio run
RAM:   [====      ]  36.4% (used 119412 bytes from 327680 bytes)
Flash: [=======   ]  74.6% (used 2492669 bytes from 3342336 bytes)
========================= [SUCCESS] Took 144.16 seconds =========================
```

首次编译 144 秒（含下载 framework-arduinoespressif32 和各个库），之后增量快得多。

```console
$ pio run -t upload
Wrote 2493040 bytes (1615202 compressed) at 0x00010000 in 13.3 seconds (effective 1501.5 kbit/s)
Hash of data verified.
========================= [SUCCESS] Took 34.93 seconds =========================
```

```console
$ python3 tools/shot.py
shot.png  (720x405, 原始 240x135)
```

---

## 顺手更正两处 README 的说法

**串口截屏不需要"十几秒"。** 早期描述说"整屏 RGB565 hex 化之后约 130KB，115200 波特下
十几秒"。实测 **131,581 字节只花 1.3 秒**：

```
总字节 131581, 行数 138, 耗时 1.3s
```

因为板子走的是**原生 USB CDC**，`monitor_speed = 115200` 只是给串口 API 的一个形式参数，
实际传输跑在 USB 全速链路上，不受这个数字约束。烧录那 1501 kbit/s 也是同理。

**`shot.py` 偶发"只收到 0/? 行"不是 DTR 的问题。** 一开始怀疑是 `serial.Serial(port, ...)`
这种构造即打开的写法没设好 DTR/RTS，做了 A/B 对照：

| 打开方式 | 打开后状态 | 结果 |
|---|---|---|
| `serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)` | `dtr=True rts=True` | ✅ 正常 |
| 先设 `dtr=True rts=False` 再 `open()` | `dtr=True rts=False` | ✅ 正常 |

两种都能通，**DTR 无关**。实际触发条件是**前一个进程刚占用过端口就立刻再开**——
间隔一两秒重试即可。写脚本连续截图时中间留点间隔。

---

## 坑 3：三个桌面测试台原来只能在 Mac 上跑

`tools/odidtest` / `tools/powertest` / `tools/uisim` 的 `build.sh` 里编译器写死成
`/usr/bin/clang++`，uisim 还写死了 `SDL=$(brew --prefix sdl2)`——Linux 上三个都是
第一行就 `No such file or directory`。

### 修（已合入）

编译器改成 `: "${CXX:=c++}"`，SDL2 改成先试 `pkg-config sdl2`、没有再回退 brew。
现在两个平台都能直接跑：

```bash
sudo pacman -S sdl2            # Arch；Debian 系是 libsdl2-dev
cd tools/odidtest  && ./build.sh    # Remote ID 解码器，40+ 用例
cd tools/powertest && ./build.sh    # 充电判定/电量推算，两个场景
cd tools/irtest    && ./build.sh    # 红外编码器，8 组用例（SAN=1 可开 sanitizer）
cd tools/uisim     && ./build.sh    # 渲染 25 张页面 PNG 到 out/
```

uisim 还需要 `.pio/libdeps/` 里的 M5GFX 和 ArduinoJson，所以**得先成功 `pio run` 一次**；
另外 `router.cpp` / `sats.cpp` 都 `#include "secrets.h"`，得先
`cp src/secrets.h.example src/secrets.h`。⚠️ 值大多可以留空，但 `N2YO_API_KEY`
要填个非空占位串（比如 `"SIMULATOR"`）——Sats 页在取数之前就先判它空不空，
留空的话那一页只会渲出 "no N2YO key in secrets.h"。

### 附赠：Linux 上 ASan 能用

`odidtest/build.sh` 里原本记着"加 `-fsanitize=address` 会让程序空转卡死，所以不开"——
那是旧版特定平台的问题。Linux + g++ 下完全正常，全部用例通过、零报错。所以加了个开关：

```bash
SAN=1 ./build.sh       # 开 address + undefined
```

解析器是按外来字节算偏移的，越界读是这类代码最典型的坑，在 Linux 上改 `odid.cpp` 时
建议一直带着这个开关跑。

### 另：`uisim` 也栽在同一件事上

跟下面 powertest 那条一模一样的毛病：`260b34e` / `0672c6d` 给 `router.cpp` 加了
`trafClient.stop()` 和 `trafHttp.setReuse(false)`，`tools/uisim/stubs/` 里的假
`WiFiClient` / `HTTPClient` 没跟上，uisim 从此**编不过**——而它不在 CI 里，
所以只有下次想用它的人才会发现。`d7325cf` 引入 `tls_ca.cpp` 之后又多一条：
那个根证书符号是链接器从 `data/cert/` 嵌进 flash 的，桌面上根本不存在。

已经补齐（`stop()` / `setReuse()` / `tlsUseCaBundle()` / `tlsClockReady()`）。
**以后在 `src/` 里用一个新的 Arduino API，顺手 `cd tools/uisim && ./build.sh` 一下**——
它编的是同一份 `src/`，几秒钟就能知道桩有没有掉队。

### 另：`powertest` 曾经链接失败

`824d639` 把 `power_util.cpp` 改成调 `globals.h` 的 `loadUChar`/`saveUChar` 之后，
powertest 的桩没跟上，`undefined reference` 了两周没人发现（它不在 CI 里）。已在
`tools/powertest/main.cpp` 里补上桩。**以后再动 `globals.h` 的 NVS helper，记得跟着补。**

---

## 排错速查

| 症状 | 多半是 |
|---|---|
| `No module named pip` / `MissingPackageManifestError` | 坑 1，且必须连带删掉 `~/.platformio/packages/tool-esptoolpy` |
| `Permission denied: '/dev/ttyACM0'` | 坑 2，`uucp` 不是 `dialout` |
| `/dev/ttyACM*` 压根不出现 | 先 `lsusb \| grep 303a` 确认板子枚举了；有设备但无节点才去查 `cdc_acm` |
| `sg: command not found` | Arch 的 shadow 不带 `sg`，用 `newgrp` + stdin |
| 串口读到 0 字节但设备没坏 | 固件平时不主动打日志，发 `HELP` 试探才有输出 |
| 截图 0 行 | 端口刚被占用过，隔一两秒重试 |
| esptool 连不上 / 烧录卡住 | 按住 G0 再插 USB（见 [README.zh-CN.md](../README.zh-CN.md)），固件开机崩溃时 USB CDC 起不来 |
| `build.sh: /usr/bin/clang++: No such file` | 坑 3，拉一下最新的 build.sh |
| uisim 报找不到 M5GFX / ArduinoJson | 先成功跑一次 `pio run`，它才会把 libdeps 拉下来 |
| uisim 报 `secrets.h: No such file` | `cp src/secrets.h.example src/secrets.h`，它在 .gitignore 里 |
| uisim 报 `WiFiClient has no member named 'stop'` 之类 | `src/` 用了新的 Arduino API 但 `stubs/` 没跟上，见坑 3 |
| powertest `undefined reference to loadUChar` | `globals.h` 的 NVS helper 变了但桩没跟上，见坑 3 末尾 |

**English** | [简体中文](linux-setup.zh-CN.md)

# Setting Up the Toolchain from Scratch on Linux

    While the flashing section of the main README is sufficient for macOS, Linux environments
    (especially Arch Linux) encounter two major setup hurdles whose error messages point in misleading directions.
    This document records the exact issues and verified solutions.

Test environment: Arch Linux / kernel 7.1.8 / PlatformIO Core 6.1.19 / pipx 1.15.0.
Hardware connected directly via native USB-C, enumerating as `303a:1001 Espressif USB JTAG/serial debug unit`.

---

## Issue 1: PlatformIO Installed via pipx Lacks pip, Breaking esptool Installation

[README.md](../README.md) suggests running `pipx install platformio`. While `pio --version` succeeds, the initial `pio run` command fails when installing `tool-esptoolpy`:

```
Tool Manager: Installing platformio/tool-esptoolpy @ ~2.41100.0
Downloading 0% 10% 20% 30% 40%
/home/you/.local/share/pipx/venvs/platformio/bin/python: No module named pip

Please ensure that the following packages are installed:

sudo apt install python3-dev libffi-dev libssl-dev

MissingPackageManifestError: Could not find one of 'package.json' manifest files in the package
```

**Both diagnostic messages are misleading:**

- `sudo apt install python3-dev ...` — Arch does not use apt, and even on Debian installing these packages does not resolve the issue, because the true problem is the line preceding it.
- `MissingPackageManifestError` is a **symptom, not the cause**: after extracting esptool, PlatformIO relies on pip to install Python dependencies. Without pip, installation terminates prematurely, leaving a corrupted directory in `~/.platformio/packages/tool-esptoolpy/` missing `package.json`. Subsequent runs immediately fail on the missing manifest.

The root cause is that recent versions of pipx no longer install pip into application virtual environments, whereas PlatformIO expects `python -m pip` to be functional.

### Fix

```bash
# 1. Supply pip to PlatformIO's virtual environment
~/.local/share/pipx/venvs/platformio/bin/python -m ensurepip --upgrade

# 2. Remove the corrupted partial installation directory, otherwise PlatformIO will assume it is valid
rm -rf ~/.platformio/packages/tool-esptoolpy

# 3. Re-run compilation; packages will install completely
pio run
```

> Step 1 alone is insufficient—Step 2 is crucial because the leftover incomplete directory will continuously break builds.

---

## Issue 2: Arch Uses 'uucp' Group Instead of 'dialout'

[README.md](../README.md) recommends `sudo usermod -aG dialout $USER`. While accurate for Debian/Ubuntu, **Arch Linux has no `dialout` group**; serial device nodes belong to `uucp`:

```console
$ getent group dialout
                          # Empty
$ stat -c "%U:%G %a" /dev/ttyACM0
root:uucp 660
```

### Fix

```bash
sudo usermod -aG uucp $USER      # Takes effect upon re-login
```

To execute commands without re-logging (such as in an active SSH session), switch groups temporarily. Note that Arch's shadow package does not include `sg`; use `newgrp` via stdin:

```bash
newgrp uucp <<'EOF'
pio run -t upload
EOF
```

### Optional udev Rule for Built-in JTAG Debugging

```
SUBSYSTEM=="usb", ATTR{idVendor}=="303a", MODE="0660", GROUP="uucp", TAG+="uaccess"
```

(The README rule specifies `GROUP="plugdev"`, which also does not exist on standard Arch systems.)

---

## Verification: What a Working Setup Looks Like

Once both hurdles are resolved, the complete workflow (compile → flash → boot → screenshot) executes as follows:

```console
$ pio run
RAM:   [====      ]  36.4% (used 119412 bytes from 327680 bytes)
Flash: [=======   ]  74.6% (used 2492669 bytes from 3342336 bytes)
========================= [SUCCESS] Took 144.16 seconds =========================
```

The initial build takes ~144 seconds (downloading framework-arduinoespressif32 and dependencies); subsequent incremental builds are significantly faster.

```console
$ pio run -t upload
Wrote 2493040 bytes (1615202 compressed) at 0x00010000 in 13.3 seconds (effective 1501.5 kbit/s)
Hash of data verified.
========================= [SUCCESS] Took 34.93 seconds =========================
```

```console
$ python3 tools/shot.py
shot.png  (720x405, raw 240x135)
```

---

## Correcting Two Statements from the README

**Serial screenshots do not require "tens of seconds".** Early documentation estimated that transferring ~130KB of hex-encoded RGB565 over 115200 baud would take tens of seconds. In practice, **131,581 bytes transfer in only 1.3 seconds**:

```
Total bytes: 131581, Lines: 138, Duration: 1.3s
```

Because the hardware uses **native USB CDC**, `monitor_speed = 115200` is merely a formal configuration parameter for serial terminal APIs. The physical link operates at native USB Full-Speed bandwidth unconstrained by baud settings. The 1501 kbit/s flashing speed functions on the same principle.

**Intermittent "received 0/? lines" in `shot.py` is not a DTR issue.** Initial suspicion fell on DTR/RTS states when opening ports via `serial.Serial(port, ...)`. An A/B test was conducted:

| Open Method | State After Open | Result |
|---|---|---|
| `serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)` | `dtr=True rts=True` | ✅ OK |
| Pre-set `dtr=True rts=False` then `open()` | `dtr=True rts=False` | ✅ OK |

Both methods work consistently; **DTR is not the cause**. The condition actually occurs when **a new connection opens immediately after a prior process releases the port**—waiting 1–2 seconds before reconnecting avoids this issue. Add slight pauses when scripting consecutive screen captures.

---

## Issue 3: Desktop Testbeds Originally Hardcoded for macOS

`build.sh` scripts across `tools/odidtest`, `tools/powertest`, and `tools/uisim` originally hardcoded compilers to `/usr/bin/clang++`, and uisim hardcoded `SDL=$(brew --prefix sdl2)`. On Linux, all three aborted immediately with `No such file or directory`.

### Fix (Merged)

Compilers now default to `: "${CXX:=c++}"`, and SDL2 resolution checks `pkg-config sdl2` first before falling back to brew. Both platforms are fully supported:

```bash
sudo pacman -S sdl2            # Arch; Debian systems use libsdl2-dev
cd tools/odidtest  && ./build.sh    # Remote ID decoder, 40+ tests
cd tools/powertest && ./build.sh    # Charging detection / battery math
cd tools/irtest    && ./build.sh    # IR encoder, 8 test suites (SAN=1 enables sanitizers)
cd tools/uisim     && ./build.sh    # Render 25 page PNGs to out/
```

uisim requires M5GFX and ArduinoJson from `.pio/libdeps/`, meaning **`pio run` must complete successfully at least once beforehand**.
Additionally, both `router.cpp` and `sats.cpp` `#include "secrets.h"`, requiring `cp src/secrets.h.example src/secrets.h`. ⚠️ While most keys may remain blank, `N2YO_API_KEY` must contain a non-empty placeholder string (e.g. `"SIMULATOR"`)—the Sats view checks for key presence before querying and will render only "no N2YO key in secrets.h" if empty.

### Note: ASan Works on Linux

`odidtest/build.sh` previously noted that `-fsanitize=address` caused hangs; that issue was specific to legacy configurations on other platforms. Under Linux + g++, all test cases pass with zero errors with ASan enabled:

```bash
SAN=1 ./build.sh       # Enables address + undefined sanitizers
```

Since the decoder calculates field offsets from arbitrary external payloads, out-of-bounds reads are common hazards. Running with sanitizers enabled when modifying `odid.cpp` on Linux is strongly recommended.

### Note: uisim Stub Drift

Similar to powertest below: commits `260b34e` / `0672c6d` added `trafClient.stop()` and `trafHttp.setReuse(false)` to `router.cpp`, but mock implementations in `tools/uisim/stubs/` were not updated simultaneously, leaving uisim broken until someone next ran it locally. Commit `d7325cf` introduced `tls_ca.cpp`, requiring a certificate bundle symbol embedded via flash linking that does not exist in desktop environments.

These mocks have been synchronized (`stop()`, `setReuse()`, `tlsUseCaBundle()`, `tlsClockReady()`).
**Whenever introducing new Arduino APIs in `src/`, run `cd tools/uisim && ./build.sh`**—because it compiles against the same `src/` codebase, it catches outdated mocks in seconds.

### Note: powertest Stub Drift

When commit `824d639` changed `power_util.cpp` to call `loadUChar`/`saveUChar` in `globals.h`, powertest stubs lagged behind, generating `undefined reference` link errors. Stubs have been added in `tools/powertest/main.cpp`. **Remember to update mocks whenever changing NVS helpers in `globals.h`**.

---

## Troubleshooting Cheat Sheet

| Symptom | Probable Cause |
|---|---|
| `No module named pip` / `MissingPackageManifestError` | Issue 1; must also delete `~/.platformio/packages/tool-esptoolpy` |
| `Permission denied: '/dev/ttyACM0'` | Issue 2; user belongs to `uucp`, not `dialout` |
| `/dev/ttyACM*` device node does not appear | Run `lsusb \| grep 303a` to verify board enumeration; check `cdc_acm` driver if device exists without node |
| `sg: command not found` | Arch shadow package lacks `sg`; use `newgrp` via stdin |
| Zero bytes read over serial on functional hardware | Firmware suppresses continuous logs; send `HELP` to trigger output |
| Screenshot returns 0 lines | Port was recently released by another process; retry after 1–2 seconds |
| esptool fails to connect / flashing hangs | Hold G0 while plugging in USB (see [README.md](../README.md)); USB CDC cannot initialize if firmware crashes during boot |
| `build.sh: /usr/bin/clang++: No such file` | Issue 3; pull latest build.sh |
| uisim cannot find M5GFX / ArduinoJson | Run `pio run` once first to fetch dependencies into `.pio/libdeps` |
| uisim reports `secrets.h: No such file` | Run `cp src/secrets.h.example src/secrets.h` (file is gitignored) |
| uisim reports `WiFiClient has no member named 'stop'` | `src/` adopted new Arduino APIs not yet mirrored in `stubs/`; see Issue 3 |
| powertest reports `undefined reference to loadUChar` | `globals.h` NVS helpers changed without updating mocks; see Issue 3 |

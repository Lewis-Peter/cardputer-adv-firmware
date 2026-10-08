**English** | [简体中文](README.zh-CN.md)

# Cardputer ADV Custom Firmware

> **Open Source Edition Note**: This is a sanitized public release (with fresh commit history). Before use, run `cp src/secrets.h.example src/secrets.h` and fill in your own API keys.
> Personal configurations such as SSH / VPN probe targets are configured directly on the device (stored in NVS), defaulting to empty or documentation-reserved addresses (203.0.113.10).
> Compile-time defaults (NTP servers, hotspot SSID/default password, network probe targets, first-run SSH defaults) are located in `src/config.h`; copy `src/config_local.h.example` to `src/config_local.h` to override macros.
> Radio presets: edit `CFG_RADIO_PRESETS`, or place `/radio.txt` in the SD card root (one entry per line: `Name|http://direct_url|Genre`, `#` for comments, up to 12 presets, without recompiling).
> Online map tile sources: `CFG_MAP_TILE_URL` (placeholders `{s}{x}{y}{z}`), `CFG_MAP_TILE_SUBS`, `CFG_MAP_TILE_WGS84` (coordinate system).
> `cardputer-bridge` mentioned in documentation is a companion tool, not yet public. License: MIT (`lib/` and `sha512_crypt.*` are third-party code and retain their original licenses).

Custom firmware for the M5Stack Cardputer ADV, packing 40 apps into a 240×135 screen.
Started from "displaying a running clock", and evolved into what it is today.

- SoC: ESP32-S3FN8 (Stamp-S3A), 8MB Flash, **no PSRAM** (a constraint spanning the entire project; see "Lessons Learned & Pitfalls" below)
- Display: 1.14" ST7789, 240×135
- Keyboard: TCA8418 I2C keyboard controller (**not** the GPIO matrix of the original Cardputer; old code cannot be directly ported)
- Framework: Arduino + M5Unified, managed via PlatformIO
- Expansion: Cap LoRa-1262 (SX1262 + ATGM336H GNSS); LoRa / GNSS available only when attached

For hardware pinouts, I2C addresses, and hardware quirks, see [HARDWARE.md](HARDWARE.md).
For toolchain setup and troubleshooting on Linux, see [docs/linux-setup.md](docs/linux-setup.md);
For automated serial sweeping of all apps (memory profiling, batch screenshots), see [docs/serial-sweep.md](docs/serial-sweep.md);
For forensics and debugging crash dumps without an active console, see [docs/ble-teardown.md](docs/ble-teardown.md);
For the full project memory and resource leak audit (including two resolved real leaks), see [docs/memory-audit.md](docs/memory-audit.md).

---

## Flashing

```bash
brew install platformio          # macOS; if not already installed
pipx install platformio          # Linux; not in apt, install via pipx
pio run -t upload                # Build and flash
pio device monitor               # Monitor serial output (115200)
```

The platform, Arduino core, and all libraries are pinned to exact versions in `platformio.ini`. **Do not change them to version ranges**,
and avoid upgrading casually: newer FastLED versions consume an extra 375KB Flash and 6KB RAM, while newer M5GFX versions cut display refresh rates in half.
For reasons, benchmark data, and upgrade steps, see [`docs/dependency-pins.md`](docs/dependency-pins.md).

The board uses native USB on the ESP32-S3 (VID `303A`), **requiring no USB-to-UART drivers on either OS**:
macOS enumerates it as `/dev/cu.usbmodem*`, and Linux enumerates it as `/dev/ttyACM*` (in-kernel cdc_acm driver).
The serial port is usually detected automatically; if not, identify the device path and specify it in `platformio.ini`:

```bash
python3 -m serial.tools.list_ports -v    # Locate the port with VID:PID = 303A:1001
```

> **Linux Permissions**: Run `sudo usermod -aG dialout $USER` (re-login to apply) to access
> `/dev/ttyACM*`. **Arch Linux does not have a `dialout` group; serial devices belong to `uucp`**, so use
> `sudo usermod -aG uucp $USER`. To use the internal JTAG debugger, add a udev rule to grant access to raw USB endpoints:
> `SUBSYSTEM=="usb", ATTR{idVendor}=="303a", MODE="0660", GROUP="uucp", TAG+="uaccess"`
> (On Debian-based distros, replace `uucp` here with `plugdev`.)

> **Linux First `pio run` Error (`No module named pip` / `MissingPackageManifestError`)**:
> The PlatformIO virtual environment installed via pipx does not include pip, failing to install esptool. The suggested
> `sudo apt install python3-dev ...` is misleading and does not resolve it. For the complete troubleshooting process and other Linux tips, see
> [docs/linux-setup.md](docs/linux-setup.md).

> **Connection Failures / Flashing Stuck**: Hold down the **G0 button on the top edge**, plug in the USB cable, hold for 1~2 seconds, then release.
> If firmware crashes during early boot, USB CDC cannot initialize and esptool will fail to connect; this recovery sequence is required in such cases.

### Wi-Fi and Secrets

Wi-Fi configuration requires no code changes: after flashing, navigate to **Settings → Wi-Fi** on the device to scan, select an AP, and enter the password. Connected networks are saved to NVS.

**Wi-Fi is always-on with automatic reconnection**: It connects at boot and remains connected, allowing online pages like Weather / Planes / Sats / Router / Map tiles to be used immediately without waiting 10+ seconds for on-demand connection. When the saved network is out of range, the network stack retries continuously in the background and reconnects automatically upon re-entering coverage. When the onboard SoftAP (Hotspot) is active, Wi-Fi scanning (Settings → Wi-Fi), channel analysis (WiFi Chan), and Wardrive survey operate in ESP32 **AP+STA promiscuous coexistence mode**, **no longer forcing the hotspot off**, keeping connected hotspot clients online; LAN Scan only sweeps the STA subnet, leaving the hotspot unaffected; only Sniffer and Drone ID temporarily suspend the hotspot while monopolizing the RF frontend in promiscuous mode, restoring the hotspot automatically upon exit; after exiting these apps, the watchdog in `loop()` brings the STA connection back up.

The trade-off is that the network stack permanently occupies approximately **36KB of heap** (measured on 2026-08-26 via `STAT`: 72.7KB free when enabled vs. 109.4KB when disabled,
a delta of 36,744 bytes). ⚠️ **The delta remains stable, but absolute free heap decreases as firmware grows**: on 2026-07-30 the same test showed
102KB / 139KB, dropping by ~29KB four weeks later—of which 24.5KB was consumed by static RAM growing from 94,876 to 119,420 bytes
(reported on the final RAM line of `pio run`). Therefore, absolute free heap numbers must always be quoted with a timestamp; only relative deltas are comparable across versions.
Where that 24.5KB went was **untraceable** at the time—`pio run` only reports a aggregate total, while `STAT` / `MEMCAP` only report "what remains right now".
`RAMLOG` is now available: it logs memory consumption step-by-step during boot in a signed table, allowing cross-version comparison by inspecting the first row.
⚠️ **Bluetooth usage is an exception**: The BLE stack occupies ~64KB and cannot coexist with Wi-Fi on this board.
Exiting Bluetooth back to the main menu leaves Wi-Fi disconnected—**it will not recover while idling on the menu**; entering any other
app releases BLE and restores network connectivity (`btReleaseForOtherApps()`, intentionally deferred until that moment to ensure zero overhead when switching between Bluetooth apps).

Because this board has no PSRAM,
the 48KB base map buffer in GNSS Map may fail allocation—it first falls back to 8bpp (24KB, badged `8bit`, zoom remains functional with visible color banding); if neither allocation succeeds, it displays `no zoom: low mem`.

Timezone is configured via `TZ_INFO` in `src/globals.cpp` (defaults to `CST-8`).

Apps requiring API keys need a configured secrets file:

```bash
cp src/secrets.h.example src/secrets.h    # Already in .gitignore, will not be committed
```

| Config Key | Consumer | Behavior if Unset |
|---|---|---|
| `CHAT_BASE_URL` / `CHAT_API_KEY` / `CHAT_MODEL` | ChatGPT page | Error message displayed on query |
| `N2YO_API_KEY` | Sats satellite pass page | Page displays "no N2YO key" (instead of network error) |
| `CLASH_BASE` / `CLASH_SECRET` | Router page | Distinguishes failure modes: incorrect secret reports `auth failed - check CLASH_SECRET`; unreachable server reports `no reply from ...` |
| `GITHUB_USER` | GitHub contribution heatmap | Page prompts that username is not configured (this endpoint requires no token, only username) |

APIs used by Weather, Planes, GNSS Map, Typhoon, Quake, FX, and OKX require no API keys.

---

## Operation

Navigation keys: `;` Up　`.` Down　`,` Left　`/` Right　`Enter` Confirm/Enter　`` ` `` Back

Top **G0 button** (`M5.BtnA`): Wakes up display when asleep · One-key return to main menu inside apps · Advances to next group if already on main menu.

The main menu is a grouped grid, each group fitting exactly one screen (4×2) without scrolling. Left/Right keys navigate within the group or cross boundaries to adjacent groups; the top tab bar indicates the active group.

| Group | Apps |
|---|---|
| **TOOLS** | Time (Clock / Stopwatch + Countdown, two pages) · IMU · Files (includes TXT reader) · Spectrum (Mic FFT) · Calc · Converter · Player (SD card WAV) · **Crypto Suite (AES/RC4/XOR cipher · Caesar/Vigenère/Base64/Hex · Linux Shadow · Cyber Oven Benchmark · Futility Crack challenge)** |
| **SCAN** | WiFi Chan (Channel analyzer) · Sniffer (Promiscuous sniffer) · Wardrive · **Drone ID** (Drone Remote ID scanner) · **BLE Scan** (Device details + Sonar Radar) — **All five rely on promiscuous/scanning modes and require no network connection**, functioning fully offline |
| **NET** | Hotspot (includes QR code join) · LAN Scan · NetProbe · DnsFuzz · Router (Clash/mihomo monitor) · **SSH (Interactive terminal client)** — Functional only with active network (or acting as AP) |
| **HID** | **BT Keys** (BLE keyboard) · **BT Media** (BLE media remote) · Ducky (USB HID) — Originally separated into two groups, unified under HID: **using this device as a keyboard/remote to control host machines**, over BLE or USB |
| **SIG** | LoRa (SX1262 sniffer) · GNSS (6 pages: Pos/Detail/Sats/Config/Speed/Trip & Diag) · **Map** (Amap satellite imagery) · **IR (Infrared remote, TX only)** |
| **SKY** | **Astro (Solar / Lunar / Terminator, three pages, 100% offline)** · Weather (Current / Next 24h / Wind / AQI / 5-Day forecast, five pages) · Planes (ADS-B flight radar) · Sats (Satellite passes) · Typhoon (Typhoon warning + track) · **Quake (Global earthquake bulletin)** |
| **MORE** | ChatGPT · GitHub (Contribution heatmap) · Radio (HTTP MP3 internet radio) · **FX (USD/CNY exchange rate + chart)** · **OKX (USDT/CNY market rate)** · Reader (Standalone bookshelf) · BadApple · Settings |

### Settings

The Settings menu provides 14 system and hardware preference options (stored in NVS, persisting across reboots and power cycles):
- **Wi-Fi**: Scans nearby Wi-Fi APs and connects with password; saved credentials persist in NVS with auto-reconnect on boot.
- **PC MODE**: Enters PC-directed mode (releases 64.8KB full-screen canvas to push status screen directly, allowing host-side cardputer-bridge [companion tool, not yet public] to ingest GNSS / IMU / Wi-Fi telemetry streams; press ` or send serial `PCEXIT` to exit back to Settings).
- **Brightness**: Screen backlight brightness control (0~100%).
- **Volume**: Speaker master volume control (0~100%).
- **Boot sound**: Boot audio toggle (ON / OFF; when enabled, plays a high-fidelity 5-step rising arpeggio; silent when disabled for quiet environments).
- **LED Mode**: Onboard RGB LED indicator mode. Press Enter to cycle through 6 modes: OFF / Battery (default) / Breathe / Rainbow / Chase / Music Reactive. Saved to NVS, persists on boot. Supports temporary override by external features (Spectrum reactive lighting, timer alarm), restoring automatically on exit.
- **UI Theme**: 8 system-wide high-tech HUD color themes (Auto Reactive / Cyan / Green / Amber / Red / Purple / Ice Blue, etc.).
- **Auto sleep**: Idle display timeout without keypresses (Never / 30s / 1m / 3m / 5m).
- **Timezone**: System POSIX timezone configuration (defaults to CST-8).
- **Weather Unit**: Temperature unit selection (Celsius / Fahrenheit).
- **Battery**: Real-time battery voltage, percentage, discharge curve, and calibration status.
- **Debug**: Persistent bottom telemetry status bar toggle (free heap / largest block / Wi-Fi RSSI / voltage / percentage / frame render time).
- **Format SD**: Quick MicroSD card format (with dual-confirmation guard).
- **About**: Firmware version, build timestamp, and hardware resource utilization.

**LED Modes and Override Priority**:
The LED mode selected in Settings serves as the global baseline. When specific applications require the LED, they temporarily acquire control via `ledSetOverride(true)`, overriding baseline effects:
1. **Spectrum Page**: Press `l` to enable microphone-driven LED reactivity, dynamically mapping live audio volume to color pulses.
2. **Countdown Alarm**: When triggered, if the microphone, player, or radio is active (speaker busy), the alarm flashes the red LED as a visual alert.
3. **Priority Protection**: If the Spectrum page has already acquired the LED (reacting to audio), the countdown alarm will not forcibly preempt the LED (preventing premature extinction upon release); the alarm only uses the LED when unassigned. Any override calls `ledSetOverride(false)` upon exit or cancellation, smoothly restoring the baseline LED mode configured in Settings.

When **Settings → Debug** is enabled, a telemetry bar remains pinned to the bottom of the screen (saved to NVS, persisting across reboots):

```
h100k b87k     -49dBm +4126 83%      23ms s2
 ↑Free  ↑LargestBlock ↑WiFi ↑mV   ↑Batt%   ↑FrameRenderTime ↑ScreenID
```

The sign on the battery readings indicates charging estimation (`+` charging / `-` discharging). **When unplugged, serial communication is disconnected,
making this bar the only place to monitor voltage and state-of-charge**—and battery lookup table calibration can only be performed while unplugged
(see the empirical reconstruction curve table in `src/power_util.cpp`).

When the largest continuous block drops below 50KB, it turns amber—this is where the 48KB base map buffer for GNSS Map is allocated; if insufficient, the system falls back to the world map.
This bar covers bottom key hints/page indicator dots and restores them when disabled. It also unthrottles verbose serial logs (such as per-tile map download notices).

### Cyberpunk Boot Sequence & POST

During startup, the firmware executes a complete cyberpunk-themed boot sequence and hardware self-test:
- **Cyberpunk Visuals**: Horizontal laser horizon sweep, tactical HUD corner reticles `┌ ┐ └ ┘`, and micro-dot matrix background.
- **Dynamic Core Topology**: Central hexagonal processor core with pulsing bus traces, paired with a "CARDPUTER" character stream matrix deciphering animation and an "ADV" tactical neon glowing badge.
- **Plasma Charge Rail**: 16-segment plasma charge rail (Emerald -> Cyber Cyan -> Aurora White overload), dynamically refreshing hardware status (`CORE / TCA8418 / AOD / AUDIO / SEC BUS`).
- **Synthesizer Boot Chime**: 5-step rising arpeggio chime (`C5 -> E5 -> G5 -> C6 -> G6 Ready`), synthesized in real-time via `M5.Speaker`; can be muted at any time in `Settings → Boot sound`.
- **Instant Key Skip**: The keyboard controller `kbd::begin()` and speaker are initialized early during boot; **pressing any key or the top G0 physical button interrupts and skips the animation in milliseconds**, entering the main system without latency.
- **Hardware Self-Test (POST AUDIT)**: High-tech dark console HUD automatically auditing the ESP32-S3FN8 core, TCA8418 keyboard matrix, MicroSD card, RTC breadcrumb bus, and network stack at boot.

### Key App Details & Guides

Multi-page apps use `.`/`/` to paginate, with page indicator dots at the bottom. Key bindings and specifics:

- **GNSS (6 Pages)**: ATGM336H navigation and positioning module on the Cap LoRa-1262. Paginate with `.`/`/`:
  - **Page 1 Fix Overview**: Sky view polar plot (center zenith, outer horizon; colored by constellation, point size mapped to SNR), latitude, longitude, altitude, ground speed, etc.
  - **Page 2 Detailed Inspection**: 2D/3D fix status, GGA fix quality indicator (Single/DGPS/RTK Fix/Float/Dead Reckoning, etc.), compact PDOP/HDOP/VDOP dilution of precision, Geoid Separation, in-use vs. visible satellite counts; **press `c` to cycle through 4 coordinate formats** (DEG decimal degrees / DMS degrees-minutes-seconds `dd°mm'ss.s"` / GRID Maidenhead QTH locator / UTM projected zone and metric coordinates).
  - **Page 3 Satellite Signal Table**: Scroll with `[` / `]` to inspect constellation, PRN, SNR, elevation, and azimuth for each satellite; **clearly distinguishes and prefixes in-use satellites (from GSA) versus visible-only satellites**.
  - **Page 4 Module Configuration**: `[` / `]` selects options, `-` / `=` adjusts values, `Enter` applies, `S` saves configuration to module Flash. Supports 5 parameters: Update Rate (1Hz/2Hz/5Hz, default 5Hz), System constellation mix (GPS/BDS/GLONASS, etc.), Dynamic model (Aero <2g / Aero <1g / Vehicle / Portable, defaults to Aero <2g high-dynamics on boot), NMEA output sentence set (full / nav+gsv reduced set, default nav+gsv), and RF antenna power toggle.
  - **Page 5 Speedometer**: Semi-circular analog sports dial with glowing dynamic arc and mechanical needle; **adaptive dynamic scale**: 120 km/h (city cycling/driving) / 360 km/h (high-speed rail/highway; auto-upshifts above 100 km/h) / 1000 km/h (commercial aviation cruise; auto-upshifts above 360 km/h), equipped with 50/100 and 300/360 km/h hysteresis buffers to eliminate jitter at threshold boundaries.
  - **Page 6 Trip & Diagnostics**: Cumulative trip distance (adaptive anti-drift filter, consecutive outlier suppression), total and moving time, max and average speed, TTFF (Time To First Fix) and REACQ re-acquisition latency, in-use/visible satellite counts, motion state (MOVING/IDLE), Top-4 satellite average SNR, and last 2-minute SNR time-series history curve; **press `r` to reset trip statistics and waveform history**.
  - ⚠️ **Boot Optimization and Data Integrity**: Boot sequence automatically configures high-dynamics mode (`PCAS11,6` Aero <2g), 5Hz high update rate (`PCAS02,200`, avoiding 10+ meter lag caused by factory 1Hz during rapid transit), and reduces NMEA sentence output to nav+gsv (`PCAS03`, preserving only GGA/GSA/GSV/RMC to prevent 5Hz dual-constellation streams from overwhelming UART baud rate and RX buffers). Includes a 3.5s delayed fallback retransmission and non-blocking 500ms hot-plug retransmission. Because TinyGPS++ `isValid()` remains permanently true after a single initial fix, the firmware strictly enforces a 3-second freshness check (`GNSS_FIX_STALE_MS = 3000`); speed and heading are trusted only when the fix is fresh and HDOP ≤ 8.0, displaying `n/a` during signal loss or severe multi-path jumps, preventing stationary drift from reporting false speeds.
- **Map**: `[` zoom out, `]` zoom in, five levels: WORLD / z5 / z8 / z11 / z14.
  When satellite positioning is unavailable, it falls back to **IP geolocation** as the map center (top bar badged `ip`), allowing indoor viewing;
  however, **breadcrumbs and the "You Are Here" marker are drawn only when a genuine GPS fix exists**—IP geolocation provides only a city-level estimate;
  rendering it as current location is misleading, and drawing tracks from it is inaccurate.
  ⚠️ Entering this page allocates a 48KB base map buffer (measured via `STAT`: heap drops from 72,632 → 24,132 on entry, and is fully restored on exit).
  It was originally page 5 of GNSS, allocating silently upon paging; moving it to a standalone app makes this allocation an explicit action.
- **LoRa**: Cap LoRa-1262 module driver (SX1262 transceiver), two pages:
  - **Page 1 Spectrum & Telemetry**: 2.4-inch spectrum bar graph and waterfall plot, supporting ISM (863-928MHz) and VHF (161.5-162.5MHz) dual-band toggle (`v`); quad-dial telemetry showing live RF parameters (Freq/BW/SF/CR) and noise floor RSSI/SNR needles; press `a` to toggle "AIS Maritime Watch" with automatic SD card CSV logging.
    ⚠️ **This is pure RSSI energy detection without packet demodulation**—`up` represents rounds where "the AIS channel flashed brighter than the baseline",
    not received maritime AIS packets. This project has not demodulated any AIS packets to date.
    ⚠️ Bottom readings will be occluded by the debug overlay; disable Debug in Settings before taking measurements.
    During logging, data is written to `/ais/YYYYMMDD_HHMMSS.csv`, recording one row every 5 seconds plus one row per UP/DN event,
    with columns `t_s,round,a1,a2,c1,c2,d,bias,spread,thr,ev,lat,lon,alt`. On-screen `up/dn` is computed against
    a local adaptive threshold; **relocating changes the scale, making readings incomparable across locations**; to compare different sites or antennas,
    compare absolute dBm of `a1/a2` and distribution of `a-c` in the CSV. If the header displays `NOLOG` instead of `SD`, logging is inactive.
  - **Page 2 Packet Sniffer & P2P Tactical Terminal**: Real-time over-the-air LoRa packet capture (displays Hex Raw Dump, RSSI, SNR, length), supporting direct typing and transmission of P2P tactical short messages.
- **BLE Scan / BT Keys / BT Media**: Originally three sub-modes under a single Bluetooth app,
  now split into three independent apps (BLE Scan under SCAN, the other two under HID)—they **share only a single BLE initialization**
  and are functionally decoupled. Switching between them incurs **zero overhead**: `btReleaseForOtherApps()` only frees resources
  when "neither on the main menu **nor** on any BLE page", ensuring smooth transitions via the main menu.
  ⚠️ **Reboot after cycling repeatedly between Bluetooth and other apps**. Arduino's BLE library lacks cleanup paths
  (`BLEServer` does not declare a destructor; calling `createServer()` overwrites the previous instance), permanently leaking a full GATT object tree on each transition.
  Measured largest contiguous block drops: `63476 → 23540 → 18420 → 17396 → 10740`;
  beyond this point, `BLEDevice::deinit()` blocks permanently, hanging the device (captured via RTC breadcrumbs,
  hanging inside deinit rather than during destruction). The Bluetooth menu warns when free memory is low:
  amber `restart soon` below 20KB, red `restart before using` below 13KB.
  Attempts to patch the library showed that while memory leaks could be resolved (stabilizing at 18KB free heap without further drop), the deinit hang remained unresolved and was not merged.
  **For the complete investigation, benchmark numbers, and two architectural lessons
  (instrumentation masks bugs / delays only reduce probability without eliminating it), see [docs/ble-teardown.md](docs/ble-teardown.md)**—
  the breadcrumb forensic facility developed during that investigation has been merged into the firmware, accessed via the `TRAIL` command below.
  - **Keyboard mode**: Turns the device into a genuine BLE keyboard, forwarding raw keystrokes to the host.
    - **Modifier Keys**: `ctrl`, `opt`, and `alt` function as true modifiers (`opt` maps to Cmd/Win). Press and hold for standard usage; **a single tap and release latches the modifier**, automatically unlocking after the next keystroke, enabling one-handed Ctrl+C. Four squares on screen: filled = held, outline = latched.
    - **Fn Layer**: `Fn+Tab` = Esc, `Fn+1..0-=` = F1~F12, `Fn+;`/`.`/`,`/`/` = ↑↓←→, `Fn+Backspace` = Delete, `Fn+[`/`]` = Home/End. Tab transmission is supported.
    - **Auto-Repeat**: Begins after 420ms at ~18 keys/sec; active only on this page without affecting menu navigation elsewhere.
    - The middle of the screen features an **echo box** reflecting host line buffer state (backspace deletes characters,
      **Enter clears the buffer**—since the host line has been committed, the new line is empty; preserving the old line would misrepresent state).
      The bottom line displays the last transmitted modifier combo, and the top right tracks cumulative keystrokes. Typing while disconnected shows a red
      `keys dropped` warning, clearly distinguishing "untransmitted" from "transmitted but ignored".
    - **`Fn+Enter` Toggles IME Mode** (indicated by cyan `IME` in top right). Required when using Chinese input methods on the host:
      keys are transmitted in real time and candidate windows pop up normally, but **the echo box becomes desynchronized**—the screen shows
      pinyin while the host shows Hanzi; backspace deletes characters inside the host IME candidate buffer, which the device cannot inspect,
      causing immediate desynchronization once committed. In IME mode, the echo box falls back to a literal "local keystroke stream": backspace records
      `<`, Enter records `|`, without attempting to guess host state; auto-repeat is disabled (repeat backspaces would wipe the entire candidate buffer).
      Disable IME mode for standard English typing for an improved echo experience.
    - ⚠️ **Exit this page using `` Fn+` ``, not bare `` ` ``**—the bare backtick must be preserved for transmission to the host.
      The top G0 physical button also exits. Esc is relocated to `Fn+Tab`.
      To prevent confusion across pages, `` Fn+` `` is equivalent to bare `` ` `` across **all other pages**.
  - **Media remote**: Turns the device into a BLE media remote. `,`/`/` Previous/Next track, `;`/`.` Volume Up/Down, `Enter`
    Play/Pause, `m` Mute, `s` Stop (previously omitted from screen hints, now added to bottom bar).
    Shares the same HID device and pairing session as Keyboard mode.
    ⚠️ When upgrading from older firmware, the host may cache stale HID report descriptors causing media keys to become unresponsive;
    remove ("forget") the device on the host and pair again.
  - **Device details page** also displays Service Data (AD type 0x16). This is distinct from Service UUID; Remote ID (OpenDroneID)
    payloads broadcast here—UUID `0xFFFA` with initial byte `0x0D` highlights green upon detection. Useful for confirming whether a drone broadcasts Remote ID over BLE.
  - **Sonar Radar HUD**: Scan devices → select device `Enter` for details → press `Enter` again to enter radar.
    Tactical 360-degree radar display with rotating beam, continuously tracking target RSSI and plotting time-decay curves; paired with pentatonic proximity audio (pitch increases with proximity; `m` to mute, `r` to reset curve).
    Distance is a **rough estimate** derived from path loss; body shadowing, antenna orientation, and metal furniture can skew estimates by 2x; use only for relative proximity ("closer or farther").
    ⚠️ Tracks by MAC address; devices using randomized addresses (AirPods, AirTags, smartphones) rotate addresses every 15 minutes, losing tracking—return to scan list to re-select (address type is listed on details page).
- **WiFi Chan**: 2.4GHz 13-channel spectrum energy bar graph with peak-hold markers, reporting live AP counts, max RSSI, and channel congestion.
  - `;`/`.` changes selected channel, `r` forces rescan, `a` toggles Auto Scan.
  - `Enter` opens **Channel Detail Inspection** (`SCREEN_WIFI_CHAN_DETAIL`), listing all scanned APs in that channel (SSID, BSSID, RSSI, encryption, OUI vendor identification such as Apple/Huawei/Xiaomi/TP-Link/DJI), helping isolate co-channel interference.
  - **Hotspot Coexistence**: When onboard Hotspot is active, scanning automatically uses ESP32 `WIFI_AP_STA` promiscuous coexistence mode, **no longer forcing the hotspot off**, keeping connected clients online; returns to pure AP mode upon exit.
- **Wardrive**: Wi-Fi war-driving and geographic mapping. Combines Wi-Fi promiscuous mode with the ATGM336H GNSS module on the Cap LoRa-1262 to survey wireless networks while moving.
  - Top HUD displays live GPS fix status, tracked satellites, discovered AP count, and signal quality.
  - Automatically logs standard Wigle CSV to SD card (`/wardrive/YYYYMMDD_HHMMSS.csv`) for import into WiGLE or GIS tools.
  - `Enter` starts/stops single scan, `a` toggles continuous auto-scan, `r` resets statistics.
  - **Hotspot Coexistence**: Operates in `WIFI_AP_STA` mode when hotspot is active, maintaining client connections.
- **Drone ID**: Scans nearby drone Remote ID broadcasts (decodes both ASTM F3411 and GB national standards). Displays ID,
  status (AIR in flight / GND ground / EMG emergency / LOST lost contact), distance, bearing, altitude, and RSSI;
  press `Enter` for details (coordinates, speed, heading, pilot location, registration ID). `;`/`.` selects, `r` clears.
  Once a target is acquired, it **locks to that specific channel**—channel hopping drops ~92% of packets (hopping receives 10~12 packets over 25s, while camping on channel receives 140 packets); channel locking increases packet yield by an order of magnitude; resumes hopping when target contact is lost.
  ⚠️ 2.4G Wi-Fi only: ESP32-S3 lacks 5GHz support, and BLE promiscuous mode requires exclusive RF access incompatible with Wi-Fi promiscuous mode
  (domestic drones typically broadcast over Wi-Fi beacons; for BLE-only broadcasts, fallback to serial `BTDUMP` / `BTEXT`).
  ⚠️ **Drones typically broadcast only when airborne**; stationary drones on the ground generally emit nothing—this indicates an idle drone, not scan failure.
  ⚠️ **Hotspot Suspension**: Because promiscuous mode requires exclusive RF access, the hotspot is automatically suspended (`hotspotSuspend()`) and marked suspended on entry, resuming (`hotspotResume()`) upon exit.
- **Sniffer**: Wi-Fi promiscuous mode over-the-air sniffer and packet counter. Like Drone ID, suspends hotspot during exclusive RF operation, resuming upon exit.
- **Hotspot**: `Enter` toggles AP, `p` changes password, `q` displays QR code (scan with smartphone camera to connect).
  - Maintains connectivity via AP+STA promiscuous coexistence during Wi-Fi scan (Settings), WiFi Chan, and Wardrive;
  - Automatically suspended during RF-exclusive Drone ID / Sniffer runs, restoring seamlessly upon exit; LAN Scan only sweeps STA subnet without impacting hotspot.
- **SSH**: Full-featured interactive SSH terminal client (based on LibSSH).
  - **Zero-PSRAM Memory Self-Healing**: Automatically releases 64.8KB full-screen canvas (`cv.deleteSprite()`) upon establishing an SSH connection, boosting free SRAM to 140KB+ to host a 24KB dedicated thread stack and crypto handshake heap; restores `cv` upon disconnection, ensuring other 40+ system apps continue running. Connect progress renders directly to the display without video memory overhead or flickering.
  - **VT100/ANSI Direct Hardware Terminal with Dynamic Font Scaling**: Direct character rendering via `M5.Display`, providing flicker-free, ultra-fast refresh; supports ANSI 16-color / 256-color / 24-bit true color, absolute/relative cursor positioning, clear screen/line, backspace scrolling, and UTF-8 box-drawing fallbacks. In an active terminal session, **press `Fn + Enter` to cycle through 3 font modes**:
    - `8x16` (30 cols × 8 rows, classic VGA large font, crisp and legible)
    - `8x8` (30 cols × 15 rows, wide medium font, balancing readability and line density)
    - `6x8` (40 cols × 15 rows, compact small font, maximizing width to 40 columns)
    Font switching sends live PTY window size renegotiation (`TIOCSWINSZ` / `ssh_channel_change_pty_size`), allowing htop, vim, and other utilities to reflow layout dynamically.
  - **Authentication: Password & SD Card Private Key Auto-Discovery**: Supports RSA / ED25519 private key authentication in addition to passwords. Scans MicroSD card root, `.ssh/`, and `ssh/` directories (probing 13 common key filenames such as `/id_ed25519`, `/id_rsa`, `/id_ecdsa`, `/ssh.key`), or explicit paths configured via `key=/path/to/key` in `/ssh.cfg` or `/ssh.txt`; for passphrase-protected keys, the password field decrypts the key automatically.
  - **Serial Terminal Pass-Through**: When connected via USB serial, input strings (e.g., `ls`, `pwd`, `fastfetch`) typed into the serial terminal pass directly to the remote SSH shell, excluding system control commands (`STAT`/`MENU`/`REBOOT`/`VOL`, etc.).
  - **Key Mappings**: `Fn + Tab` sends `Esc` (supports vim/nano); `Fn + ; . , /` maps to ANSI arrows `↑ ↓ ← →`; `Fn + [ / ]` sends `Home / End`; `Fn + Backspace` sends `Delete`; `Ctrl + letter` supports combos like `Ctrl+C` / `Ctrl+D` / `Ctrl+Z`; `Fn + \`` or physical `G0` safely disconnects and exits (CAS ownership semantics prevent session/channel double-free).
- **Planes**: `;`/`.` selects aircraft, `r` refreshes.
  ⚠️ All HTTP requests uniformly include `User-Agent: cardputer-adv/1.0` (see `http_json.h`).
  Default requests from Arduino's `ESP32HTTPClient` are blocked by adsb.lol by User-Agent
  (403 `User-Agent too generic; include valid contact info.`).
  **Note that `http.addHeader("User-Agent", ...)` does not work**—Arduino HTTPClient maintains an
  internal managed header list (Connection / User-Agent / Host); addHeader silently ignores matches,
  requiring `setUserAgent()` instead.
- **Sats**: `c` toggles Starlink/GPS, `r` refreshes. Requires `N2YO_API_KEY`.
  Search radius is categorized (Starlink 25° / GPS 90°): visibility requires the satellite subpoint to fall within
  the horizon circle, whose angular radius `acos(Re/(Re+h))` depends strictly on orbital altitude:
  Starlink 550km → 23°, GPS 20,200km → 76°. Using a hardcoded 70° for both would be too narrow for GPS,
  and would pull down 122 Starlink satellites (18KB, mostly sub-horizon and immediately discarded),
  compounding TLS heap pressure and starving heap down to 6.2KB, failing response parsing.
- **Typhoon**: `;`/`.` toggles typhoon (when multiple exist), `r` refreshes; page 2 displays track map.
- **IR**: Infrared remote control. `,`/`/` selects device, `;`/`.` selects button, `Enter` transmits, `p` power sweep
  (transmits Power commands for all devices sequentially), `r` reloads SD.
  ⚠️ Hardware infrared is **transmit only, no receiver** (GPIO44, see [HARDWARE.md](HARDWARE.md)),
  meaning **it cannot learn codes from existing remotes**; codes must come from code tables. Includes built-in profiles for LG, Samsung, and Sony TVs,
  sourced from public code tables and **not verified on physical TVs** (marked `[BUILT-IN]` in top right; SD loaded profiles display `[SD]`).
  Custom remotes can be defined in `/ir/<name>.ir` on the SD card, one button per line:

  ```
  # Hash lines are comments; optional name overrides filename
  name    My TV
  Power   nec32  0x20DF10EF        # NEC timing, 32-bit MSB first — standard public table syntax
  Ch+     nec    0x0408            # High byte addr, low byte cmd; inverted bytes computed automatically
  Mute    sony   0x290 12          # Sony requires bit length (12/15/20)
  Beep    rc5    0x300C
  AcOn    raw    38000 3400,1700,430,1300,430,430
  ```

  Transmission uses **RMT** hardware rather than software bit-banging: with Wi-Fi active, protocol stack interrupts can seize CPU for hundreds of microseconds,
  while IR bit intervals are only 560µs—software timing stretches inevitably, causing intermittent transmission failures.
  RMT is hardware-timed. ⚠️ Uses RMT channel 3, **installing driver only while on this page** and releasing upon exit
  (FastLED driving SK6812 also shares RMT).
  Encoding logic (timings, bit orders, Manchester polarity) can be verified on host: `tools/irtest`.

- **FX**: USD/CNY exchange rate. `;`/`.` navigates records, `r` refreshes, `m` toggles 30-day / 90-day window (saved to NVS).
  Page 1 displays spot rate + daily change + trend chart; page 2 displays daily close and change for each trading day.
  Data sourced from **frankfurter** (European Central Bank reference rates, keyless and unthrottled). Chosen primarily because
  **a single request returns the complete time-series**—plotting 30-day trends avoids 30 individual requests,
  which is critical on this board where each TLS handshake requires 40KB contiguous heap.
  ⚠️ Two semantic caveats: the ECB **publishes on working days only**, so the horizontal axis represents "trading day index"
  rather than calendar days, collapsing weekends; and this is a **reference rate, not a retail bank exchange rate**,
  explaining minor deviations from bank settlement prices.
  Color coding uses **red for up and green for down** (Chinese financial convention), but ▲/▼ and ± signs provide primary indicators.

- **OKX**: USDT → CNY real-time market rate (dual-source high availability: OKX real-time C2C + CoinGecko automatic fallback), `r` manual refresh (auto-refreshes every 5 minutes).
  - **Primary Source (OKX C2C Real-Time)**: Fetches live OKX C2C buy quotes (`v3/c2c/otc-ticker/quotedPrice`), reflecting actual buyer/seller OTC market rates (including live OTC premium).
  - **Fallback Source (CoinGecko Fallback)**: For connectivity blocks, timeouts, or API outages, automatically falls back to CoinGecko's USDT/CNY exchange rate endpoint (stable, accessible without VPN).
  - **UI State Awareness**: Top status bar indicates current active data source (cyan `OKX C2C` or amber `CoinGecko`), subtitle clarifies data nature, and inverse conversion (1 CNY = x USDT) is calculated concurrently.
  - Serial commands allow inspecting raw packets: `OKXDUMP` for primary source, `OKXDUMP FB` for fallback.

- **Radio**: Cyberpunk Hi-Fi streaming internet radio, decoding HTTP MP3 audio streams in real-time.
  - Dark-themed tuner interface with integrated 15-band dynamic audio spectrum analyzer (dynamic FFT visualizer) and master volume meter.
  - Dual gauge displays real-time buffer level waveform, bitrate (kbps), and stream metadata (ICY-Title / Station Name).
  - `;`/`.` switches preset stations (Groove Salad / Lush / Deep Space 1, etc.), `Enter` plays/pauses, `,`/`/` or `-`/`=` adjusts volume.
  - `u` opens custom stream URL terminal (`SCREEN_RADIO_URL`) to enter and resolve external radio streams (Space key auto-fills `http://` prefix).

- **Quake**: USGS public earthquake feed, global coverage, keyless. `;`/`.` selects event, `r` refreshes,
  `m` toggles between feeds (**M2.5+ / 24 Hours** and **M4.5+ / 1 Week**, saved to NVS).
  Page 1 is a list (magnitude / location / time elapsed; details depth, distance, local time for selected item),
  page 2 plots events on a world map, sizing and coloring points by magnitude.
  Deliberately complements Typhoon: JMA reports only **Northwest Pacific** tropical cyclones, whereas Quake covers globally.
  ⚠️ **Does not use `fetchJsonHttp`**: The M2.5+/24h feed frequently exceeds 100 events and 200KB total payload;
  parsing the entire payload into ArduinoJson results in NoMemory crash (as occurred on GitHub page). Rewritten to use streaming element-by-element parsing with
  filters, keeping heap independent of response size. Events with `null` magnitude (recent unclassified events) are skipped.
- **GitHub**: `r` refreshes (cached for 30 minutes).
  ⚠️ **`today` may display `--`**, representing "no data available for today yet in local timezone", not "0 commits today".
  The contribution API window closes at **UTC midnight**, while the device runs local time—in UTC+8 mornings (local date rolled over, UTC has not),
  the local day is outside the API window. It populates once UTC passes midnight.
  (Private repository commits **are** included if "Include private contributions on my profile" is enabled on GitHub.)
- **Spectrum**: 256-point FFT microphone audio spectrum analyzer.
  - **`W` Toggles Waterfall vs. Bar Chart**: Press `W` to switch between **Waterfall (Spectrogram)** and **Classic Bars**. The waterfall uses localized hardware scrolling, 60 dB adaptive dynamic range mapping pre-AGC raw decibels, filters DC components, advances lines only on fresh FFT frames, and applies black→blue→cyan→yellow-green→orange-red→white pseudo-color gradients; bar chart displays 32 frequency bands with white peak-hold indicators.
  - **`l` Toggles Microphone-Driven LED Reactive Mode**: Acquires onboard RGB LED via `ledSetOverride`, dynamically pulsing red/yellow/green with ambient volume.
  - **`s` Toggles Spectrum Streaming** (top bar badged red `REC->serial`): Streams `SPEC {json}` over serial at 5 frames/sec, consumed by `tools/spec_view.py` for ndjson recording / spectrogram rendering, and analyzed offline with `tools/spec_analyze.py`.
    The device performs **no semantic audio analysis**—lacking PSRAM, the chip cannot host audio classification models; but it possesses a battery-powered microphone, whereas host computers have compute power without onboard microphones. The stream transmits raw amplitudes **before automatic gain control (AGC)**: screen AGC scales quiet and loud sounds to full scale, which loses absolute sound pressure levels.
- **Time**: Page 1 Clock, Page 2 Stopwatch / Countdown.
  - **Page 1 Clock**: **Press `f` (or `s`) to cycle through 4 watch faces**: Original (tiered progress bars), Analog (classic watch face, ~12fps smooth sweep hand), Large Digital (7-segment digital display, 500ms blinking colon), and QlockTwo (minimalist English word matrix), saved to NVS and restored on boot.
  - **Page 2 Stopwatch / Countdown**: `m` toggles stopwatch/countdown, `Enter` starts/stops, `l` records lap, `r` resets. Countdown preset adjustment (when stopped): `[`/`]` adjusts ∓1 minute, `-`/`=` adjusts ∓10 seconds (range 10s~99min). ⚠️ **Uses brackets/math keys rather than arrow keys**—this page resides in the navigation chain, where `;.,/` are consumed by page navigation, matching the `[ ] - =` bindings used in GNSS config.
  - **Global Alarm Dismissal**: When the countdown timer reaches zero, pressing any key or the physical G0 button from **any page** immediately silences and stops the alarm without triggering page-specific actions (intercepted early in main loop).
  - **Peripheral Conflict Avoidance & LED Fallback**: Before sounding the alarm, audio state is audited: if the microphone (Spectrum), audio player (Player), or internet radio (Radio) is active, the alarm gracefully falls back to flashing the red LED to avoid bus conflict and distortion; if Spectrum has already acquired the LED, the alarm yields to prevent interrupting LED reactivity.
- **Astro**: Three pages computed entirely locally, **requiring zero bytes of network data** (Terminator requires no location,
  Solar requires only coordinates, falling back to IP geolocation if GPS is unavailable).
  - **Solar**: Sun elevation curve + sunrise, solar noon, sunset, and daylength. Sunrise and sunset are calculated where the sun's center passes
    **−0.833°** (atmospheric refraction 34′ + apparent radius 16′, astronomical standard), not 0°.
    Solar noon and the `max NN°` label are **derived by finding the curve maximum directly**, rather than averaging sunrise and sunset—
    polar day/night has no horizon transitions, and setting a timezone mismatched with local longitude (e.g. UTC while in China)
    prevents sunrise and sunset from pairing on the same calendar day, causing midpoint calculations to fail in both scenarios.
  - **Lunar**: Phase, age, illumination ratio, next full/new moon.
  - **Terminator**: Day/night boundary on world map + current solar declination.
  - Press `r` across all three pages to relocate.
- **Crypto Suite (Password Workshop / Cyber Oven)**: Multi-algorithm cryptography and hashing benchmark suite. Press `m` to cycle through 5 modes:
  - **Mode 0 Benchmark Oven (BENCH OVEN)**: Dual-core stress test featuring high-contrast H/s tachometer, per-hash millisecond latency, dynamic heating coil animation, and battery mA current telemetry; `Enter` starts/stops, `d` toggles Single-core / Dual-core Turbo mode, `r` resets counters.
  - **Mode 1 Symmetric Cipher (SYMMETRIC CIPHER)**: Modern symmetric encryption and decryption. `a` switches algorithm (**AES-128-CBC**, **RC4 (ARC4)**, **XOR (OTP)**); `t` toggles **ENCRYPT** and **DECRYPT**; `;`/`.` cycles plaintext/ciphertext presets, `,`/`/` selects key, outputs live hex ciphertext or restored plaintext, `s` dumps to USB serial.
  - **Mode 2 Classic & CTF Ciphers (CLASSIC & CTF)**: Classical cryptographic and encoding utilities. `a` switches algorithm (**Caesar/ROT**, **Vigenère**, **Base64**, **Hex**); in Caesar mode, `[`/`]` adjusts shift across 1~25 (with ROT13 indicator); Vigenère and Base64/Hex support `t` to toggle Encode/Decode, `s` dumps to serial.
  - **Mode 3 Hash & Shadow (HASH & SHADOW)**: `a` switches hashing algorithm (**$6$ SHA-512-crypt** 5000 rounds, **$1$ MD5-crypt** 1000 rounds, **SHA-256**, **MD5**); live progress bar and elapsed time counter, `Enter` calculates, `s` dumps to serial.
  - **Mode 4 Futility Crack Challenge (FUTILITY CRACK)**: `[`/`]` cycles keyspace (from 4 numeric digits in 12 minutes to 8 alphanumeric characters in 520 million years); `Enter` starts hardware brute-force verification; **all tiers include genuine cracking matches**: candidate generator adapts to character sets (digits / lowercase / alphanumeric), with 5 presets matching targets (0123 / card / cardpt / infinity / entropy9), triggering the `TARGET PWNED!` badge upon match; thread-safe lifecycle management (graceful background worker termination and generation counters discarding in-flight batches, preventing UAF and deadlocks).

---

## Serial Debug Interface

Commands can be entered directly in `pio device monitor` (implemented in `handleSerialCmd` in `src/serial_cmd.cpp`).
Single characters are passed directly to the UI as keypresses, enabling full control of the device without physical keyboard interaction.

| Command | Description |
|---|---|
| `HELP` / `?` | Lists all available commands |
| Single character / `ENTER` `BACK` `UP` `DOWN` | Injected as keypresses into the active screen |
| `BOOTANIM` | Plays cyberpunk boot animation and outputs full-screen RGB565 hex screenshot |
| `BOOTPOST` | Runs hardware self-test (POST AUDIT) console and captures screenshot |
| `BOOTFRAME [t]` | Step-renders boot animation frame at timestamp t milliseconds (t=0..1500) and captures screenshot (frame-by-frame tuning) |
| `BOOTSND [ON\|OFF]` | Queries or configures boot sound preference (stored in NVS) |
| **`SHOT`** / `SHOTRAW` | **Serial screenshot** (RGB565 hex, converted to PNG via `tools/shot.py`); SHOTRAW provides unformatted fast capture |
| `MENU` / `NETPROBE` / `DNSFUZZ` / `GNSSMAP` / `HASHOVEN` | Jumps to specified screen and initializes it |
| `LORA` / `WIFICHAN` / `RADIO` | Jumps directly to corresponding App and initializes associated hardware |
| `BTSCAN` / `BTDEV` / `BTRADAR` | Jumps directly to BLE Scan / Selected device details / Sonar radar |
| `GOTO n` | Jumps to arbitrary screen by `Screen` enum index (debug use: invokes enter only for data-fetching pages, **deliberately skips cleanup**, logging `[jump]`) |
| `GPS` | GNSS telemetry diagnostics: byte counts / checksum pass and fail counts / silence duration / fix status |
| `PROBE host[:port]` | One-shot TCP connectivity probe (diagnoses whether issues stem from network unreachable vs. out-of-memory) |
| `STAT` | Read-only status snapshot: Wi-Fi status / free heap / largest block / loop task stack margin / screen sleep state (`screenOffOnArrival` records whether screen was asleep upon arrival) (unlike `WIFI`, does not trigger auto-connect) |
| `WIFI` / `WIFIOFF` | Connects using NVS credentials / disconnects and disables (suspends watchdog, preventing immediate reconnect) |
| `HGET` / `HPOST` / `TCPHEX` | Raw HTTP / raw TCP requests |
| `CAT path` | Reads SD card file (used primarily for netprobe.log / dnsfuzz.log) |
| `RIDSCAN [sec] [ch]` | Remote ID: promiscuous scan of management frames, **decoding both ASTM ODID and GB national standards** (ID/registration/coordinates/altitude/speed/heading/status/pilot location), outputting raw vendor IE + NAN action frames + AP list. Specifying channel locks to that frequency (NAN auditing requires ch6; domestic drones typically operate on ch6) |
| `BTDUMP [sec]` | BLE sampling: scans and dumps Service Data / Manufacturer Data for all detected devices |
| `BTEXT [sec]` | BLE **Extended Advertising** sampling (BT5 Long Range / Coded PHY); invisible to standard BTDUMP APIs |
| `RFSCAN [sec]` | 2.4GHz per-channel 802.11 activity survey (frame count/peak RSSI/noise floor) to identify clear channels. ⚠️ Cannot detect non-Wi-Fi signals—originally designed to identify DJI O4 video links, but disproven in practice: promiscuous callbacks trigger only after recognizing an 802.11 preamble; proprietary modulations like O4 fail to produce packet events entirely |
| `TRAIL MARK n` | Manually writes a breadcrumb byte (decimal or `0x..`). Used to **verify diagnostic tools**: set a mark → press reset → verify retention during boot replay. Normal traces read as zeros; without this command, verifying that "RTC memory survives soft reset" is impossible |
| `TRAIL` | Prints **breadcrumb trace**—used to diagnose crashes where serial consoles freeze without output. Sprinkles `bcMark(n)` along suspicious paths; after crash and reset, replayed during boot (stored in RTC memory, preserved across soft resets). Identified by **which number fails to appear**. Developed while debugging BLE deinit hangs, where serial logging expanded the race window and masked the defect, detailed in [docs/ble-teardown.md](docs/ble-teardown.md) |
| `LS [path]` | Lists SD directory contents (filename + byte size), defaulting to root. Complements `CAT` when filenames cannot be guessed (e.g. identifying logs in `/battlog`) |
| `BATT [n]` | Samples battery voltage n times (default 64), outputting min/median/max/peak-to-peak/stddev + internal state (ema / charging estimation / level). **Audits all three hardware status queries**: `isCharging()` / `getBatteryCurrent()` / `getVBUSVoltage()`—all three return invalid values on this board (unknown / 0 / -1), confirming battery state must be derived from voltage |
| `BATTLOG ON\|OFF` | Logs battery curve to SD `/battlog/*.csv` every 10 seconds. **Toggle stored in NVS, surviving reboots**—its sole purpose is capturing what happens after the USB cable is unplugged, when serial commands cannot be sent. ⚠️ Must be enabled **before disconnecting USB** |
| `RAMLOG` | **Boot-phase memory timeline**, including step-by-step deltas. While `STAT`/`MEMCAP` answer "how much memory remains now", `RAMLOG` answers "**which step** consumed it"—explaining where the aforementioned 24.5KB static RAM increase originated. ⚠️ Buffered in RAM and dumped on demand rather than printed live: `Serial.begin()` is initialized late in `setup()`, while `M5.begin()` and the 64KB main canvas precede it; live printing would miss the two most critical early allocations |
| `MEMCAP` | Segmented heap audit (8BIT/INTERNAL/DMA/32BIT) + **fragmentation layout** (free blocks count vs. allocated blocks count). The debug bar's largestBlock reflects only 8BIT; free/largest metrics cannot distinguish "leaks" from "pure fragmentation", whereas block counts do |
| `RIDFAKE` | Injects two real captured Remote ID frames into Drone ID app to verify UI rendering without drones present. Injected entries display magenta `FAKE` badges + top banner to distinguish from live detection |
| `RANDMAC` / `SETMAC` | Modifies STA MAC address |
| `FONT [12\|14\|16]` | Changes TXT reader font size |

---

## Host Tools

See [tools/README.md](tools/README.md) for full documentation. Eight tools:

- **`tools/shot.py`** — Serial screenshot tool saving directly to PNG. When connected, this is the primary way to inspect the display for UI tuning:
  `python3 tools/shot.py --cmd GNSSMAP --scale 3`
- **`tools/uisim/`** — Desktop UI simulator that **compiles the actual rendering code in `src/` directly**, rendering offscreen PNGs on host computers.
  Used to verify layouts without physical hardware; runs hardware-independent pages.
- **`tools/lora_view.py`** — Real-time tabular viewer for LoRa packets.
- **`tools/spec_view.py`** — Acoustic capture: streams FFT audio data from the Spectrum app to host, logging ndjson and
  rendering spectrograms (zero third-party dependencies; PNG encoder is implemented in pure Python). `--render` re-renders existing logs.
- **`tools/spec_analyze.py`** — Acoustic analysis: reads ndjson captured by `spec_view.py` for semantic evaluation—noise baseline,
  continuous acoustic sources, narrowband tones (whistles/hums and frequencies), environmental classification, time-slice segmentation; optional `--json`/`--png` (graduated spectrograms). Depends on numpy.
  ⚠️ Firmware streams at 5 frames/sec with 16ms window (~7% duty cycle), suitable for assessing ambient acoustic character and continuous sources, but incapable of capturing single transient events, speech content, or 10~25Hz rotor modulation (see script docstring).
- **`tools/rid_view.py`** — Real-time drone Remote ID viewer. When entering Drone ID app, firmware streams
  `RIDPKT {json}`; this script formats tabular output and supports `--snapshot` to maintain a continuously updated state file.
  **Designed to eliminate latency**: serial `RIDSCAN` blocks for N seconds before printing, creating blind spots during rapid drone movement.
  Streaming output allows inspecting current state at any moment via snapshot.
- **`tools/odidtest/`** — Remote ID decoder test harness, compiling real code from `src/`.
  Without real broadcast drones on hand, parser field offsets are prone to subtle bugs (a one-byte offset compiles and parses plausible coordinates); testing compares known inputs against verified outputs.
- **`tools/powertest/`** — Battery and charging logic test harness, compiling real code from `src/`.
  Ensures verification of "unplugged" branches that cannot be tested while connected to USB serial.
- **`tools/ramtest/`** — Test harness for `RAMLOG`, compiling `src/ram_profile.cpp` against mock `heap_caps_*` data
  (including memory reclamation to verify positive deltas). Verifies column alignment, sign rendering, and circular buffer eviction.
  Run with `-Werror=format` in uisim to detect format string mismatches that would otherwise fail silently on MCU hardware.

---

## Lessons Learned & Pitfalls

This board has no PSRAM, and the main canvas consumes 64KB of heap alone; **memory constraints are the root cause of most issues**.
The following lessons were uncovered through physical debugging and are documented in code comments:

**G38 is shared between backlight and RGB LED power.** M5GFX controls the backlight on G38 via LEDC PWM; manual
`pinMode`/`digitalWrite` on G38 forces the backlight permanently on, breaking brightness adjustment and auto-sleep,
causing the device to appear "frozen with screen on". Drive only data pin G21 for the LED. See [HARDWARE.md](HARDWARE.md).

**GNSS UART RX buffer must be enlarged.** The default 256-byte buffer holds only 22ms of data at 115200 baud, while a single main loop frame
(render + `delay(20)`) takes 30~50ms—causing dropped bytes and truncated NMEA sentences every frame.
`setRxBufferSize(4096)` must be called **before** `begin()`.

**PNG cannot be decoded on this board.** Deflate decompression requires a 32KB contiguous buffer for its LZ77 sliding window (format requirement that cannot be reduced),
while measured free heap was 42KB with largestBlock at only 24KB. GNSS Map uses **Amap satellite imagery (JPEG)**
rather than street maps (PNG only)—tjpgd decodes JPEG MCU blocks in streams, requiring only a few kilobytes.

**Do not move large buffers from heap to `.bss` to save heap.** Both reside in the same physical DRAM; moving buffers to `.bss` makes allocations permanent,
shrinking the heap pool and dropping measured largestBlock from 24KB to 16KB.

**Do not malloc entire compressed image payloads before decoding.** z8/z11 tiles range from 26~35KB, while largestBlock may only have 11KB available,
causing malloc failures and half-rendered images. Use `drawJpg(Stream*)` to stream and decode directly from HTTP responses.

**Heap defragmentation cannot be performed on this board, but is rarely what is actually needed.** Defragmentation requires relocating allocated memory blocks,
which in turn requires updating all pointers referencing them—a technique requiring handle-based allocation (as in legacy Mac OS / Palm OS) or a precise garbage collector.
However, mbedTLS, Arduino String, ArduinoJson, and the BLE stack all hold raw pointers from malloc; because there is no mechanism to track pointer ownership, blocks cannot be moved.
This is an inherent property of C heap allocation, not a defect in ESP-IDF.

Analyzing block counts via `MEMCAP` distinguishes between three memory failure modes:

| Symptom | Conclusion | Investigation Focus |
|---|---|---|
| Total free memory does not recover | True memory leak | Identify missing free / destructor calls |
| Total free memory recovers; free block count rises; **allocated block count rises** | Fragmentation caused by leaks | Identify which allocations create orphaned blocks |
| Total free memory recovers; free block count rises; allocated block count unchanged | Pure fragmentation | Allocation order; isolate long-lived medium objects |

Measured across 2 Bluetooth cycles: free memory changed 73460 → 80032 (**actually increased**), largest block dropped 63476 → 18420,
free blocks rose 10 → 37, allocated blocks rose **401 → 593**. The 192 orphaned blocks were never freed, fragmenting contiguous memory like wedges—
confirming the second scenario (leak-induced fragmentation). Fixing destructors caused largestBlock to stabilize rather than continually decay.

Viable mitigations: ① Eliminate leaks; ② Allocate large, long-lived buffers early (the 64KB main canvas allocated at boot is never freed, anchoring the base of the heap without participating in fragmentation); ③ Implement degradation tiers (Map falls back 48KB → 8bpp 24KB → offline world map); ④ Reboot—a valid recovery strategy on MCUs.

**HTTPS handshake peak consumes nearly 70KB, while total free heap is only ~74KB.** Measured on 2026-08-09 (ChatGPT page):
`minEver` before handshake was 70,284 bytes; after handshake failure, `minEver` dropped to 4,236 bytes—**draining the heap to 4.2KB**,
failing even the 128-byte DMA buffer allocation for SHA (`esp-sha: Failed to allocate buf memory`).
Suspected DMA pool starvation was ruled out via `MEMCAP`: DMA shares the 8BIT pool, both having 63KB contiguous blocks post-reset.
The actual culprit is two 16KB I/O buffers from `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384` combined with temporary certificate chain objects.
Consequently, the ChatGPT page remains **marginally usable**: occasional successes, frequent failures. Tuning this configuration requires rebuilding the framework (Arduino mbedtls is precompiled); the fundamental fix is reducing baseline memory during requests.

**TLS incurs significant memory and latency costs.** mbedTLS handshakes require 40~50KB of contiguous heap, which cannot be satisfied on the Map page
(`start_ssl_client: -1`, despite `PROBE 443` succeeding in 8ms); handshakes also take hundreds of milliseconds.
Therefore, **public data endpoints carrying no credentials** use cleartext HTTP: map tiles, ADS-B, weather, IP geolocation.
Switching ADS-B to cleartext prevented largestBlock from collapsing from 90KB down to 47KB during handshakes.
**HTTPS is retained in four locations**: 1. Sats—N2YO includes the API key in the URL, where cleartext would expose the key over the wire;
2. ADS-B **route lookups** (adsbdb.com, airports + progress); 3. **Typhoon** (jma.go.jp); 4. **Quake** (earthquake.usgs.gov).
The latter three enforce HTTPS via 301 redirects, transmitting no credentials. Typhoon and ADS-B route lookups trigger only when selecting specific items,
with payloads under a few KB, making handshake costs acceptable. Quake payloads are much larger, and are processed using **streaming element-by-element parsing**—accepting handshake overhead while avoiding buffering complete JSON payloads (see `src/quake.h`).

> ### Security Boundaries (Documented explicitly, not just performance trade-offs)
>
> This is an experimental hobby project with three known, intentional security trade-offs:
>
> 1. **Cleartext HTTP responses can be tampered with.** Map tiles, ADS-B, weather, AQI, and IP geolocation use cleartext HTTP.
>    These endpoints transmit no credentials, presenting zero credential leakage risk; however, adversaries on the local network can spoof responses,
>    displaying incorrect locations, weather, or maps. Acceptable for this device's application—**but this is an integrity risk, not merely a performance optimization**.
>
>    **Exception: The Router page uses cleartext while carrying credentials.** `CLASH_BASE` uses `http://`, and polling requests transmit
>    `Authorization: Bearer <CLASH_SECRET>` (`router.cpp`). While this secret is restricted to the local LAN to read router metrics,
>    the assertion that "cleartext endpoints carry no credentials" does not apply here; treat it with appropriate caution.
> 2. **HTTPS enforces full certificate verification (since 2026-08-26).** Earlier versions of `sats.cpp`, `github.cpp`,
>    ADS-B route queries, and `typhoon.cpp` used `client.setInsecure()`, preventing passive eavesdropping but vulnerable to active MITM attacks—
>    **particularly concerning for Sats**, where N2YO API keys in URLs could be harvested with self-signed certificates.
>
>    This is now resolved using a **Mozilla root CA bundle**: 121 certificates compiled directly into flash (~56KB; see
>    `platformio.ini` `board_build.embed_files` and `src/tls_ca.h`). RAM overhead is minimal, with only an index resident in memory.
>
>    ChatGPT page provides three verification tiers: explicit `CHAT_ROOT_CA` pins certificates; otherwise defaults to full bundle verification
>    (**current default, eliminating manual certificate entry**); setting `CHAT_ALLOW_INSECURE` to `true` allows insecure fallback with serial warnings.
>    Compiling a full CA bundle avoids hardcoding individual certificates that would brick devices upon expiration.
>
>    ⚠️ **Cost: Certificate chain validation incurs additional heap spikes.** The GitHub page previously failed under concurrent pressure—handling
>    TLS buffers, validation spikes, and a full-year 15.5KB JSON payload simultaneously, reporting `json: NoMemory` with
>    `STAT` showing `minEver=584` bytes. Streaming JSON parsing brought `minEver` up to 3,948 bytes, functional but tight.
>    **This page acts as a memory pressure canary for TLS**—if it fails again with NoMemory, consider tuning `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`
>    (asymmetric 16KB RX / 2KB TX buffers, saving ~14KB, requiring framework rebuild).
>
>    ⚠️ **New Failure Mode: Certificate validation checks validity periods, requiring synchronized time.** During early boot, NTP runs
>    asynchronously; while the clock rests at 1970, certificates are rejected as not yet valid. Affected pages include
>    `tlsClockReady()` guards, reporting `waiting for clock (NTP)` rather than failing with confusing network errors.
> 3. **Keys in `src/secrets.h` are compiled into the firmware binary.** While the file is gitignored and absent from commit history,
>    compiled binaries (`.pio/build/*/firmware.bin`) contain keys in plaintext—**do not distribute compiled binaries to third parties**;
>    rebuild with placeholder keys before sharing.

**A single HTTPS handshake permanently fragments the heap, penalizing subsequent large allocations.** Measured via `STAT`: cold boot
`largestBlock` is 94KB, allowing GNSS Map to allocate its 48KB base map buffer; **after a single TLS handshake**
(Sats / Typhoon / Chat), `largestBlock` permanently drops to 41~47KB—despite total free heap remaining nearly unchanged
(100KB → 98KB); the loss is **contiguity**, not capacity. The 48,480-byte allocation threshold sits just above this level,
causing map zooming to fail mysteriously after viewing Sats. Large buffers must implement fallback tiers (Map falls back to 8bpp),
and **degradations must be communicated in the UI**—the original code reset zoom level to 0 on allocation failure while leaving the label at `z8`,
rendering a world map with a `z8` label where pressing `[ ]` updated only labels, resembling a broken feature.

**Do not perform synchronous network requests in key handlers or render loops.** `handleKey` and `render` run inside `loop()`;
HTTPS requests take ~1 second, during which keyboard polling halts—causing perceptible stutter when opening pages.
Entering and refreshing Planes/Sats/Router was refactored to push a loading frame first, delegating the network fetch to subsequent `update()` calls in `loop()`.

**Time-consuming operations must not reside in the render path.** `render()` is invoked from `loop()`; performing synchronous network calls within it
blocks the main loop, halting keyboard polling and giving the impression of a complete system hang. Tile fetching was converted to a state machine, advancing at most one step per frame.

**16-bit `TFT_*` macros and 24-bit hex literals must not be mixed in the same return type.** LGFX interprets `uint32_t` as RGB888,
causing `TFT_WHITE` (0xFFFF) to render as cyan and `TFT_CYAN` to render as blue. Use either `uint16_t` + `color565()` consistently, or 24-bit color throughout.

**Mainland China Network Reachability:** Carto and OpenStreetMap tile endpoints timed out over TCP 443 in testing, while Amap connected in 8ms.
Amap uses GCJ-02 coordinates, while GPS produces WGS-84 (a 300~600m discrepancy), requiring runtime conversion (`wgs2gcj()`).

**TinyGPS++ `isValid()` does not indicate "currently valid fix".** Once TinyGPS++ successfully decodes a single coordinate,
`gps.location.isValid()` remains true indefinitely. Entering tunnels or indoor areas with total signal loss does not reset it to false. Relying on it as a validity check causes pages to display stale coordinates; worse, weak indoor multi-path signals cause HDOP to spike, generating wild 100-meter coordinate jumps and reporting phantom speeds of 30~40 km/h while resting stationary on a desk. Dual safeguards are required:
① Check fix freshness (`location.age() < 3000`); ② Treat speed and heading as trusted only when fix is fresh and `HDOP <= 8.0` (`gnssSpeedTrusted()`),
displaying `n/a` otherwise.

**HTTP/1.1 Chunked Transfer Encoding causes severe memory pressure on non-PSRAM MCUs.** If an upstream server responds with
`Transfer-Encoding: chunked`, the client must maintain chunk decoding buffers in memory. For streaming responses like ChatGPT, this consumes an extra 12KB+ of dynamic heap,
triggering OOM during TLS handshakes. The fix is explicitly issuing `HTTP/1.0` request headers, requiring servers to disable chunked encoding and transmit with explicit Content-Length, eliminating chunk buffers.

**Wi-Fi Mode Switching Timing Windows and Promiscuous Exclusivity.** Mode changes (`WiFi.mode()`) in ESP-IDF are asynchronous;
callers must await `WiFi.getMode()` confirmation and ensure protocol state bits are ready (`waitStatusBits(STA_STARTED_BIT)`), otherwise immediate calls to `scanNetworks()` fail silently.
Furthermore, enabling promiscuous mode via `esp_wifi_set_promiscuous(true)` (Drone ID / Sniffer) monopolizes the RF frontend, conflicting with SoftAP and STA modes.
The hotspot must be suspended via `hotspotSuspend()` prior to entering promiscuous mode and restored via `hotspotResume()` upon exit; standard scans (WiFi Chan / Wardrive)
can safely use `WIFI_AP_STA` mode to coexist with the hotspot.

**I2S Audio and Microphone Bus Exclusivity.** Switching the ES8311 codec between recording (Spectrum) and playback (Player / Radio) involves analog power transients
prone to DC pop noise; more critically, the hardware audio bus can be owned by only one subsystem at a time. If the background countdown alarm calls `Speaker.tone()` while the radio or player is active,
it causes bus contention, distortion, or task deadlocks. The countdown alarm must check peripheral state via `isAudioBusy()` prior to sounding, gracefully falling back to red LED flashing without contending for the audio bus.

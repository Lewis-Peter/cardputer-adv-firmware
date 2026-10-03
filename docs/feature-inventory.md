**English** | [简体中文](feature-inventory.zh-CN.md)

# Feature Inventory (Cardputer ADV Firmware)

Extracted directly from source code, not copied from the README. Scope:
**40 apps / 85 Screens / 7 groups**.

Terminology:
- **Subpage** = Items linked in `pages.cpp` pagination carousel, flipped via `.`/`/`, with page indicator dots at bottom
- **Submode** = Different operational states within the same Screen (does not consume Screen slots)
- **Subscreen** = Dedicated Screen not in a pagination chain (entered via Enter, returned via `` ` ``)
- Universal keys omitted: `;.,/` navigation, `Enter` confirm, `` ` `` back, `G0` return to main menu

---

## Current Groups & Margins

| Group | Used | Margin | Apps | Categorization Rationale |
|---|---|---|---|---|
| **TOOLS** | 8/8 | **0** | Time · IMU · Files · Spectrum · Calc · Converter · Player · Hash Oven | Local utility tools, no RF dependencies |
| **SCAN** | 5/8 | 3 | WiFi Chan · Sniffer · Wardrive · Drone ID · BLE Scan | Passive discovery (promiscuous mode / scanning, connection not required) |
| **NET** | 5/8 | 3 | Hotspot · LAN Scan · NetProbe · Router · SSH | Active networking (requires active connection or operating as AP) |
| **HID** | 3/8 | 5 | BT Keys · BT Media · Ducky | Device acting as keyboard / remote controller to control host (BLE/USB) |
| **SIG** | 4/8 | 4 | LoRa · GNSS · Map · IR | Other RF transceiver channels (over-the-air signals and positioning) |
| **SKY** | 6/8 | 2 | Astro · Weather · Planes · Sats · Typhoon · Quake | Skyward observation (astronomy, meteorology, hazards, aerial traffic) |
| **MORE** | 8/8 | **0** | ChatGPT · GitHub · Radio · FX · OKX · Reader · BadApple · Settings | Personal dashboards, multimedia, and system configuration |

⚠️ **Hard limit of 8 apps per group (4×2 grid; each group fits on a single screen without vertical scrolling)**. TOOLS and MORE are currently at full capacity (8/8).
Total capacity: 56; currently used: 39; spare slots distributed across SCAN (3) / NET (3) / HID (5) / SIG (4) / SKY (2).

---

## TOOLS (Local, No RF Required)

### Time — 2 Subpages (Merged Clock + Timer)
1. **Clock** (`SCREEN_CLOCK`): Running clock + calendar. **Press `f` (or `s`) to cycle between 4 face styles**: Original (hierarchical progress bar), Analog (classic watch face, smooth ~12fps sweep second hand), Big Digital (7-segment digits, 500ms blinking colon), and QlockTwo Text (minimalist English word matrix highlight). Selection persists automatically to NVS across boots.
2. **Stopwatch / Timer** (`SCREEN_STOPWATCH`):
   - `m` toggles between **Stopwatch / Timer**
   - `Enter` starts/stops · `l` records lap (Stopwatch, up to `LAP_MAX`) · `r` resets to zero
   - Timer preset adjustment when idle: `[`/`]` ±1 minute, `-`/`=` ±10 seconds, range 10s–99min (⚠️ distinct from arrow keys, which are consumed by page pagination)
   - **Global Safe Alarm Mute**: While ringing, pressing any key or physical G0 on **any screen** mutes the alarm immediately without triggering the underlying key action
   - **Peripheral Avoidance & LED Alert**: When microphone/player/radio is active, the alarm falls back to a flashing red LED alert instead of using the speaker to prevent audio conflicts and clipping; if the spectrum page has claimed the LED, the alarm yields gracefully without overriding
   - Preserves timer state on exit (timed against `millis()`), continuing in background

### IMU — 2 Subpages
1. **Compass**
2. **Details** (`SCREEN_COMPASS_DETAIL`)

### Files — 1 Screen + 1 Subscreen
- Directory browsing, entering/exiting folders, `` ` `` steps up one level (preserves file list and cursor position without re-scanning directory)
- `Enter` opens `.txt` → **Reader** subscreen (`SCREEN_READER`)
- `Backspace` deletes files (with confirmation dialog)
- Reader controls: `.`/`/` page turn, `[`/`]` jump ~5% back/forward, auto-saves reading progress on exit

### Spectrum — 1 Screen
- 256-point FFT audio spectrum analyzer via microphone
- **`W` toggles Waterfall and Bar charts**: Press `W` to switch between **Spectrogram (Waterfall)** and **Classic Bars**. Waterfall utilizes partial-screen hardware scrolling, mapping raw pre-AGC decibels across a 60 dB adaptive dynamic range, filtering DC offset, and smoothly advancing rows only when new FFT frames arrive, using a dark → blue → cyan → green-yellow → orange-red → white pseudo-color palette. Bar mode renders 32 frequency bands with white peak-hold indicators.
- `l` toggles "Microphone-driven RGB LED sync" (takes over on-board RGB LED via `ledSetOverride`, mapping dynamic color transitions to acoustic intensity)
- `s` streams **spectrum data over serial** (`SPEC {json}`, 5 frames/sec), companion to `tools/spec_view.py` and `spec_analyze.py`

### Calc — 1 Screen
- Expression calculator; `Enter` or `=` evaluates; `Backspace` deletes (clears evaluated result when expression is empty)
- Typing operators after evaluation continues calculation from the prior answer; typing digits starts a new calculation

### Converter — 1 Screen
- `,`/`/` switches category · `;`/`.` switches source unit · `[`/`]` switches target unit
- Numeric entry; `.` acts as decimal point when input is present, or switches units when empty (context-sensitive key handling)

### Player — 1 Screen
- SD card WAV player; list navigation, `Enter` plays/pauses
- Dynamic buffer queue space timeout calculation (prevents premature playback termination on 8kHz low-bitrate streams); cooperative shutdown prevents FATFS deadlocks; supports volume adjustment, auto-track progression, and real-time VU meter

### Hash Oven — 1 Screen (Crypto Workshop / Cyber Hand Warmer)
- Multi-algorithm cryptography and encryption/decryption experiment toolkit; press `m` to cycle through 5 submodes:
  - **Mode 0: Compute Benchmark (BENCH OVEN)**: Dual-core full-load stress test with prominent real-time H/s tachometer, per-hash millisecond latency, animated heating filament graphics, and battery mA telemetry. `Enter` starts/stops, `d` toggles single-core / dual-core Turbo, `r` resets.
  - **Mode 1: Symmetric Ciphers (SYMMETRIC CIPHER)**: Modern symmetric encryption/decryption. `a` selects algorithm (**AES-128-CBC**, **RC4 (ARC4)**, **XOR (OTP)**); `t` toggles **ENCRYPT** vs **DECRYPT**; `;`/`.` cycles through plaintext/ciphertext presets, `,`/`/` cycles keys; displays real-time hex ciphertext or recovered plaintext; `s` dumps output over USB serial.
  - **Mode 2: Classic & CTF Ciphers (CLASSIC & CTF)**: Classical ciphers and encoding tools. `a` selects algorithm (**Caesar/ROT**, **Vigenère**, **Base64**, **Hex**); in Caesar mode, `[`/`]` continuously shifts alphabet offsets from 1–25 (highlights ROT13 automatically); Vigenère and Base64/Hex support `t` to toggle Encode/Decode; `s` dumps to serial.
  - **Mode 3: Password Hashes & Shadow (HASH & SHADOW)**: `a` selects hash algorithm (**$6$ SHA-512-crypt** 5000 rounds, **$1$ MD5-crypt** 1000 rounds, **SHA-256**, **MD5**); real-time progress bar and elapsed duration; `Enter` computes; `s` dumps to serial.
  - **Mode 4: Futility Cracker (FUTILITY CRACK)**: `[`/`]` selects keyspace scale (from 4-digit PINs at 12 minutes to 8-character mixed alphanumeric at 520 million years); `Enter` starts real-time brute-force testing on device. **Every tier features genuine match verification**: underlying candidate generation adapts to the character set (digits/lowercase/alphanumeric), and all 5 preset tiers support authentic solution matching (`0123`, `card`, `cardpt`, `infinity`, `entropy9`), unlocking a celebratory `TARGET PWNED!` badge upon discovery rather than acting as a static demo; task lifecycle safety ensures clean shutdown via coroutine generation counters, preventing UAF and deadlocks.

---

## NETWORK (Wi-Fi RF Transceiver & Network)

### WiFi Chan — 1 Screen + 1 Subscreen
- Channel congestion analyzer; `r` rescans, `a` toggles auto-scan
- `Enter` → **Channel Details** subscreen (`SCREEN_WIFI_CHAN_DETAIL`; allows navigating channels via `;`/`.` while viewing details)
- **Hotspot Coexistence**: When on-board AP mode (Hotspot) is active, scanning automatically employs ESP32's `WIFI_AP_STA` hybrid coexistence mode **without disconnecting active clients**; reverts to pure AP mode upon exit

### Sniffer — 1 Screen
- Promiscuous-mode packet sniffer and statistics; `r` clears counters
- **Hotspot Suspension**: While promiscuous mode claims exclusive RF access, Hotspot is suspended via `hotspotSuspend()` and resumes automatically upon exit via `hotspotResume()`

### Hotspot — 1 Screen + 2 Subscreens
- `Enter` toggles AP · `p` → **Password Edit** subscreen (`SCREEN_HOTSPOT_PW`, requires ≥8 chars) · `q` → **QR Code** subscreen (`SCREEN_HOTSPOT_QR`, enables AP automatically upon entry)
- Main screen displays live connected client DHCP leases with IP and MAC addresses
- **Coexistence and Auto-Resume**: Coexists online with Wi-Fi scanning (Settings), WiFi Chan, and Wardrive in `WIFI_AP_STA` mode without dropping clients; suspends cleanly during exclusive RF modes (Drone ID / Sniffer); LAN Scan runs across STA subnets without affecting AP mode

### LAN Scan — 1 Screen
- Local network host discovery (ARP cache + ICMP Ping sweep across connected STA subnet) + reverse DNS / mDNS resolution for hostnames
- `Enter` rescans, list navigates up/down
- **Non-interfering with Hotspot**: Operates exclusively over the connected STA interface without requiring exclusive promiscuous RF mode; Hotspot remains active

### NetProbe — 1 Screen (Two Modes) + Edit Submode
- Unified network reachability multi-target probe and DNS stress/fuzz tester; press `m`/`M`/`Tab` to switch between modes:
  - **Probe Mode**: Polls reachability across TCP/DNS/HTTP endpoints (Google/Baidu/Aliyun/Cloudflare/VPN node/gstatic 204), detecting AS32934 DNS poisoning and captive portals to derive overall connectivity status. Logs results to SD `/netprobe.log`.
  - **DNS Mode**: Injects 10 RFC-compliant malformed query types against target DNS resolvers (defaults to DHCP DNS/gateway, `g` realigns to default gateway): cyclic compression pointers, truncated labels, oversized names, illegal reserved bits, forged EDNS0, multiple questions, malformed headers, empty packets, TCP baseline, and length mismatches. Measures RTT latency via controlled queries; automatically halts (`HALTED`) if targets become unresponsive, resumable via `Enter`. Logs to SD `/dnsfuzz.log`.
- Key shortcuts:
  - `Enter`: Re-runs current mode; resumes DNS mode when halted
  - `m` / `M` / `Tab`: Toggles between Probe and DNS modes
  - `a` / `A`: (Probe mode only) Toggles periodic auto-poll (runs every 5 minutes); DNS mode deliberately omits auto-polling to avoid unattended fuzzing of household routers
  - `e` / `E`: Edits target for current mode (Probe edits VPN node, DNS edits DNS IP/port; two-stage prompt, `` ` `` cancels)
  - `g` / `G`: (DNS mode only) One-click reset to default network DNS/gateway
  - `` ` ``: Returns to main menu

### Drone ID — 1 Screen + 1 Subscreen
- Drone Remote ID scanner (dual decoders: ASTM F3411 + Chinese National Standard GB)
- List view: ID / Flight Status (AIR/GND/EMG/LOST) / Range & Bearing / Altitude / RSSI
- `;`/`.` selects · `r` clears · `Enter` → **Details** subscreen (coordinates, speed, heading, operator location, registration)
- Locks channel upon target discovery (channel hopping drops ~92% of frames)
- Streams `RIDPKT {json}` over serial, compatible with `tools/rid_view.py`
- **Hotspot Suspension**: Promiscuous capture requires exclusive RF access; suspends Hotspot upon entry via `hotspotSuspend()` and restores it upon exit via `hotspotResume()`

### SSH — 1 Screen + Terminal Submode
- Full-featured interactive SSH terminal client (based on LibSSH)
- **PSRAM-Free Memory Reclamation**: Automatically deallocates the 64.8KB full-screen canvas (`cv.deleteSprite()`) upon establishing connection, expanding free internal SRAM to >140KB to accommodate 24KB thread stacks and cryptographic handshake allocations; canvas restores seamlessly upon disconnection
- **Hardware Direct-Draw VT100/ANSI Terminal**: Directly paints character glyphs to `M5.Display` without intermediate framebuffers, delivering flicker-free fast updates; supports ANSI 16-color, 256-color, 24-bit TrueColor, absolute/relative cursor addressing, line/screen clears, scrollback, and UTF-8 box-drawing fallbacks
- **Font Size Switching & Dynamic PTY Renegotiation**: Press `Fn + Enter` inside an active terminal session to cycle through 3 font modes:
  - `8x16` (30 cols × 8 rows, classic VGA large font, highly readable)
  - `8x8` (30 cols × 15 rows, wide font balancing clarity with row count)
  - `6x8` (40 cols × 15 rows, compact font maximizing terminal width to 40 columns)
  Renegotiates PTY window dimensions with the remote server in real time (`TIOCSWINSZ` / `ssh_channel_change_pty_size`), ensuring terminal tools like htop and vim reformat cleanly
- **Authentication**: Supports password authentication and automatic discovery of private keys (RSA/ED25519) on SD cards (scans 13 common paths such as `/id_ed25519`, `/id_rsa`, `/ssh.key`, or specified via `/ssh.cfg`; supports key passphrases)
- **Serial Terminal Passthrough**: When connected via USB serial, terminal shell input passes directly to the remote SSH session alongside standard system management commands
- **Keybindings & Connection Safety**: `Fn + Tab` = Esc, `Fn + ; . , /` = Arrow keys, `Fn + [ / ]` = Home/End, `Fn + Backspace` = Delete, `Ctrl + Letter` combinations; exit anytime via `Fn + \`` or physical `G0` (guarded by CAS ownership checks preventing double-free panics)

---

## SIGNAL (Non-Wi-Fi Transceiver Channels)

### LoRa — 4 Submodes + 1 Guard Substate (All in Single Screen)
| Mode | Purpose | Activation |
|---|---|---|
| **SCAN** | Frequency sweep to identify energy peaks | `;` Up |
| **LISTEN** | Monitors a specific frequency for incoming packets | `.` Down / `Enter` in SCAN locks to peak |
| **AUTO** | Steps through SF/BW combinations on current frequency; locks on reception | `a` (press again to cancel) |
| **CHAT** | Bidirectional messaging using private packet frames; keys act as keyboard input | `c` |

- `,`/`/` cycles preset frequencies · `r` resets peak-hold / packet log / guard statistics
- In SCAN mode, `v` toggles between **ISM (863–928MHz) / VHF (161.5–162.5MHz)** bands
- In VHF band, `a` activates **AIS Guard Mode** (raw RSSI energy detection without demodulation; logs to SD `/ais/*.csv`)
- ⚠️ AIS Guard and LoRa AUTO share the `a` key, routed dynamically based on whether VHF band is active
- Architectural note: CHAT shares RF frontend state with the other modes (uses the actively monitored frequency preset), returning cleanly to LISTEN upon exit

### Bluetooth — 3 Parallel Subscreens
1. **Scan Devices** (`SCREEN_BT_SCAN`) → **Device Details** (`SCREEN_BT_DEVICE`, parses Service Data & Remote ID) → **Proximity Radar** (`SCREEN_BT_RADAR`, RSSI signal curve + pentatonic audio tones, `m` mutes, `r` resets)
2. **Keyboard Mode** (`SCREEN_BT_KEYBOARD`): Real BLE HID keyboard. Modifier keys (Ctrl/Opt/Alt, held or tap-locked), Fn layer (Esc/F1–F12/Arrows/Home/End), typematic auto-repeat, local echo box, `Fn+Enter` toggles IME mode, `Fn+\`` exits
3. **Media Remote** (`SCREEN_BT_MEDIA`): `,`/`/` Prev/Next track · `;`/`.` Volume · `Enter` Play/Pause · `m` Mute · `s` Stop

### GNSS — 5 Subpages (Map Extracted as Independent App; Module Config Moved to Settings)
1. **Fix Overview** (`SCREEN_GNSS`): Skyplot (constellation colors, SNR dot sizes), lat/lon, altitude, ground speed
2. **Details** (`SCREEN_GNSS_DETAIL`): 2D/3D status, GGA fix quality indicator (Autonomous/DGPS/RTK Fix/Float/Dead Reckoning), compact PDOP/HDOP/VDOP precision factors, Geoid Separation, used/visible satellite count; **press `c` to cycle through 4 coordinate formats** (DEG Decimal Degrees / DMS Degrees Minutes Seconds `dd°mm'ss.s"` / GRID Maidenhead QTH locator / UTM projected coordinates in meters)
3. **Satellite Signal** (`SCREEN_GNSS_SAT`): Defaults to a **zenith skyplot** (aligned with Sats on azimuth-horizontal and elevation-vertical coordinates), mapping satellites by azimuth/elevation, colored by constellation, sized by SNR, with active satellites highlighted by green glow rings and collision-avoidant PRN labels. Press `m` to toggle seamlessly between **Skyplot and Signal Table**. The table retains SNR sorting, `[`/`]` row selection, 8-bar signal meters, and `Enter` inspects individual satellite telemetry.
4. **Speedometer** (`SCREEN_GNSS_SPEED`): Semicircular sports gauge with auto-ranging dynamic scales (120 / 360 / 1000 km/h, auto-switching with 50/100 and 300/360 hysteresis buffers to prevent gauge jitter)
5. **Trip & Diagnostics** (`SCREEN_GNSS_TRIP`): Cumulative distance (with adaptive anti-drift filtering), total elapsed time vs moving time, top speed and average speed, TTFF first fix duration and reacquisition latency, used/visible satellites, motion state (MOVING/IDLE), Top-4 average SNR, and a rolling 2-minute SNR history graph. **Press `r` to reset trip statistics and waveform history**.
- **Module Configuration** (`SCREEN_GNSS_CONFIG`, relocated to Settings as a standalone item): `[`/`]` selects items, `-`/`=` alters values, `Enter` applies, `s` persists to Flash. Configures 5 parameters: Update Rate (1Hz/2Hz/5Hz, default 5Hz), System Constellations (GPS/BDS/GLONASS), Dynamic Platform Model (Aero <2g / Aero <1g / Vehicle / Portable, defaults to Aero <2g high-dynamic mode), NMEA Sentence Set (full / nav+gsv reduced set), and RF Antenna power. Press `` ` `` to return to Settings.
- **Underlying Protocol & Safety Mechanics**: Automatically configures high-dynamic mode (`PCAS11,6` Aero <2g), 5Hz rate (`PCAS02,200`), and streamlined NMEA output (`PCAS03` retaining GGA/GSA/GSV/RMC) upon boot, with 3.5s delayed retry and 500ms hotplug recovery. TinyGPS++ preserves `isValid()` indefinitely after a single fix; firmware enforces a strict 3-second freshness check (`GNSS_FIX_STALE_MS = 3000`). Speed and heading are trusted only when fixes are fresh and HDOP ≤ 8.0, displaying `n/a` otherwise to eliminate stationary drift artifacts.

### Map — 1 Screen (Independent App)
- `[` zooms out, `]` zooms in across five zoom levels: WORLD / z5 / z8 / z11 / z14
- Offline JPEG stream decoding from MicroSD, supported by 48KB base map buffer (falls back to 24KB 8bpp buffer, and ultimately to built-in world vector map if allocation fails)
- When indoor without GPS, centers on IP geolocation (indicated via `ip` status tag in top bar); trail points and "You Are Here" markers render only when true satellite fixes exist; location queries utilize DeferredFetch to avoid blocking UI rendering

### IR — 1 Screen (Infrared Remote)
- Hardware: GPIO44, **transmit-only, no receiver**; outputs codes from built-in tables or SD card definitions
- `,`/`/` selects device, `;`/`.` selects key, `Enter` transmits, `p` power sweep, `r` reloads SD `/ir/*.ir`
- Built-in profiles for LG, Samsung, and Sony TVs; supports loading external SD definitions in NEC32, NEC, Sony, RC5, and RAW formats
- Transmissions driven by hardware RMT channel 3, initialized on demand only while viewing this screen; line buffer expanded to 1024 bytes statically allocated outside loop stacks; automatically recovers RMT drivers after OOM retries

---

## SKY (Sky-Facing Apps)

### Astro — 3 Subpages (Fully Offline, Local Math)
1. **Daylight** (`SCREEN_ASTRO_SUN`): Solar elevation curve + Sunrise / Solar Noon / Sunset / Day Length (computed locally using standard astronomical −0.833° refraction model), falling back to IP geolocation coordinates if GPS is unavailable
2. **Lunar Phase** (`SCREEN_MOON`): Phase name, lunar age in days, illuminated percentage, next full / new moon predictions
3. **Terminator** (`SCREEN_ASTRO_TERM`): Day/night shadow boundary overlaid on world map + current subsolar point coordinates
- All three subpages execute entirely through local calculation without network calls; `r` triggers location recalculation

### Weather — 5 Subpages
1. **Current Conditions** 2. **Next 24h Hourly Curve** 3. **Wind & Atmospheric Data** 4. **Air Quality Index (AQI)** 5. **5-Day Extended Forecast**
- All five subpages share `r` and `Enter` for refresh
- Metric vs Imperial units selectable in Settings; solar daylight view separated into Astro

### Planes — 1 Screen
- ADS-B aircraft radar; `;`/`.` selects aircraft, `r` refreshes (clearing route cache)
- Aircraft list fetches via plaintext HTTP independent of NTP sync; route lookup (adsbdb.com) for selected aircraft uses HTTPS
- Standardized `User-Agent: cardputer-adv/1.0` header prevents upstream 403 blocks

### Sats — 1 Screen
- Satellite pass tracker; `c` (or `;`/`.` ) toggles Starlink / GPS constellations; `r` refreshes; requires `N2YO_API_KEY`
- Search radius differentiated by target class (Starlink 25° / GPS 90°) to prevent oversized JSON responses from overflowing TLS heap

### Typhoon — 2 Subpages
1. **Alerts & Current Status** 2. **Track Map**
- Shared `;`/`.` cycles storms; `r` refreshes; queries JMA via HTTPS

### Quake — 2 Subpages
1. **List View** (`SCREEN_QUAKE`): Magnitude, location name, timestamp, depth, and relative distance
2. **World Map Plot** (`SCREEN_QUAKE_MAP`): Global earthquake scatter plot; marker size and color coded by magnitude
3. USGS public summary feeds, globally comprehensive, key-free; `m` toggles feeds (M2.5+/24h vs M4.5+/7d) stored in NVS; streaming JSON deserialization prevents memory exhaustion

---

## MORE (Personal Dashboards, Multimedia & System Settings)

### Ducky — 3 Submodes
- **DK_PICK**: Script selector from MicroSD (list view)
- **DK_READY**: `Enter` executes payload (requires active BLE connection), `p` returns to selector
- **DK_RUN**: Keystrokes suppressed during execution; `` ` `` aborts entire app

### ChatGPT — 1 Screen + 1 Subscreen
- Query entry (≤200 characters), `Enter` dispatches to background task (animated "asking..." prompt)
- Response subscreen (`SCREEN_CHAT_REPLY`): `;`/`.` page turn; preserves 3 turns of conversational context; requires `CHAT_*` configuration
- **Memory Pressure Protection**: Temporarily releases 64.8KB display canvas (`cv.deleteSprite()`) before initiating HTTPS handshakes, rendering text prompts directly via `M5.Display`; canvas restores seamlessly upon completion or exit. Enforces `HTTP/1.0` to eliminate 12KB chunked transfer buffers, preventing TLS handshake OOMs and task deadlocks.

### GitHub — 1 Screen
- Contribution activity heatmap; `r` refreshes (rate-limited to 30-minute intervals); buffers incoming data into temporary arrays before committing, preserving error context at bottom

### Radio — 1 Screen + 2 Subscreens
- 6 SomaFM presets + 1 `[Custom URL]` slot
- Station list: `;`/`.` selects station · `Enter` plays · `u` edits custom URL · `[`/`]` or `-`/`=` volume · `m` mute · `r` reconnect
- **Playback Screen** (`SCREEN_RADIO_PLAY`): 15-band dynamic audio spectrum analyzer, real-time buffer waveform, bitrate, and ICY-Title metadata
- **URL Editor** (`SCREEN_RADIO_URL`): Accepts `http://` streams (≤120 characters)
- Dual-core cooperative task shutdown prevents lwIP mutex deadlocks; signals timer alarms to yield audio hardware

### Router — 1 Screen
- Clash / mihomo status: connection count, active proxy node, version, latency; `r` refreshes; requires `CLASH_*` configuration
- Node and group names URL-encoded; PUT requests serialized via ArduinoJson

### FX — 2 Subpages
1. **Spot & Trend** (`SCREEN_FX`): Spot exchange rate + daily change + 30/90-day historical trendline
2. **Daily Closes** (`SCREEN_FX_DAYS`): Chronological table of daily close prices and daily deltas
- European Central Bank (Frankfurter) public reference feed, key-free; fetches full timeseries in a single request; color-coded trend indicators; `m` toggles 30/90-day window, `r` refreshes

### OKX — 1 Screen
- Real-time USDT → CNY exchange rate; `r` refreshes (auto-refreshes every 5 minutes)
- Dual-source failover: Primary source fetches live OKX C2C matching buy prices; automatically degrades to CoinGecko on timeout or network block, indicating active source in status bar

### Reader — 1 Screen + Reader Subscreen
- Standalone TXT ebook bookshelf: Scans MicroSD for text files, supports font size adjustments, reading progress memory, and fast page seeking

### BadApple — 1 Screen
- ASCII animation and audio playback: Automatically seeks ahead when playback lags by >1 frame to maintain strict synchronization with audio

### Settings — 14 Items
| Item | Presentation & Behavior |
|---|---|
| Wi-Fi | → Scan subscreen → Password prompt subscreen (saved to NVS, auto-connects on boot) |
| **GNSS Config** | → Subscreen (`SCREEN_GNSS_CONFIG`: Hardware parameters for Rate / Constellations / Dynamics / NMEA sentences / Antenna power; `` ` `` returns to Settings) |
| **PC MODE** | → Enters PC host mode (releases 64.8KB canvas, direct-draws minimal status screen, exchanges serial PCM handshake; `` ` `` or `PCEXIT` exits to Settings) |
| Brightness | → Subscreen (`;`/`.` adjusts LCD backlight in ±5% steps) |
| Volume | → Subscreen (`;`/`.` adjusts speaker volume in ±10% steps) |
| Boot sound | **In-place toggle** (ON/OFF: 5-tone rising arpeggio on boot; fully silent when disabled) |
| **LED Mode** | **Press Enter to cycle 6 modes**: OFF / Battery / Breathe / Rainbow / Chase / Music Reactive |
| UI Theme | **Press Enter to cycle 8 high-contrast HUD themes** |
| Auto sleep | → Subscreen (Never / 30s / 1m / 3m / 5m display sleep timeout) |
| Timezone | → Subscreen (POSIX timezone selection, default: CST-8) |
| Weather Unit | **In-place toggle** (Celsius / Fahrenheit) |
| Battery | → Subscreen (Read-only: live voltage, percentage, discharge curve, and calibration data) |
| **Debug** | **In-place toggle** (Permanent bottom telemetry status bar, saved to NVS) |
| Format SD | → Subscreen (Two-step safety confirmation; press `y` twice to format) |
| About | → Subscreen (Firmware version, build timestamp, and boot-stage RAM waterfall chart) |

> **LED Priority Arbitration**:
> - The mode selected in Settings provides the system-wide baseline lighting pattern.
> - When the Spectrum analyzer enables microphone reactive lighting (`L` key) or the countdown timer rings, the subsystem temporarily claims the LED via `ledSetOverride(true)`, suspending standard patterns. Exiting or muting calls `ledSetOverride(false)` to restore previous modes.
> - Priority Safeguard: If the Spectrum visualizer has already claimed the LED, timer alarms yield without overriding (preventing visual disruption and avoiding erroneous turn-offs upon alarm clearance); when unencumbered, timer alarms borrow the LED to pulse red when audio hardware is busy.

---

## Off-Menu Features: Serial Command Channel

Accessed via `pio device monitor`; implemented in `serial_cmd.cpp`. **The device can be fully driven remotely from a PC** (single characters pass directly to the UI as keypresses).

- Navigation: `HELP`/`?` · Single characters / `ENTER` / `BACK` / `UP` / `DOWN` · `MENU` · `GOTO n` · `GNSSMAP`
- Diagnostics: `STAT` · `MEMCAP` · `RAMLOG` · `TRAIL` / `TRAIL MARK n` · `GPS` · `SHOT` · `CAT path`
- Networking: `PROBE` · `WIFI`/`WIFIOFF` · `HGET`/`HPOST`/`TCPHEX` · `RANDMAC`/`SETMAC`
- RF Sampling: `RIDSCAN` · `BTDUMP` · `BTEXT` · `RFSCAN` · `RIDFAKE`
- Toggles: `DEBUG ON`/`DEBUG OFF`

> Certain hardware capabilities **have no corresponding menu entries** (2.4GHz census via `RFSCAN`, extended advertising sampling via `BTEXT`, memory fragmentation inspection via `MEMCAP`, and boot-stage memory waterfall via `RAMLOG`).

---

## Considerations Prior to Menu Reorganization

1. **TOOLS and NETWORK are full**. Adding apps requires either introducing a 6th group or migrating existing apps.
2. **Three complex apps encompass 3–6 distinct functions each**: GNSS (6 pages), Bluetooth (3 distinct modes), and LoRa (4 modes). Splitting them consumes menu slots rapidly.
3. **HID utilities are fragmented**: Bluetooth Keyboard, Media Remote, and USB Ducky span different groups despite representing the same functional class.
4. **IR was previously a placeholder**; must either be implemented or removed.
5. **NetProbe and DnsFuzz shared structural overlap** and have been unified into Probe / DNS modes within NetProbe.
6. **Asymmetry between serial and on-device capabilities**; certain diagnostic utilities exist exclusively over serial.
7. **Settings mixes in-place toggles with subscreen navigations**.

---
---

# Menu Reorganization Plan (Finalized 2026-08-26)

## Three Finalized Principles

1. **Astro Remains Purely Offline** — Houses Lunar Phase / Terminator / Daylight only; **excludes Sats** (which requires internet + N2YO API keys, breaking the offline identity).
2. **Clock Merged into Time** — Consolidates Clock + Stopwatch + Timer into a single unified app.
3. **Complex Apps Split** — GNSS / Bluetooth / LoRa evaluated for dedicated top-level entries.

## Shift in Grouping Philosophy

Earlier groupings mixed two orthogonal concepts: TOOLS/NETWORK/SIGNAL were organized **by hardware peripheral** (developer implementation perspective), SKY was organized **by theme** (user perspective), and MORE was a catch-all. Grouping Moon Phase under Clock felt awkward because Clock was categorized under "no RF needed" hardware constraints, whereas lunar observation is thematic.

The reorganization **prioritizes thematic relevance**. Hardware exclusivity (RF contention) remains a physical constraint, but describes "what happens after launch" rather than user intent, and should not dictate the primary navigation taxonomy.

## Target Layout: 34 Apps / 7 Groups

| Group | Count | Margin | Apps | Categorization Rationale |
|---|---|---|---|---|
| **TOOLS** | 7 | 1 | **Time**★ · IMU · Files · Spectrum · Calc · Converter · Player | Local utility tools |
| **SKY** | 5 | 3 | **Astro**★ · Weather · Planes · Sats · Typhoon | Sky-facing observations |
| **SCAN** | 5 | 3 | WiFi Chan · Sniffer · Wardrive · Drone ID · **BLE Scan**★ | Passive discovery (promiscuous mode / scanning, connection not required) |
| **NET** | 4 | 4 | Hotspot · LAN Scan · NetProbe · DnsFuzz | Active networking (requires active connection) |
| **SIGNAL** | 5 | 3 | LoRa · **LoRa Chat**★ · GNSS · **Map**★ · IR | Non-Wi-Fi transceiver channels |
| **HID** | 3 | 5 | **BT Keyboard**★ · **BT Media**★ · Ducky | Device acting as keyboard / controller to host |
| **MORE** | 5 | 3 | ChatGPT · GitHub · Radio · Router · Settings | Personal dashboards and settings |

★ = Newly added or extracted from composite apps

Total capacity: 56; used: 34; **every group retains spare margin** (previously TOOLS/NETWORK had 0 margin).

## Itemized Changes

### Merges

- **Time** ← Clock (Page 1) + Timer. Clock's remaining subpages (Terminator / Moon Phase) move to Astro.
- **Astro**★ (New) ← Moon Phase (Clock p3) + Terminator (Clock p2) + Daylight (Weather p3).
  All three subpages rely purely on local computation: `moon.cpp` / `ui_common.cpp:subsolarPoint()` / `weather.cpp:solarElev()`.

### Splits

- **GNSS** (6 pages) → **GNSS** (Fix / Details / Satellites / Config, 4 pages) + **Map**★ (Standalone app)
- **Bluetooth** (3 modes) → **BLE Scan** (Scan / Details / Proximity Radar) + **BT Keyboard** + **BT Media**
- **LoRa** (4 modes) → **LoRa** (SCAN/LISTEN/AUTO) + **LoRa Chat** (Private packet messaging)

### Resolved Bugs

**Daylight subpage previously broken by Weather empty-state guard.** `drawWeatherSun()` began with `if (drawEmptyState()) return;`, suppressing the solar elevation curve when network weather was unpopulated—even though curve points are computed entirely locally via `solarElev()`. Moving the feature into Astro resolves this naturally.

## Trade-offs and Costs

1. **7 groups mean wider tab bars.** G0 advances across groups in the main menu; traversing from group 1 to 7 requires 6 presses (previously 4).
2. **⚠️ Splitting Bluetooth increases BLE teardown cycles.** Previously, toggling between the 3 modes occurred within the app without tearing down stacks; splitting them into top-level apps forces roundtrips through the main menu, triggering `btExit()`. Because BLE teardown is an established hazard (see [ble-teardown.md](ble-teardown.md)), ensure transitions do not trigger unnecessary deinit calls. **This represents the primary technical risk of the reorganization.**
3. **Map must acquire location independently.** Previously it relied on GNSS fixes; as a standalone app it uses `geoGet()` (falling back to IP geolocation when GPS is unacquired)—enabling indoor use.
4. Compile-time assertions in `GROUPS[]` catch intermediate misconfigurations.

## Open Questions

- Should the **GNSS Configuration page** remain within GNSS or move to Settings?

---

## Implementation Progress

- **2026-08-26 Step 0**: Consolidated duplicate `isBleScreen()` implementations (commit `37a8a38`). Diagnosed and resolved issue where exiting Bluetooth left 8KB free RAM and disabled Wi-Fi (commit `4e9b455`).
- **2026-08-26 Step 1**: Time merged + Astro extracted.
  - Clock + Timer → **Time** (Clock / Stopwatch + Timer subpages)
  - Moon (Clock p3) + Terminator (Clock p2) + Daylight (Weather p3) → **Astro** (SKY group)
  - TOOLS: 8→7, SKY: 4→5, total APPS remains 30
  - Astronomical math consolidated into `astro.cpp`
  - **Fixed**: Daylight view was previously masked by `drawWeatherSun()`'s `drawEmptyState()` check
  - **Fixed**: Sunrise/sunset calculations computed locally via −0.833° criteria rather than relying on API strings; `weather.cpp` drops `sunrise,sunset` API parameters
  - **Fixed**: Timer bottom status prompt truncated on 240px screens
  - Timer preset adjustment keys updated from `;.,/ ` to `[ ] - =`
- **2026-08-26 Step 2**: NETWORK divided into SCAN and NET.
  - Divided by **passive vs active**: SCAN apps rely on promiscuous mode / scanning without connections; NET apps require active connections or AP mode.
  - 6 groups: TOOLS 7 · SCAN 4 · NET 4 · SIGNAL 4 · SKY 5 · MORE 6 = 30
  - ⚠️ Group names restricted to **maximum 6 characters**: 240px / 6 groups = 40px per tab; Font0 uses 6px/char ("SIGNAL" spans 36px, leaving 2px margin)
  - **Added compile-time assertion `groupsChainOk()`**: Replaced runtime check with a constexpr validator verifying: contiguous group ranges, ≤8 apps per group, and complete coverage of `APPS[]`.
  - HID group deferred to Step 4 pending Bluetooth split.
- **2026-08-26 Step 3**: GNSS separated from Map. **LoRa split cancelled**.
  - **Map established as independent app** (SIGNAL group, alongside GNSS). Rationale: the 48KB base map buffer is now managed on explicit entry/exit rather than allocating silently during pagination.
    Measured: Entering drops heap 72632→24132 (−48.5KB); exiting restores 72072.
  - **Indoor functionality added**: Falls back to IP geolocation when satellite fixes are unavailable (marked with `ip` tag). `haveGps` and `haveCenter` are strictly decoupled—trails and position markers render only with true GPS fixes.
  - GNSS reduced from 6 to 5 pages, requiring zero exit cleanup (the 48KB buffer is allocated exclusively by Map).
  - 7 groups, 31 apps. Validated via `groupsChainOk()`.

### ⚠️ LoRa Chat Not Split — Original Plan Was Flawed

Source code review revealed that CHAT **shares underlying RF state** with the other modes: `curFreq`, `presetIdx`, and `activeModem` are global. Entering CHAT calls `loraApplyPreset()` using the frequency currently monitored in LISTEN, and exiting returns directly to LISTEN. CHAT semantically represents "transmit on the channel just verified", rather than an independent application.

Splitting into an independent top-level app would require selecting a preset in LoRa, exiting to the menu, and entering LoRa Chat—where `,`/`/` are consumed by text input, preventing in-situ channel changes. The plan was discarded in favor of pressing `c`.

- **2026-08-26 Step 4 (Conclusion)**: Bluetooth split into three apps; HID group established.
  - **BLE Scan → SCAN group** (Includes device details + proximity radar). Aligns with WiFi Chan / Sniffer / Wardrive / Drone ID.
  - **BT Keys + BT Media + Ducky → new HID group**. Consolidates device-as-HID functions previously scattered across groups.
  - Legacy 3-way Bluetooth menu (`SCREEN_BT`, `drawBtMenu`, `btMenuIndex`) eliminated.
  - Removed special-case BtnA logic in `main.cpp` for the keyboard screen.
  - **Added second compile-time check `groupNamesFit()`**: With 7 groups, tab width shrinks to 240/7 = 34px, requiring "SIGNAL" to be shortened to "SIG".
  - Verified clean entry/exit across all three apps; zero overhead traversing main menu.
  - Finalized baseline: **7 groups, 33 apps**
    TOOLS 7 · SCAN 5 · NET 4 · HID 3 · SIG 4 · SKY 5 · MORE 5
- **2026-09 Expansion to 40 Apps**:
  - TOOLS adds **Hash Oven** (Crypto workshop: Bench/Symmetric/Classic/Hash/Cracker 5 modes; capacity reached at 8/8)
  - NET adds **SSH** interactive terminal client (6/8)
  - SIG completes **IR** infrared transmitter (Hardware RMT driver, built-in + SD codes, 4/8)
  - SKY adds **Quake** global earthquake monitor (6/8)
  - MORE adds **FX** (forex trends), **OKX** (USDT spot), **Reader** (ebook shelf), **BadApple** (animation player; capacity reached at 8/8)
  - Post-expansion status: **7 groups, 39 apps / 84 Screens**
    TOOLS 8 · SCAN 5 · NET 5 · HID 3 · SIG 4 · SKY 6 · MORE 8
- **2026-09-24 ~ 2026-09-25 Feature Evolution & Stability Refactoring**:
  - **Clock Face Switching**: `Time` app page 1 cycles 4 faces via `f` (original progress bar, analog sweep second hand, big 7-segment digital, QlockTwo text matrix); persisted to NVS.
  - **LED Subsystem & Priority Arbitration**: Settings introduces 6 LED modes; `ledSetOverride` / `ledOverrideActive` arbitrates priorities: Spectrum mic reactive lighting takes precedence; timer alarms fall back to red LED pulses when audio hardware is active; alarms yield if Spectrum already holds the LED.
  - **Global Alarm Mute**: Pressing any key or G0 on any screen mutes ringing alarms instantly.
  - **GNSS Overhaul**: Automatically sets high-dynamic mode (`PCAS11,6` Aero <2g), 5Hz rate, and streamlined NMEA output; details view adds 2D/3D, PDOP/VDOP, GGA quality, and geoid separation; `c` cycles 4 coordinate formats; speedometer auto-ranges across 120/360/1000 scales with hysteresis; adds Page 6 Trip & Diagnostics; enforces 3s freshness and HDOP ≤ 8 checks.
  - **Spectrum Waterfall**: `W` toggles between Waterfall and Bars; 60 dB adaptive dynamic range, DC offset filtering, smooth row advances on new FFT frames.
  - **Hash Oven Cracker Enhancements**: Brute-force cracker supports dynamic character sets and authentic matching across all 5 tiers (`0123`, `card`, `cardpt`, `infinity`, `entropy9`), triggering `TARGET PWNED!` badges; generation counters prevent UAF in background tasks.
  - **Wi-Fi & Hotspot Coexistence**: Hotspot remains online during Wi-Fi scans, WiFi Chan, and Wardrive in `WIFI_AP_STA` mode; LAN Scan avoids exclusive RF modes; Drone ID and Sniffer suspend Hotspot cleanly via `hotspotSuspend()` and resume via `hotspotResume()`.
  - **SSH Enhancements**: Adds private key discovery on SD (RSA/ED25519, 13 candidate paths, passphrases); `Fn + Enter` cycles 3 font sizes with real-time PTY renegotiation; USB serial passthrough; CAS guards prevent double-free crashes.
  - **Code Review Defect Fixes (1–12)**: Player low-bitrate timeout adjustments and cooperative exit; IR 1024B buffer moved off loop stack; Drone ID bounds protection; JSON stream validation; GitHub error retention; Geoloc NTP checks with 10-minute GPS priority; Router URL encoding; ADS-B clock independence; NetProbe case-insensitivity; Radio cooperative shutdown; BadApple frame catch-up; LED sleep wakeups preserving backlight power.
- **2026-09-28 GNSS Module Configuration Relocated to Settings**:
  - **Architectural Decision**: GNSS configuration (`SCREEN_GNSS_CONFIG`) represents **system-level hardware configuration** rather than a continuous operational monitor, and was extracted from the `GNSS_PAGES[]` pagination chain into `Settings` (placed next to Wi-Fi).
  - **Keybinding Conflict Elimination**: Extracted subscreen avoids conflicts between pagination shortcuts and in-situ parameter adjustments; `[`/`]` selects items, `-`/`=` edits, `Enter` applies, `s` saves, and `` ` `` returns directly to Settings.
- **2026-09-27 PC MODE Relocated to Settings**:
  - **Architectural Decision**: PC MODE (`SCREEN_PCMODE`) is a **system-level feature** outside the standard application grid, and was placed in `Settings` (Page 1, Item 2) rather than consuming one of the 40 app grid slots.
  - **Protocol Discipline**: USB connection does not automatically enter PC MODE; activation requires manual entry or remote `PCMODE` commands from companion tools (e.g. `cardputer-bridge`, companion tool, not yet public).
  - **Unified Entry/Exit**: `pcmodeEnter()` standardizes entry (releases 64.8KB canvas, sends `PCM {"t":"enter"}` handshake); tracks entry origin via `pcmodeReturnScreen` to return cleanly to Settings or main menu.
- **2026-09-27 GNSS Satellite Skyplot Unified with Sats**:
  - **Visual Alignment**: GNSS Satellite view (`SCREEN_GNSS_SAT`) adopts the azimuth-horizontal / elevation-vertical coordinate system shared with Sats and Planes.
  - **Projection Extraction**: `(az, el) → (x, y)` math extracted into `skyview.cpp`'s `skyCoord()`, keeping Sats visuals pixel-identical. `skyview.cpp` operates without board dependencies for desktop simulation.
  - **Multidimensional Visualization**: Constellation colors, SNR dot radii, active satellite glow rings, and collision-avoidant PRN labels. Bottom HUD displays constellation visibility counts and maximum SNR readings. Seamless toggle between Skyplot and Signal Table via `m`.
- **2026-09-27 NetProbe and DnsFuzz Unified**:
  - Merged into a unified NetProbe app supporting Probe Mode (multi-target reachability/poisoning/portal checks) and DNS Mode (10 RFC malformed query tests).
  - `m`/`M`/`Tab` toggles modes; shares target editor and SD logging framework; auto-poll is restricted to Probe Mode to avoid unintended DNS fuzzing.
  - Preserves distinct log paths (`/netprobe.log` and `/dnsfuzz.log`) and NVS namespaces.
  - `DNSFUZZ` and `NETPROBE` serial commands retained as aliases.
  - Net group usage updated from 6/8 to 5/8 (3 spare slots); global app count adjusted to 39 across 84 screens.

### Subsequent Observations

- Hardware capabilities accessible exclusively over serial: `RFSCAN`, `BTEXT`, and `MEMCAP` heap fragmentation analysis.

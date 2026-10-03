**English** | [简体中文](README.zh-CN.md)

# UI Simulator (Host-side Screen Renderer)

240×135 is very compact, and the hardware board might not always be connected. Trying to imagine coordinates mentally often leads to accidentally stacking two elements on top of each other.
This tool **directly compiles the actual rendering code from `src/`** (rather than maintaining a duplicate copy, preventing drift from firmware behavior) and renders off-screen PNGs on the host machine to verify layout overlap and clipping.

```bash
pio run                                    # Run once first: M5GFX and ArduinoJson are fetched
                                           # from .pio/libdeps/; dependencies must be installed
cp src/secrets.h.example src/secrets.h     # Both router.cpp and sats.cpp #include this
cd tools/uisim
./build.sh          # Compile + render; PNGs output to out/
# macOS: open out/weather_1_now.png
# Linux: xdg-open out/weather_1_now.png
```

Currently renders these views (saved as matching PNG names under `out/`):

| File | View |
|---|---|
| `weather_1_now` / `weather_2_hourly` / `weather_4_air` / `weather_5_aqi` / `weather_6_forecast` | 5 Weather pages |
| `weather_2_hourly_cold` / `weather_6_forecast_cold` | Same two pages rendered with winter mock data: temperature labels expand to 4 characters (`-12C` / Imperial `104F` equivalent width), verifying edges are not clipped and neighboring text is not overwritten |
| `astro_1_daylight` / `astro_2_moon` / `astro_3_terminator` | 3 Astro pages |
| `astro_1_daylight_polar_n` / `_polar_s` | Daylight polar day / polar night branches (latitude set to ±85; which is polar day shifts with season) |
| `adsb` | ADS-B aircraft radar |
| `sats` | Satellite passes |
| `typhoon_1_alert` / `typhoon_2_track` | Typhoon alerts / tracks |
| `quake_1_list` / `quake_2_map` / `quake_3_scrolled` | Earthquake list / world map plot / list scrolled to bottom |
| `okx` | OKX USDT/CNY exchange rate (mock values intentionally formatted as **strings**, matching true OKX v5 format) |
| `fx_1_rate` / `fx_2_days` | Forex spot rate + chart / daily closes (mock data deliberately skips weekends, matching ECB publication schedules) |
| `router_0_connecting` / `router` | Router "connecting" and normal operational states |
| `stopwatch` | Stopwatch |
| `clock_1_hierarchic` / `_unsynced` | Clock face 1: Hierarchical life progress bar (synced / unsynced) |
| `clock_2_analog` / `_unsynced` | Clock face 2: Classic analog hands (smooth millisecond-interpolated sweep second hand, synced / unsynced) |
| `clock_3_digital` / `_unsynced` | Clock face 3: Big digital face (Font7 digital segments full-width, synced / unsynced) |
| `clock_4_text` / `_unsynced` | Clock face 4: QlockTwo minimalist text matrix (synced / unsynced) |
| `gnss_1_overview_fix` / `_nofix` / `_rfoff` | GNSS overview: Skyplot azimuth/elevation distribution, lat/lon/alt/speed, unlocated and RF-off states |
| `gnss_2_detail_deg` / `_dms` / `_grid` / `_utm` / `_neg_coords` / `_nofix` | GNSS details: 4 coordinate systems (DEG/DMS/GRID/UTM), southern hemisphere negative coordinates, unlocated state |
| `gnss_3_sat_sky` / `_sky_crowded` / `_sky_empty` | GNSS satellite skyplot (same coordinate system as Sats): typical data / crowded 5-constellation azimuth clutter, zero SNR, 3-digit PRN label collision avoidance / empty state without satellites |
| `gnss_3_sat_p1` / `_p2` | Satellite table toggled via `m`: active satellites (highlighted green dot + vivid constellation color) vs visible satellites, SNR and el/az, 2-page vertical scroll |
| `gnss_4_config_on` / `_off` | GNSS config: ATGM336H rate/constellation/dynamic mode/NMEA sentence/antenna power pill indicators |
| `gnss_5_speed_120` / `_360` / `_1000` / `_nofix` | GNSS speedometer: semicircular gauge dashboard, auto-ranging 120/360/1000 scales, light arc and digital readout |
| `gnss_6_trip` | GNSS trip & diagnostics: TTFF, reacquisition duration, distance, duration, top speed records, 2-minute SNR history |
| `spectrum_0_mic_unavailable` / `_1_bars` / `_2_waterfall` / `_3_vu_led` / `_4_rec_serial` | Audio spectrum: bar chart, pseudo-color waterfall, single RGB VU level, serial audio stream |
| `hash_oven_0_cold` / `_baking` | Hash Oven compute benchmark: standby and dual-core full-load burn-in animation |
| `hash_oven_1_aes_enc` / `_rc4_dec` | Hash Oven symmetric crypto: AES-128-CBC and RC4 stream cipher |
| `hash_oven_2_caesar` / `_base64` | Hash Oven classic/CTF tools: Caesar ROT13 shift and bidirectional Base64 conversion |
| `hash_oven_3_sha512_standby` / `_sha512_done` / `_sha256` | Hash Oven password hashing: Linux Shadow $6$ and standard hash calculation |
| `hash_oven_4_crack_standby` / `_crack_trying` | Hash Oven brute-force cracker: PIN dictionary brute-force attempt and progress ETA |
| `settings_1_p1` ~ `_p4` | Settings menu 4-page list: Wi-Fi, brightness, volume, LED, theme, sleep timeout, time zone, and 13 configurable parameters |
| `settings_led_off` ~ `_music` | Settings LED mode: 6 operating modes fine-tuning and status pill indicators |
| `settings_sub_bright` ~ `_format` | Settings secondary popups: brightness/volume sliders, sleep timeout, timezone list, battery status, SD format confirmation |
| `ssh_cfg_1_password` ~ `_nowifi` | SSH config: host/port/user/password/SD private key scan, font size selection, connect button, and parameter popups |
| `menu_0_tools` ~ `menu_6_more` | Main menu: 7 functional group carousel navigation with snapping selector animations |
| `menu_theme_amber` | Main menu: Amber theme color scheme |
| `about_1_info` / `_2_usage` / `_3_ram` / `_ram_scrolled` / `_ram_empty` | About system: Hardware specs, memory and storage usage, 12-stage RAM Profile waterfall chart with scrolling |

`data/` stores **real JMA payloads** (targetTc, specifications, and forecast samples).
Mock responses for typhoon pages are read directly from these files rather than embedded as strings in `sim_main.cpp`—storing 5.7KB of JSON as C string literals is messy and prone to escape errors, and keeping raw files allows verifying parsing results later.

## How It Fools Firmware Code

- `stubs/` provides a minimalist mock layer: `M5Unified.h` (M5Canvas → parentless `LGFX_Sprite`), `WiFi.h` / `HTTPClient.h` / `Preferences.h`, and a functional `String` implementation. `#define ARDUINO` must NOT be defined—otherwise LovyanGFX attempts to locate the real Arduino runtime.
- Mock `HTTPClient` fetches JSON responses mapped by `begin()` URL paths in `sim_main.cpp`, supporting both `getString()` and streaming `getStream()` / `getStreamPtr()`. Thus, data retrieval functions for weather, ADS-B, satellites, and router execute, parse, and populate page states identically to firmware.
- Rendering relies on M5GFX's built-in SDL platform layer (macOS: `brew install sdl2`, Linux: `sudo apt install libsdl2-dev`), drawing directly in memory without opening a GUI window.
- PNGs are output by a concise ~50-line encoder in `sim_main.cpp` (using zlib stored blocks) without external library dependencies, scaled 4x by default for readability.

## It Also Validates Menu Tables

Before compiling the simulator, `build.sh` performs a **syntax-only compile** of `src/globals.cpp`, `pages.cpp`, and `icons.cpp`. These files are omitted from the simulator binary (`globals.cpp` would collide with definitions in `sim_main.cpp`), and are checked purely to trigger two compile-time `static_assert` checks in `globals.cpp`:

- `GROUPS[]` must be contiguous, fully cover `APPS[]`, and each group must fit on a single screen
- Group names must fit into the tab bar (only 34px per tab when 7 groups are present)

When adding apps, menu tables are prone to silent errors (an app disappearing from menus, or two groups overlapping to display identical items). This step validates configuration integrity without requiring PlatformIO or physical hardware—deliberately decrementing the count of SKY immediately trips the assertion.

## Self-Tests Fail build.sh for Real

In addition to rendering images, the simulator executes a self-test: the forex parser previously lacked live verification against real-world responses (due to egress proxy restrictions on free APIs), so two response shapes for identical data are parsed and verified to yield matching data points.
Failures emit `FAIL` **and force `./build.sh` to exit with a non-zero code**—a single text warning would otherwise be lost among dozens of `wrote out/xxx.png` lines. PNGs are copied out before propagating exit codes, allowing visual inspection even if self-tests fail.

⚠️ Note on writing self-tests: `simResponseForPath` returns on the **first matching key**. Overriding a mock response for a key requires `simSetResponse()`; using `push_back` with a duplicate key will never be reached. An earlier draft made this mistake, parsing identical data twice and silently passing even when intentional regression bugs were introduced. **Always verify new self-tests by deliberately triggering a failure.**

## ⚠️ Missing Fields in Mock Data Leave Pages Untested

When `simResponseForPath()` cannot match a URL, it falls back to `simCannedResponse`. If data retrieval parses nothing, the page renders its empty state without error—displaying "no hourly data" and appearing deceptively normal.
When adding views, mock responses must be supplied for every endpoint dependency, followed by visual inspection of the resulting PNGs.

For example, `simCannedResponse` initially omitted `hourly` and air quality endpoints, causing the "Next 24h" and AQI pages to display empty states unnoticed. Supplying complete mock data immediately revealed that `drawWeatherHour()` had only 14px left margin—whereas the "34C" temperature label required 18px, pushing the initial character off-screen into "4C".

**Typical values alone are insufficient**: summer mock data featured two-digit temperatures (3 characters), fitting comfortably in Forecast page columns with 2px margin. In winter (`-12C`, 4 characters), labels overlapped neighboring columns and clipped off-screen. Both views now render supplementary winter variants (`*_cold.png`) to test maximum-width labels.

`data/usgs_quakes.json` contains a authentic USGS summary feed payload (9 items, including one with `mag: null` for unclassified events, which parser must skip, resulting in "8 shown" in the top right corner).
⚠️ Timestamps are offset to relative "now" by `restampQuakes()` in `sim_main.cpp`: raw millisecond timestamps would cause "time ago" displays to grow indefinitely over months (e.g. `180d`), appearing broken. Only the time axis is shifted; all other fields remain unmodified.

Similarly for `/above/` (N2YO satellites): omitting `info.satcount` causes sats.cpp to report "bad response (no 'info')" and display a red error message. Furthermore, realistic satellite distributions are essential—initially placing 12 satellites within 1 degree of the device caused them to cluster at the zenith, leaving elevation tracks unrendered. Distributing them across central angles 1°–22° (matching the ~23° horizon visibility radius for 550km orbits) properly covers zenith to horizon.
Additionally, ADS-B mock data deliberately includes an aircraft heading due north: azimuths at 0°/360° span across the screen wrap boundary, exercising the `drawWrapped` logic.

⚠️ Secrets handling: when `N2YO_API_KEY` is empty, the Sats view exits early before fetching. Since the simulator makes no live requests, filling `src/secrets.h` with any non-empty placeholder (e.g. `"SIMULATOR"`) suffices.

Likewise, `/memory` mock responses must include two entries: the initial entry in mihomo contains zero placeholder values, which `router.cpp` skips via `clashLine(..., skipObjects=1)`. Supplying only one entry keeps MEM permanently at 0M, reproducing a previously resolved bug.

## Adding New Pages

To add a view, add `shoot("out/xxx.png", drawXxx);` in `main()` of `sim_main.cpp`, and append the corresponding `src/xxx.cpp` to the compile list in `build.sh`.
If the page depends on external modules (e.g., `M5.Power`, SD card), either implement a mock in `sim_main.cpp` or decouple the dependency—the simulator focuses strictly on visual layout correctness.

## ⚠️ An Iron Rule: Never "Duplicate" Rendering Code in the Simulator

The core value of the simulator lies in **compiling the production code from `src/`**. Duplicating `drawSkyBg()` into `sim_main.cpp` previously led to confusion when edits to `src/` produced no change in simulator output.

The correct approach is to **extract shared rendering components into lightweight helper files without board-level dependencies** (such as `src/skyview.cpp`), and add them to `build.sh`.

The only allowed duplications in `sim_main.cpp` are trivial helper utilities (`trunc()`, `nowHM()`, `drawPageDots()`), annotated with clear rationales. All application drawing code must compile directly from source.

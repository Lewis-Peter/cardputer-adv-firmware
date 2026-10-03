**English** | [简体中文](README.zh-CN.md)

# tools — Companion Desktop Utilities

> Serial ports do not need to be typed manually: scripts use `serialport.py`'s `find_port()` to automatically detect the board by Espressif's VID `0x303A` (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux). You only need `-p` if auto-detection fails.

| Tool | Purpose |
|---|---|
| [`shot.py`](#shotpy--serial-screenshot) | Capture screenshots via serial to PNG — the only way to inspect the actual screen while connected |
| [`uisim/`](uisim/README.md) | Desktop UI simulator: directly compiles production drawing code from `src/`, rendering off-screen PNGs |
| [`lora_view.py`](#lora_viewpy--lora-sniffer-viewer) | Real-time tabular viewer for LoRa packets |
| `spec_view.py` | Acoustic capture: streams FFT spectrum from the Spectrum view, saves ndjson + spectrograms for PC-side analysis |
| `gen_cert_bundle.py` | Generates `data/cert/x509_crt_bundle.bin`: system root CAs + build-time verified cross-signed roots; see the Root Certificate Bundle section below |
| `spec_analyze.py` | Acoustic analysis: processes ndjson from `spec_view.py`, computes noise baselines/tones/classifications/time cuts, exports text/JSON/PNG |
| `rid_view.py` | Real-time drone Remote ID stream viewer; `--snapshot` continuously records latest state |
| [`memsweep.py`](#memsweeppy--per-app-heap-delta-audit) | Cycles in and out of each app to measure net heap deltas and detect memory leaks; `--selftest` tests the tool without hardware |
| `irtest/` | IR encoder testbed: compiles `src/ir_proto.cpp` to decode mark/space pulse sequences back to bits, verifying timings and polarities. `cd tools/irtest && ./build.sh` |
| `odidtest/` | Remote ID decoder testbed: compiles `src/odid.cpp` and feeds synthetic ODID packets to verify field offsets. `cd tools/odidtest && ./build.sh` |
| [`powertest/`](powertest/README.md) | Battery / charging testbed: compiles `src/power.cpp`, validating charge/discharge curves and low-battery alerts. `cd tools/powertest && ./build.sh` |
| `ramtest/` | RAMLOG memory log testbed: validates ring buffer statistics and formatted output. `cd tools/ramtest && ./build.sh` |
| [`gnsstest/`](gnsstest/README.md) | GNSS algorithm testbed: compiles `src/gnss.cpp`, covering DMS/Maidenhead/UTM conversions, trip filtering, and GSA satellite parsing |
| [`clocktest/`](clocktest/README.md) | Text clock word selection testbed: compiles `src/clock.cpp`, exhaustively verifying all 1440 minutes of English word highlight combinations |
| [`astrotest/`](#astrotest--astronomical-calculation-testbed) | Astronomy testbed: compiles `src/astro.cpp` and `src/moon.cpp`, validated against an independent Meeus implementation covering solar elevation, sunrise/sunset, polar day/night, subsolar points, and lunar phases |
| [`oventest/`](oventest/README.md) | Hash Oven brute-force testbed: compiles `src/hash_oven.cpp`, verifying keyspaces, collision-free full mappings, and solution generation |
| [`jsontest/`](jsontest/README.md) | Streaming JSON parser testbed: drives `src/http_json.h` with Stream mocks, verifying array streaming, truncation, timeouts, separators, and HTTP errors |
| [`calctest/`](calctest/README.md) | Calculator & unit conversion testbed: compiles `src/calc.cpp` and `src/conv.cpp`, running 107 assertions covering operator precedence, factorials, error handling, formatting, and unit conversion roundtrips |

---

## shot.py — Serial Screenshot

240×135 is very compact; relying purely on mental coordinate math easily results in overlapping elements, and photographing the small screen with a phone produces poor results.
This tool sends the `SHOT` command over serial and reconstructs the returned RGB565 per-line hex data into a PNG.
Layouts for GNSS and Weather pages were tuned iteratively using this utility.

```bash
python3 tools/shot.py                              # Capture current screen -> shot.png
python3 tools/shot.py --cmd GNSSMAP --scale 3      # Jump to map page first, then capture at 3x scale
python3 tools/shot.py --cmd GNSSMAP --wait 15      # Wait 15s (for tiles to finish loading) then capture
python3 tools/shot.py -p /dev/ttyACM0 -o /tmp/x.png       # Specify port manually if auto-detection fails
```

A full 240×135 frame of RGB565 hex occupies ~130KB (measured 131,581 bytes) and transfers in **about 1.3 seconds**.
The board uses native USB CDC; `monitor_speed = 115200` is merely a formal parameter for serial APIs—actual transfers run over native USB Full-Speed links unconstrained by this baud rate. Output images are scaled 3x by default for readability.

Requires `pyserial` (`pip3 install pyserial`).

### Division of Labor with uisim

- **uisim** requires no hardware, but only runs pages decoupled from physical peripherals (network and sensors are mocked).
- **shot.py** requires hardware connected, capturing the **actual runtime state** (real fixes, real SD map tiles, real heap conditions).

Use uisim for rapid layout iteration, and always confirm final layouts on hardware using shot.py.

---

## lora_view.py — LoRa Sniffer Viewer

Displays LoRa packets streamed from the Cardputer over USB serial in a real-time terminal table.
Provides a more comfortable experience for monitoring Meshtastic and LoRa packets on a computer screen than browsing on a 1.14" display.

### How It Works

When Cardputer enters the **LoRa app** in listen mode (LISTEN or AUTO), each received packet triggers a line printed over USB serial (implemented in `loraPushPkt` in `src/lora.cpp`):

```
LORAPKT {"ms":12345,"freq":869.525,"bw":250,"sf":11,"cr":5,"rssi":-92,"snr":-4,"crc":0,"len":40,"to":"ffffffff","from":"a1b2c3d4","id":"0f1e2d3c","hop":3,"hopStart":3,"ch":8,"ack":0,"mqtt":0,"hex":"...full payload..."}
```

- Prefix `LORAPKT ` facilitates easy filtering by scripts without mixing with boot logs (`[boot] ...`).
- Only prints during LoRa listening; other screens are unaffected.
- While the on-device screen only shows 5 bytes of hex per packet, **the serial output provides the complete payload**.

#### Field Reference

| Field | Meaning |
|---|---|
| `ms` | Milliseconds since firmware boot (packet arrival timestamp) |
| `freq` | Receive frequency (MHz) |
| `bw` / `sf` / `cr` | Modulation parameters: Bandwidth (kHz) / Spreading Factor / Coding Rate (x in 4/x) |
| `rssi` / `snr` | Signal strength (dBm) / Signal-to-Noise Ratio (dB) |
| `crc` | 1 = CRC failure (packet may be corrupt), 0 = valid |
| `len` | Payload length in bytes |
| `hex` | Full payload in hexadecimal |
| `to` / `from` / `id` | Meshtastic header: destination / source node ID (4 bytes little-endian) / packet ID |
| `hop` / `hopStart` | Remaining hops / initial hop count |
| `ch` | Meshtastic channel hash |
| `ack` / `mqtt` | want_ack flag / relayed via MQTT flag |

> Header fields such as `to`/`from`/`id`/`hop` appear only when a **valid Meshtastic header is decoded** (valid CRC and length ≥ 16 bytes).
> Meshtastic headers are unencrypted, allowing these metadata fields to be read even when payloads are encrypted.

### Usage

Switch Cardputer to the LoRa app → select **LISTEN**, and tune to an active frequency
(e.g., EU868 = 869.525MHz, or use SCAN to find peaks and lock on).

Then on your computer (**in a new terminal window; do not run `pio monitor` concurrently, as it will lock the port**):

```bash
python3 tools/lora_view.py                      # Auto-detect serial port
python3 tools/lora_view.py -p /dev/ttyACM0
python3 tools/lora_view.py -v                   # Also display non-LORAPKT serial lines in gray
```

Options:
- `-p/--port`: Serial device (auto-detected if omitted)
- `-b/--baud`: Baud rate (default: 115200)
- `-v/--verbose`: Also print non-LORAPKT serial lines (for debugging)

### Dependencies

Requires `pyserial`:

```bash
pip3 install pyserial
```

On Linux, system packages are also supported: `sudo apt install python3-serial`.
Alternatively, run directly with PlatformIO's bundled Python:

```bash
~/.platformio/penv/bin/python tools/lora_view.py                              # pipx / official installer
/opt/homebrew/Cellar/platformio/*/libexec/bin/python3 tools/lora_view.py      # Homebrew
```

### Example Output

```
Connected to /dev/ttyACM0 @ 115200. Switch Cardputer to LoRa LISTEN to view packets. Ctrl-C to exit.
#1      12.3s  869.525MHz SF11/BW250  -92dBm snr -4 ok   40B  a1b2c3d4→ffffffff  ch8 hop 3/3       <hex>
#2      15.8s  869.525MHz SF11/BW250 -104dBm snr -8 ok   28B  55667788→a1b2c3d4  ch8 hop 2/3 ACK   <hex>
```

RSSI is color-coded by strength (green ≥ -80 / yellow ≥ -100 / red weaker); CRC failures are marked in red.

### Port Not Found / Connection Issues

```bash
python3 -m serial.tools.list_ports -v      # List devices matching VID 303A
```

- If `303A:1001` is missing from the list: check the USB-C cable (must support data lines) or press reset to re-enumerate.
- If connections fail repeatedly: verify that no other software (`pio monitor`, Arduino Serial Monitor, etc.) holds an open lock on the port.
- On Linux, if permission denied occurs: add your user to the `dialout` group (`sudo usermod -aG dialout $USER`, relogin to take effect).

---

## Root Certificate Bundle (`data/cert/x509_crt_bundle.bin`)

Used by HTTPS pages (Sats / GitHub / Flight Routes / Typhoon / Quake / FX / OKX / Chat) to perform **strict certificate validation** instead of `setInsecure()`. Embedded into flash via `board_build.embed_files` in `platformio.ini`.
The linker-generated symbol is `_binary_data_cert_x509_crt_bundle_bin_start` (referenced in `src/tls_ca.cpp`)—**renaming paths requires updating this symbol name**.

Consumes ~56KB flash across 122 certificates. RAM overhead is minimal: only the index remains memory-resident; certificate bodies reside in flash and are read on demand.

### Regenerating the Bundle

```bash
python3 tools/gen_cert_bundle.py            # Update in place
python3 tools/gen_cert_bundle.py --dry-run  # Report additions without modifying files
```

⚠️ **Do not** manually run Espressif's default `gen_crt_bundle.py` to package raw system root certificates directly.
Bundles generated that way miss certain certificate chains. As diagnosed in detail in the header of `tools/gen_cert_bundle.py`:

- The terminal certificate in a chain is not always a self-signed root. When CAs transition to a new root, they issue cross-signed variants (subject is the new root, issuer is the old root). `esp_crt_bundle` inspects only the terminal certificate's **issuer** against its store, causing two failure modes: the legacy root has been removed from Mozilla trust stores (FX/OKX failure), or verification requires RSA-4096 (exceeding available heap, GitHub/Sats failure). Both surface as "Failed to verify certificate".
- The script moves verification to **build time**: the host validates the terminal certificate against trusted system roots, bundling it only if valid. On-device handshakes then take the fast path directly. ⚠️ The `openssl verify -partial_chain` step in the script enforces a strict security boundary; verification failures must abort bundle generation.
- Hostnames are scanned automatically from `src/` without manual list maintenance. The criteria are conservative—hosts where traditional paths suffice (e.g. USGS, whose issuer is in Mozilla stores using RSA-2048) are excluded to avoid pinning frequently rotated intermediate certificates.

This is complemented by a **patched `lib/WiFiClientSecure/`** (a copy of the Arduino-ESP32 framework implementation, taking precedence via PlatformIO's `lib/` search order). The patch modifies `esp_crt_bundle.c`: if the terminal certificate itself resides in the bundle (matching subject **and byte-for-byte identical public key**), it is accepted directly without re-verifying the signature.
⚠️ **Re-evaluate this file after updating the Arduino core**; modifications are explicitly marked with `[Local patch]` comments.

After generation, verify structural integrity (the first 2 bytes store the certificate count, followed by records of `name_len(2) key_len(2) name key`, sorted by name for binary search; the script validates this automatically):

```bash
python3 -c "
import struct; d=open('data/cert/x509_crt_bundle.bin','rb').read()
n=struct.unpack('>H',d[:2])[0]; off=2
for _ in range(n):
    nl,kl=struct.unpack('>HH',d[off:off+4]); off+=4+nl+kl
print(n,'certificates,', '✅ Structure valid' if off==len(d) else '❌ Mismatch')"
```

---

## memsweep.py — Per-App Heap Delta Audit

Inspecting code can prove that deallocations were written, but cannot prove they freed all allocated memory. This script utilizes serial primitives defined in [docs/serial-sweep.md](../docs/serial-sweep.md) (`MENU` triggers `cleanupApp`, `STAT` reports heap, `/` acts as right arrow, `ENTER` enters app) to cycle through each app N times, reporting the **median net heap change and largest continuous block change per round**.

```bash
python3 tools/memsweep.py                      # All apps, 3 rounds (2-phase auto-confirmation for long plateaus)
python3 tools/memsweep.py --rounds 5 --only Quake IR
python3 tools/memsweep.py --confirm-wait 180   # Extend confirmation observation to 3 mins for slow network protocols
python3 tools/memsweep.py --no-confirm         # Skip second phase confirmation; output phase 1 quick scan only
python3 tools/memsweep.py --selftest           # Verify tool logic using mock device without hardware
```

### Two-Phase Detection & Long-Plateau Confirmation

1. **Phase 1: Adaptive Quick Scan (All Apps)**
   When exiting an app, certain resources are released asynchronously (e.g. lwIP ARP timeouts taking several seconds, TLS cleanup tasks, ping task self-deletion delays):
   - After issuing `MENU`, the script waits for a minimum `--settle` duration (default 0.8s), then polls `STAT` at ~1-second intervals until consecutive readings for `heap` and `largest` stabilize, or until the `--settle-max` ceiling is reached (default 15s).
   - This phase scans all apps rapidly, preventing full sweeps from taking hours if every app waited multiple minutes.

2. **Phase 2: Extended Observation Confirmation (Flagged Apps Only)**
   Terminated TCP connections in the lwIP stack enter the `TIME_WAIT` state (2×MSL, ~120s), releasing PCB buffers only after expiration (e.g. DnsFuzz holding 448 bytes per round). The heap remains completely flat until expiration, causing Phase 1 adaptive probing to falsely flag this 2-minute plateau as a persistent leak.
   - Phase 2 cycles once through **only apps flagged in Phase 1 as suspected leaks or fragmentation**, entering extended observation (`--confirm-wait` defaults to 150s, polling every `--confirm-step` 5s).
   - If heap and largest continuous block return to baseline levels during observation (within ±64 bytes margin), the classification updates to `Delayed release (reclaimed after ~N sec, common cause TCP TIME_WAIT)` and is not considered a defect. If unreturned at expiration, the suspected leak classification is preserved and annotated with "not returned after 150s".
   - Pass `--no-confirm` to skip Phase 2.

### Decision Rules (6 Status Classes)

Determinations evaluate rounds **subsequent to round 1** (round 1 permits one-time initialization costs):
- **ok**: No significant reduction across initial or subsequent rounds.
- **One-time overhead**: Initial round incurs startup cost (e.g. TLS buffers or caches), but subsequent round medians return to 0; not classified as a leak.
- **Temporary drop (recovers in seconds)**: Drops immediately upon exit but recovers within ~10–15 seconds (e.g. LAN Scan / SlowFree); not a defect.
- **Delayed release (tens of seconds to minutes)**: Confirmed in Phase 2 to fully return during extended observation (e.g. DnsFuzz TCP TIME_WAIT expiration); not a defect.
- **Suspected leak (unreturned after confirmation)**: Heap continuously declines after Phase 2 observation (median delta ≤ -256 bytes).
- **Fragmentation (unrecovered after confirmation)**: Total heap recovers after Phase 2, but largest continuous block continuously contracts (median delta ≤ -512 bytes).
- **Unsettled**: Readings continue fluctuating within the `--settle-max` ceiling; indicates a need to increase the settle ceiling.

⚠️ BLE utilities, USB Ducky, Hotspot, and Settings are skipped by default for reasons documented under `SKIP_DEFAULT` in the script.
For comprehensive audit findings, see [docs/memory-audit.md](../docs/memory-audit.md).

---

## gnsstest/ — GNSS Coordinate Conversion, Trip Statistics, and GSA Parsing Testbed

Pure GNSS algorithmic logic (coordinate formats, trip filtering, NMEA parsing) contains numerous edge cases, yet debugging on physical hardware is slow and constrained by outdoor satellite reception. `gnsstest/` directly imports `src/gnss.cpp` to compile and run on the host with ASan/UBSan, evaluating 58 strict assertions.

### Why It Exists

1. **Coordinate Conversion Rigor**:
   - **DMS (Degrees Minutes Seconds)**: Validates directional hemisphere indicators (N/S/E/W), 0° and 180° extreme boundaries, and subtle carry behaviors—such as `59.996"` seconds carrying to 1 minute rather than invalid outputs like `"60.0\""`.
   - **Maidenhead Grid (6-character)**: Verified against globally recognized reference benchmarks: Shanghai People's Square (`31.2304N, 121.4737E -> PM01rf`), Royal Observatory Greenwich (`51.4769N, 0.0005W -> IO91wm`), Sydney Opera House (`33.8568S, 151.2153E -> QF56od`), and Times Square NYC (`40.7580N, 73.9855W -> FN20xr`).
   - **UTM (Universal Transverse Mercator)**: Matches easting and northing against reference points within 1 meter precision; validates the 10,000,000m southern hemisphere false northing offset, central meridian boundaries (e.g. 120°E boundary), and polar out-of-range protection (latitudes >84°N or <80°S returning `false`).
2. **Trip Statistics Filtering (`gnssTripProcessPoint`)**:
   - Simulates stationary drift filtering (low speed and short distance do not increment distance), constant-speed linear movement accumulating distance and time, outlier rejection (discarding instantaneous anomalies without corrupting totals), and signal loss recovery across time steps.
3. **GSA NMEA Parsing (`gnssProcessGSA`)**:
   - Validates multi-constellation sentences (`$GNGSA`, `$GPGSA`, `$BDGSA`), 16-field legacy formats, and 18-field formats containing NMEA 4.10 `systemId`.
   - Verifies active satellite extraction (deduplicated, empty slots excluded) and precision factor parsing (PDOP/HDOP/VDOP).

```bash
cd tools/gnsstest && ./build.sh
# Enable Sanitizers:
SAN=1 ./build.sh
```

---

## astrotest/ — Astronomical Calculation Testbed

Directly compiles `src/astro.cpp` (solar elevation `solarElev`, sunrise/sunset `sunCross`, subsolar point `subsolarPointAt`) and `src/moon.cpp` (lunar phase `moonPhaseAt`, `phaseName`), exposing test hooks via `-DHOST_TEST`.

### Where Reference Values Come From

**The testbed does not use tested code outputs as ground truth.** Instead, it independently implements algorithms from Jean Meeus's *Astronomical Algorithms* as reference standards: sidereal time (Eq. 12.4), apparent solar position (Chapter 25), and true new/full moons (Chapter 49, including periodic and planetary perturbation terms). These reference functions were first validated against textbook sample problems (12.a, 12.b, 25.a, 49.a). Tested code uses alternative approximations; agreement between two distinct algorithmic implementations confirms correctness.

- Solar elevation: Validated across 20,000 random samples (2020–2035), maximum deviation ~0.007°.
- Sunrise/sunset: 7 geographical locations (including polar circle and date line boundaries) across equinoxes and solstices match within 2 seconds of reference; polar day / night correctly return -1.
- Subsolar point: Deviation oscillates with leap year cycles, reaching a maximum of ~0.55° near September equinoxes (<0.5 screen pixel).
- **Moon Phase: Currently exhibits 2 FAIL items as genuine known issues**. Linear extrapolation deviates up to ~17 hours in 2026 (source comments noted "several hours"), causing NEXT FULL/NEW dates to be off by one day in Beijing Time for 9 instances. See details in section 5 of `main.cpp`.

```bash
cd tools/astrotest && ./build.sh
SAN=1 ./build.sh    # Enable ASan / UBSan
```

---

## clocktest/ — Text Clock (QlockTwo) Word Selection Testbed

Cardputer ADV's clock app includes a QlockTwo-style matrix text face (10 rows × 11 columns). It represents time by illuminating combinations of English words under strict lexical rules.

### Why It Exists

- **Boundary Minute Transitions**: e.g. `11:58` (pre-reads next hour, illuminating `FIVE TO TWELVE`), `23:58` (crosses midnight back to `MIDNIGHT`), `12:30` (`HALF PAST TWELVE`), `0:03` (`MIDNIGHT` + 3 corner dots).
- **Word Mutual Exclusion and Cohesion**:
  - Preamble consistently illuminates `IT IS`.
  - `PAST` and `TO` are mutually exclusive; top-of-hour transitions strictly require `O'CLOCK` (excepting midnight and noon).
  - Minute words (`FIVE`, `TEN`, `QUARTER`, `TWENTY`, `TWENTYFIVE`, `HALF`) match uniquely and precisely.
  - 12-hour and 24-hour (AM/PM indicator) switching.
- The testbed links `qlockCalcWords` directly from `src/clock.cpp`, **exhaustively traversing all 1440 minutes of a 24-hour cycle** and evaluating invariants across illuminated words at every minute.

```bash
cd tools/clocktest && ./build.sh
# Enable Sanitizers:
SAN=1 ./build.sh
```

---

## oventest/ — Hash Oven Brute-Force Guess Generation Testbed

Hash Oven's Futility Cracker mode: generates candidate passwords based on the selected keyspace tier and trial index (0..total-1).

### Why It Exists

- Indices and generated candidates must form a **bijective mapping (injective and surjective)**: zero collisions, with zero permutations omitted across the space.
- Preset solution passwords for each tier must exist within the keyspace and be generated accurately.
- Directly compiles `src/hash_oven.cpp` to cover:
  - 4-Digits tier: Exhaustive traversal of all 10,000 indices (0..9999), verifying zero collisions and zero omissions.
  - 4-Lower tier: Exhaustive traversal of all 456,976 indices (0..456,975).
  - Large keyspaces (8-character alphanumeric): 100,000-point stepped sampling across space, verifying identity in bidirectional encode/decode.
  - Solution generation validation across tiers and small-buffer overrun protection.

```bash
cd tools/oventest && ./build.sh
# Enable Sanitizers:
SAN=1 ./build.sh
```

---

## jsontest/ — HTTP Streaming JSON Parser Testbed

Cardputer ADV (ESP32-S3 without PSRAM, ~320KB internal RAM, 64.8KB full-screen canvas) risks OOM crashes when parsing large HTTP responses monolithically (such as GitHub 365-day commit arrays or USGS GeoJSON earthquake lists). The `fetchJsonStreamArray` parser was built specifically to deserialize items one by one without large heap allocations.

### Why It Exists

Real network streams exhibit diverse instabilities difficult to replicate on physical hardware. This testbed drives `src/http_json.h` using lightweight Stream / HTTPClient mocks to validate 113 test cases:

1. **Streaming Deserialization**: Multi-element objects, pretty-printed JSON with whitespace/newlines, nested arrays/objects, string lists, and null values.
2. **Empty Arrays**: Compact `[]`, whitespace-padded `[  ]`, and empty arrays nested under `arrayKey` (`{"items":[]}`).
3. **Prefix Navigation**: `arrayKey` (e.g. `"\"features\":["`) navigates outer wrappers efficiently, failing gracefully on missing keys.
4. **Anomalies and Fault Tolerance**:
   - Truncation mid-element, truncation after commas, missing closing brackets (strictly distinguishing `json error` from `stream timeout`).
   - Invalid separator interception (semicolons or missing commas trigger `"unexpected separator"`).
   - Stream read timeout detection (`"stream timeout"`).
5. **Control Parameters**: `maxItems` truncation, early termination when `onItem` returns `false`, and ArduinoJson `Filter` field filtering.
6. **HTTP and Configuration**: HTTP 404 / 500 handling, `http.begin` failures, and passthrough verification for User-Agent, Authorization, and timeout configurations.

```bash
cd tools/jsontest && ./build.sh
# Enable Sanitizers:
SAN=1 ./build.sh
```

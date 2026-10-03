**English** | [简体中文](memory-audit.zh-CN.md)

# Memory and Resource Leak Audit (2026-09-02)

    Summary up front: **Two genuine leaks were identified in chat.cpp and bt.cpp; both have been resolved**.
    All other modules were audited systematically by category; no other leaks were found.
    Additionally, `tools/memsweep.py` was introduced—reading source code can only prove
    "someone wrote deallocation code", but cannot prove "memory was fully reclaimed".
    True verification requires empirical measurements on hardware.

---

## Scope and Methodology

Rather than skimming code subjectively, the audit classified leaks by **underlying mechanism**, establishing mechanical criteria for each:

| Category | Criteria | Tool / Approach |
|---|---|---|
| Unpaired heap allocations | Pairing and codepaths of `new`/`malloc` vs `delete`/`free` | grep + line-by-line review |
| Tasks / Queues / Semaphores | `xTaskCreate` paired with `vTaskDelete` | grep |
| SD file handles | `SD.open` vs `close`; ESP32 `File` RAII behavior | grep + ESP32 `File` RAII analysis |
| HTTP connections | `http.begin` vs `http.end`, and whether `end()` closes underlying sockets | grep + reading Arduino framework source semantics |
| RF / Radio states | Pairing of `esp_wifi_set_promiscuous(true/false)` and `scanNetworks/scanDelete` | grep |
| Sprites / Large buffers | `createSprite` vs `deleteSprite` | grep |
| App teardown paths | Whether every `xxxExit()` registers with `cleanupApp()`; verifying all `screen =` transitions | grep cross-reference |
| Buffer overflows | External inputs indexing fixed-size arrays | Parser review + cppcheck |
| Unbounded growth | Unbounded loops appending to `String`, text fields without character limits | grep |

cppcheck (`--enable=all --inconclusive`, ~20k lines) emitted **zero errors and zero leak warnings**, producing only minor style lints. The two identified leaks were found through manual structural review.

---

## Finding 1 (Critical): `chat.cpp` Leaks Entire TLS Context per Query

### Manifestation

Each question asked on the Chat page permanently leaked tens of kilobytes of heap on a board with barely ~70KB of total free heap.

### Two Confounding Factors: Both Necessary for the Leak

**1. `vTaskDelete(nullptr)` bypasses C++ destructors.** When terminating a FreeRTOS task, the stack is reclaimed immediately without executing destructors for stack-allocated objects. `chatWorker()` maintained three heap-bearing objects on its stack:

    WiFiClientSecure client   Releases via stop() only in destructor; mbedTLS buffers occupy 16KB+4KB
    HTTPClient       http     Releases via end() only in destructor
    String reply, err         Stores up to CHAT_REPLY_MAX, allocating dynamic heap buffers

**2. `http.end()` does not close persistent sockets.** `HTTPClient::disconnect()` implements the following branch:

```cpp
if (_reuse && _canReuse) { /* tcp keep open for reuse */ }   // Sockets remain open
else                     { _client->stop(); }
```

`_reuse` defaults to `true`. Endpoints like OpenAI invariably return `Connection: keep-alive`, setting `_canReuse` to `true`. Thus `end()` takes the keep-alive branch, leaving socket descriptors and TLS contexts intact.

This behavior was already recognized in `router.cpp`'s `trafficClose()`, which documented this exact upstream logic after persistent `/traffic` streams experienced socket reuse corruption. However, the identical issue in `chat.cpp` had gone unnoticed—and was exacerbated by the complete absence of destructor execution.

### Fix: Structural Refactoring Instead of Ad-Hoc `stop()`

An initial draft added explicit `client.stop()` before each `vTaskDelete`. This was discarded because relying on manual cleanup calls invites regressions whenever early return paths are added in the future.

The implementation was refactored into a standard returning function:

```cpp
static void chatWorkerBody() { ... return statements only ... }

static void chatWorker(void*) {
  chatWorkerBody();          // Returning here ensures stack objects (client / http / String) are properly destructed
  workerBusy.store(false, std::memory_order_relaxed);
  workerDone.store(true, std::memory_order_release);
  vTaskDelete(nullptr);      // Sole task termination call in the module
}
```

By leveraging C++ language semantics, stack unwind guarantees clean destruction. Memory ordering was also hardened: release ordering now occurs as the true final step after all destructions complete.

---

## Finding 2 (Minor): `bt.cpp` Never Deletes `new BLESecurity()`

Standard Arduino BLE examples frequently instantiate `new BLESecurity()` without invoking `delete`. The object itself manages no dynamic resources—its three setters forward arguments directly to `esp_ble_gap_set_security_param()`, updating configuration within the Bluedroid stack. The wrapper object is useless once configured and was converted to a stack-allocated local instance.

While this leak was minor (~dozens of bytes) compared to the ~13KB permanent footprint of `BLEDevice::deinit()` (a documented upstream behavior accepted intentionally in `btReleaseForOtherApps()`), there was no reason to leave memory uncollected.

---

## Audited and Confirmed Clean

- **App Teardown Pathways**: All 15 `xxxExit()` hooks register cleanly with `cleanupApp()` (with `btExit` dispatched via `isBleScreen()`). Exiting via `` ` `` and BtnA main-menu resets share identical cleanup paths, as does serial `MENU`. The only pathway bypassing cleanup is raw serial jumps (`GOTO` / `GNSSMAP`), which serve as intentional debugging escape hatches and print explicit `[jump]` warnings over serial.
- **FreeRTOS Tasks**: Background worker tasks for chat, radio, and player terminate or delete cleanly. The `bctrail` watchdog task runs continuously by design, enabled only when the Debug setting is active.
- **SD File Handles**: ESP32's `File` wraps `shared_ptr<VFSFileImpl>`, closing files upon destruction via RAII. Explicit calls to `close()` are also present across file handlers.
- **HTTP Connections**: Except for global `trafHttp` in `router.cpp` (managed via explicit `trafficClose()`), all HTTP instances reside on function stacks and destruct on return.
- **Radio RF State**: `esp_wifi_set_promiscuous(true)` and `(false)` pair strictly; `scanNetworks()` and `scanDelete()` pair consistently.
- **Sprites**: The sole large sprite allocation is the 48KB base map (+ 24KB 8bpp fallback) in GNSS, freed via `deleteSprite()` in `gnssMapExit()` wired to `cleanupApp()`. Failed allocations set `_img` to nullptr without leaving partial leaks.
- **Buffer Overflow Protection**: 802.11 parsing branches in `wsniff.cpp` were validated against field constraints (`ssid[33]` bounds `slen <= 32`, `ridSubCount[16]` bounds `fsub = (fc>>4)&0xF`, `body[24]/body[32]` and `ridOdidRaw[100]` clamp lengths before `memcpy`, and IE loops enforce `off + 2 + tlen > len`). `odid.cpp` undergoes ASan-instrumented fuzzing in `tools/odidtest`.
- **Unbounded Growth**: All interactive text buffers have fixed bounds (Calculator: 48, Chat: 200, Wi-Fi password: 63, Hotspot password: 32, Unit converter: 16, Radio URL: 120, LoRa message: 64, NetProbe target: 40). Streaming parsers accumulating `String` (`clashLine` / `trafficPoll`) enforce 400-byte bounds.

---

## `tools/memsweep.py`: Turning Audits into Reproducible Measurements

Static analysis has an upper bound: it proves that deallocation statements exist, not that they execute completely. The definitive proof is entering an app, exiting it, and verifying whether free heap returns to baseline.

Leveraging serial automation primitives from [serial-sweep.md](serial-sweep.md), `tools/memsweep.py` automates this process:

```bash
python3 tools/memsweep.py                 # Cycle each app 3 times, reporting net heap deltas
python3 tools/memsweep.py --rounds 5 --only Quake IR
python3 tools/memsweep.py --selftest      # Verify script logic against a synthetic mock device
```

The evaluation focuses on **rounds subsequent to round 1**: initial launches legitimately establish caches, initialize drivers, and establish buffers, causing a one-time drop that never recurs. True leaks manifest as continuous drops across every cycle. The built-in self-test exercises both behaviors (`Leaky` leaking 2KB/round vs `Weather` dropping 48KB once on round 1).

⚠️ BLE modules, USB Ducky, Hotspot, and Settings are skipped by default for operational reasons documented inside the script.

---

## Follow-up: `RAMLOG` — Profiling the Boot Phase

This audit originally focused on **runtime lifecycle**: verifying whether entering and exiting apps restored heap. It did not address the boot sequence: **which boot stages consume memory during startup**. As documented in [README.md](../README.md), static RAM usage grew by +24.5KB over four weeks (from 94,876 to 119,420 bytes). Pinpointing the exact consumers was impossible with only `pio run` build summaries and runtime `STAT` snapshots, because identifying allocations requires differential measurements.

To address this, `src/ram_profile.{h,cpp}` was added along with the `RAMLOG` serial command: recording memory deltas across 11 stages in `setup()`, with an additional sample once Wi-Fi connects.

Design considerations:

- **Buffered telemetry rather than immediate prints.** Directly calling `Serial.printf` at each stage (as done in some third-party firmware) misses critical early boot stages: under native USB CDC, `Serial.begin()` occurs late in `setup()`, after `M5.begin()` and the 64KB display canvas are already allocated. Storing records in a 16-element static array allows dumping telemetry on demand.
- **Old entries are preserved when the buffer fills.** The earliest boot steps are the primary justification for the tool's existence.

Memory footprint is 320 bytes of static RAM (16 × 20 bytes). Because diagnostics requiring re-flashing are rarely utilized, this profiling mechanism remains permanently enabled rather than behind a compile-time flag.

`tools/ramtest/` provides a host-side harness compiling the same `src/ram_profile.cpp` against mock heap counters, validating column formatting, delta signs, and overflow behavior. Host compilation enforces `-Werror=format` to prevent silent printf type mismatches.

---

## Remaining Verification Items

- **The two bug fixes were verified structurally but require physical hardware confirmation.** Validating `chat.cpp` requires querying the LLM 5 times while sampling heap via serial `STAT`; heap should return to baseline rather than stepping down. `memsweep.py` does not test this path because it enters views without sending chat prompts.
- **`memsweep.py` has been verified via synthetic self-tests**, but full end-to-end runs on hardware should be observed with `--verbose`.
- **`RAMLOG` was validated via desktop test harness (`tools/ramtest`) using synthetic fixtures.** Hardware baselines should confirm: the `canvas` step delta should register near -64,800 bytes (240×135×2), and `wifi up` should reflect ~ -36,000 bytes (measured network stack footprint per [README.md](../README.md)).

---

## CanvasLease Adoption (2026-09)

### Background and Architectural Invariants

The full-screen canvas `cv` occupies 64.8KB (240×135×2 DMA contiguous memory). On an ESP32-S3 without PSRAM, mbedTLS handshakes require two contiguous heap buffers of ~16.7KB each (~33KB total). As heap fragmentation develops over runtime, TLS handshakes frequently fail due to contiguous block exhaustion. `CanvasLease` (`src/globals.h`) uses RAII to temporarily release the display canvas during TLS network fetches, restoring it automatically upon exiting scope.

Adopting this pattern requires adherence to four strict invariants:

1. **Pre-instantiate Certificates and Clients**: `WiFiClientSecure client; tlsUseCaBundle(client);` must be constructed before acquiring the lease. `tlsUseCaBundle` allocates a permanent certificate index (~484 bytes) via `calloc`; allocating this inside the lease scope fragments the vacated canvas memory hole, permanently preventing canvas reallocation.
2. **Reverse Destruction Order**: Declare `CanvasLease` prior to large temporary objects like `JsonDocument` or `filter`. C++ stack unwinding destroys objects in reverse order, ensuring parsing buffers are reclaimed before reallocating canvas memory.
3. **No Persistent Allocations Inside Scope**: The lease scope must contain only transient, short-lived allocations. Persistent results (data arrays, error strings, caches) must be allocated, reserved, or statically assigned before entering the lease.
4. **Rendering & Concurrency Safety**: Verify whether network requests execute synchronously on the main thread (rendering pauses naturally) or inside background tasks. For main-thread execution, coordinate with `DeferredFetch` to trigger requests after rendering initial UI prompts.

---

### Application Evaluation and Migration Breakdown

| Module | Protocol & Source | Adaptation Pattern | Safety Analysis & Rationale |
|---|---|---|---|
| **src/quake.cpp** | USGS GeoJSON (HTTPS) | Wrap `fetchQuakes()` with `CanvasLease` | **Main-thread synchronous**. Pre-reserve `errMsg.reserve(96)` and client; parse into static array `eqs[MAX_EQ]`; `filter` and streaming docs unwind before lease destruction. |
| **src/okx.cpp** | OKX C2C + CoinGecko Fallback (Dual HTTPS) | Single `CanvasLease` spanning primary and fallback requests | **Main-thread synchronous**. No rendering occurs between fallback attempts; a single lease avoids unnecessary 64.8KB deallocation/reallocation churn; client calls `client.stop()` between attempts; pre-reserves `errMsg`, `okxErr`, and `fbErr`. |
| **src/typhoon.cpp** | JMA Disaster Prevention JSON (Multi-step HTTPS) | Lease in `fetchList()`; single lease across current conditions and forecast in `ensureDetail()` | **Main-thread synchronous**. Refactored `jmaGet` to accept external client reference, preventing repeated CA index allocations; details load after 400ms debounce; calls `client.stop()` between sub-requests; writes to static `list` and `det`. |
| **src/adsb.cpp** | ADS-B Route Lookup (api.adsbdb.com, HTTPS) | Aircraft list uses plain HTTP (unmodified); lease applied in `fetchRoute()` | **Main-thread synchronous**. Routes fetched after 350ms selection debounce; pre-reserves `fetchErr.reserve(64)` and client; route records stored in static ring cache `rcache`. |
| **src/fx.cpp** | Frankfurter Forex Rates (HTTPS) | Wrap `fetchFx()` with `CanvasLease` | **Main-thread synchronous**. Pre-allocates `pts` struct array (~700B), pre-reserves `errMsg.reserve(96)` and client; lease declared before `filter`/`doc`; parser writes directly into `pts`. |
| **src/github.cpp** | GitHub Contributions (HTTPS) | Wrap `fetchContrib()` with `CanvasLease` | **Main-thread synchronous**. Pre-reserves `errMsg.reserve(96)` and client; lease wraps `fetchJsonStreamArray`; streaming callbacks populate static arrays `tmpCount`/`tmpLevel`. |
| **src/chat.cpp** | LLM API (HTTPS) | **Unmodified** | Uses a dedicated FreeRTOS worker task with manual `canvasRelease()` / `canvasRestore()` coordination; multithreaded concurrency is unsuitable for synchronous scoped leases. |
| **Other Modules** | Weather / Router / Map Tiles / IP Geolocation (HTTP) | **Not Applicable** | Plaintext HTTP requests without the 33KB contiguous TLS buffer requirement; canvas leasing is unnecessary. |

---

### Hardware Verification Checklist

Verify the following behaviors on hardware with serial logging or the on-screen Debug Bar active:

1. **Quake View**:
   - Enter Quake; issue `STAT` to note `free heap` and `largest`.
   - Press `r` to trigger refresh; verify `largest` returns to baseline after fetch completes.
   - Switch data source (`m`) to fetch the larger 2.5_day feed; verify smooth canvas restoration.
2. **OKX Forex**:
   - Enter view; verify canvas restores after primary OKX fetch.
   - Simulate network failure to trigger fallback; verify CoinGecko fetch succeeds and canvas restores cleanly.
3. **Typhoon Alerts**:
   - Enter view; verify initial list renders correctly.
   - Navigate items with `;` / `.`; verify debounce triggers both condition and forecast fetches, restoring canvas and baseline `largest`.
4. **ADS-B Route Lookup**:
   - Enter view and wait for aircraft list to populate.
   - Navigate with `;` / `.`; verify route fetch triggers after 350ms debounce, rendering airport codes and progress bars without white screens or crashes.
5. **FX Rates**:
   - Enter view; switch between 30d/90d views via `m`; verify clean deallocation of `pts` upon exit.
6. **GitHub Heatmap**:
   - Configure a valid `GITHUB_USER`; verify heatmap renders and canvas restores following streaming deserialization.

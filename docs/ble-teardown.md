**English** | [简体中文](ble-teardown.zh-CN.md)

# Diagnosing Intermittent Hangs in the BLE Teardown Path

A full day's investigation on 2026-08-09. Summary up front: **not resolved**—switching Bluetooth back and forth repeatedly still eventually necessitates a reboot, which is explicitly noted in the UI. However, the artifacts produced during this investigation are more valuable than the immediate outcome: a persistent forensic mechanism (`src/bctrail.*`, now merged in firmware), two methodological lessons, and hard empirical data that will save others from retreading this path.

The experimental patch remains on an experimental branch; **it was not merged into mainline and merging is not recommended**, for reasons detailed in the final section.

---

## Background: Why Touch BLE Teardown in the First Place

Arduino's `ESP32 BLE Arduino` library provides no teardown mechanism—`BLEServer` does not even declare a destructor, and `createServer()` simply overwrites previous instances. Consequently, every invocation of `btHidSetup()` permanently leaks an entire tree of server + service + characteristic + descriptor objects. Real-world measurements of the largest free continuous heap block:

```
63476 → 23540 → 18420 → 17396 → 10740 → (continues dropping until crash)
```

Block counts from serial `MEMCAP` confirm this is genuine leakage rather than pure fragmentation: after two cycles, total free heap shifted from 73460 to 80032 (**actually increased**), while allocated blocks surged from 401 to 593. The 192 extra unreleased blocks permanently fragment continuous RAM like scattered nails.

---

## Round 1: Vendoring the Library to Implement Destructors

The library was vendored into `lib/BLE/` (differing by only 52 lines across 9 files compared to framework upstream) to implement recursive `delete` traversals across ownership trees and add `BLEDevice::destroyServer()`. Key architectural decisions, which proved correct in hindsight:

- **`BLEHIDDevice::~` remains empty.** It holds a **non-owning alias** into the server tree; calling delete would cause a double free. The true owner is `BLEServer → ServiceMap → CharacteristicMap`.
- **The commented-out `free` inside `BLECharacteristic::~` was not reinstated.** `m_value` is a `BLEValue` backed internally by `std::string`; calling `free` attempts to deallocate non-heap memory.
- **`createServer()` was modified to "return existing instance if present"**, preventing orphaned trees when overwriting.

### Memory Impact: Leak Successfully Fixed

Tracking the largest contiguous block across identical switching sequences (Bluetooth ↔ Calc):

```
Baseline:  63476 → 23540 → 18420 → 17396 → 10740 → (continuous decline until crash)
Patched:   63476 → 25588 → 22516 → 19444 → 19444 → 18420 → converges steadily
```

Transforming continuous decline into stable convergence around 18–19KB confirms the leak was genuinely eliminated. This conclusion stands independently of subsequent hang issues.

### But Introducing Intermittent Hangs

Approximately 1 hang occurred every 10 cycles, distributed randomly between rounds 3 and 9. Critical evidence:

- **At the moment of hang, largest block remained at 18420 with ample heap**—this was not an out-of-memory failure (earlier firmware crashing at round 5 was OOM; this was distinct).
- Saved PC halted at `esp_pm_impl_waiti`, confirming a **blocking stall** rather than a panic: the main loop was waiting indefinitely for an event that never arrived.

For a handheld device, intermittent random hangs are worse than gradual leaks: the latter are predictable and easily cleared with scheduled restarts. The fix was therefore kept out of mainline.

---

## Lesson 1: Instrumentation Masks Failures (Classic Heisenbug)

The initial hypothesis centered on FreeRTOS semaphores: each BLE object maintains semaphores (4 per service, 3 per characteristic), and FreeRTOS strictly mandates that **semaphores with tasks blocked on them must not be deleted**. Timing depends on asynchronous BLE stack operations, potentially explaining the randomness.

Serial logging markers + `Serial.flush()` were added between each teardown step. **Over 15 test cycles, zero hangs occurred.**

Stretching the teardown execution across time made the bug disappear entirely. Logging could not capture the crash site—the final log line was invariably the last benign step. This behavior was itself informative: the bug was sensitive to the **internal cadence** of the teardown sequence.

> **General takeaway**: When diagnosing race conditions, any instrumentation involving I/O risks expanding timing windows enough to mask the bug. Telemetry must be cheap enough to avoid perturbing execution schedules. This insight motivated the creation of the breadcrumb system.

## Lesson 2: Adding Delays Reduces Frequency but Never Eliminates Race Conditions

```
No delay                  Hangs on rounds 3, 9    ~1 in 8
delay(200) after deinit   Hangs on round 15       ~1 in 15
delay(20) per service     Hangs on round 35       ~1 in 35
```

Failure frequency decreased roughly in proportion to inserted delays—**a textbook indicator of race conditions**. Adding arbitrary delays never solves the underlying defect; it merely pushes probabilities down while leaving a lingering failure tail.

This also disproved the simple semaphore hypothesis: inserting `delay(200)` between `deinit` and `delete` ran cleanly for 12 rounds in cycle 1, then hung on round 3 in cycle 2.

---

## Lesson 3: Timing-Preserving Forensics — Breadcrumbs

A ring buffer in `RTC_NOINIT` memory: `bcMark()` performs a single byte write and index increment, zero I/O, zero locks. Survives warm reboots and automatically replays upon boot, also readable anytime via serial `TRAIL`.

**This captured the exact failure point.** Across two consecutive hangs, breadcrumbs terminated at the exact same sequence:

```
... 1F <SERVER_EXIT   06 after_destroySrv   00 before_GATTS_dereg   01 before_deinit
(02 after_deinit NEVER APPEARED)
```

The entire object tree destructor ran to completion cleanly; the hang occurred inside the subsequent call to `BLEDevice::deinit()`.

> Diagnostic rule: Look for **which index is absent**. The total absence of `02` proved the stall occurred between `01` and `02`, inside `deinit()`.

### Searching in the Wrong Place

Prior debugging spent two entire rounds focusing on destructors—the wrong target. The overarching firmware architecture (**never deinit BLE; only enable/disable**, noted in `bt.cpp`) was designed precisely to bypass unreliable deinit routines; `btReleaseForOtherApps()` was the sole location still calling `deinit`.

Earlier observations of crashes at round 5 (largest heap 7668) attributed to OOM were likely this identical deinit hang masked by low-memory conditions.

---

## Incidental Finding: GATTS App Slot Leak (Genuine Bug, but Not the Cause of the Hang)

Within the upstream library, `createApp()` executes `esp_ble_gatts_app_register(m_appId++)`, yet **the GATTS layer never unregisters applications** (unregistration existed only partially commented-out on the client side). Bluedroid provides limited GATTS app slots; repeated recreation will exhaust them.

The experimental branch introduced `BLEDevice::unregisterServerApp()` and restructured `btReleaseForOtherApps()` into three stages:
**Unregister (stack alive) → deinit → delete objects (callbacks stopped)**. Even with this, hangs recurred at round 9, and breadcrumbs verified that unregistration had succeeded—confirming this was not the cause of the hang.

⚠️ **Whether slot exhaustion occurs in production firmware remains unverified.** Production code traverses this path (`btReleaseForOtherApps()` clears `hidStarted` → next launch calls `btHidSetup()` → `createServer()` → `createApp(m_appId++)`), but whether `deinit(false)` via `esp_bluedroid_disable` implicitly reclaims registrations was never audited on hardware.

---

## Why Not Merge the Experimental Fix

Adopting the memory fix requires vendoring the entire ~15k-line BLE library, burdened with an ~10% intermittent hang rate. **Net negative.** Current firmware policy: accept the known leak, monitor available heap via the Bluetooth UI menu (orange warning `<20KB restart soon`, red `<13KB restart before using`), prompting intentional reboots. On embedded microcontrollers, reboots are a valid operational strategy.

## If Further Investigation is Pursued

- **Dump `vTaskList()` during hangs**—directly inspect which task is blocked on what resource rather than speculating. A background watchdog timer in the main loop could print task states when execution stalls. `bctrail`'s stall watchdog already provides the skeleton; adding `vTaskList()` is straightforward.
- **Or attach JTAG** to examine call stacks at the stall point (see Linux udev rules in [linux-setup.md](linux-setup.md)).
- Do not use serial print instrumentation; it masks the failure.

## Enduring Artifacts

`src/bctrail.{h,cpp}`—Breadcrumb logging + main loop watchdog, now permanently part of the firmware. Completely decoupled from BLE, reusable for any obscure hang. Breadcrumbs incur zero heap cost (64 bytes in RTC slow memory, zero DRAM allocation); the stall watchdog requires 3KB stack space and is enabled conditionally via the Debug setting.

**"Persistence across soft reboots" was verified empirically** on firmware (2026-08-26): writing `AB 11 1F FF` via `TRAIL MARK`, asserting RTS hard reset, and reading output on boot recovered all four bytes identically with index `next=4` preserved. Verify this tool behavior before debugging—an all-zero breadcrumb buffer looks identical to an untouched execution path.

---

## 2026-09-30 Re-audit: Incorrect Teardown Sequence

Re-reading framework source code (arduino-esp32 2.0.17 / IDF v4.4.7 `38eeba213a`):

- **The experimental `unregisterServerApp()` was a no-op.** It was invoked after `bleSuspended`, when Bluedroid had already been disabled via `btExit()`. The first line of `esp_ble_gatts_app_unregister()` asserts `ESP_BLUEDROID_STATUS_CHECK(ENABLED)` (esp_gatts_api.c:63), immediately returning `INVALID_STATE`. Breadcrumbs only proved the wrapper returned. Furthermore, `bta_gatts_deinit()` clears GATTS control blocks upon Bluedroid deinit, rendering cross-deinit "slot exhaustion" invalid.
- **The true suspect is teardown sequencing.** `btExit()` disables Bluedroid **and the controller**, after which `BLEDevice::deinit()` invokes `esp_bluedroid_deinit()`. Tearing down host stacks while the controller is halted violates IDF's required lifecycle order: disable → deinit(host) → disable → deinit(controller). `esp_bluedroid_deinit` performs an indefinite `future_await` on the BTC task; BTC allows only 1 second to join HCI/BTU threads before forcefully executing `vTaskDelete`. If terminated threads held mutexes, subsequent `osi_alarm_deinit()` blocks permanently. (This sequence is a deduction).
- Revision proposal: `btReleaseForOtherApps()` should re-enable the controller, then tear down strictly per IDF order, tracking each step via breadcrumbs `B0..B5` (inserting `BE <err_byte>` on failure).

Interpretation: `B1` present, `B2` absent = stalled in `esp_bluedroid_deinit`; `B3` present, `B4` absent = stalled in controller deinit (closed-source blob). A complete clean cycle is `B0 B1 B2 B3 B4 B5` with zero `BE` markers.

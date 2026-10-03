**English** | [简体中文](serial-sweep.zh-CN.md)

# Sweeping All 30 Apps via Serial

    The help string "single char -> fed to UI as keypress" implies the entire device can be driven
    programmatically from a host PC. Combined with STAT and SHOT, this enables an otherwise tedious manual task:
    **cycling through every app, recording heap margins, and capturing screenshots automatically**—a single sweep
    takes ~7 minutes, whereas clicking through 30 apps manually and transcribing numbers takes half an hour with guaranteed errors.

This document details the workflow and pitfalls caught during the initial audit.

---

## Available Primitives

| Command Sent | Effect | Echo Output |
|---|---|---|
| `ENTER` / `BACK` / `UP` / `DOWN` | Navigation key aliases | ✅ `[remote] cmd="..." -> screen=N` |
| `MENU` | Return to main menu (**triggers cleanupApp**, see below) | ✅ |
| `/` `,` `;` `.` | Single characters fed directly as keys (Right/Left/Up/Down) | ❌ **Silent** |
| `STAT` | `screenOff=` / `idle=` / `sleepAt=` / `heap=` / `largest=` / `minEver=` / `screen=` | ✅ |
| `SHOT` | RGB565 per-line hex dump (~1.3 seconds) | ✅ |
| `GOTO n` | Raw jump, **no cleanup**, `enter()` called only for network fetch pages | ✅ Annotates `[jump]` warning |

Common pitfalls:

- **Do not send `\n` to emulate ENTER.** In automation scripts, developers often write `write(b'\n')`. The serial parser operates line-by-line; an isolated newline is treated as an empty line and discarded. Use the `ENTER` command alias.
- **Single-character commands produce no echo, but execute reliably.** Lack of terminal response initially looks like an ignored key; verify navigation by issuing `ENTER` and inspecting `screen=`.
- **⚠️ Never assume starting position.**
  The menu index `menuIndex` **persists across navigations**: `MENU` merely resets `screen` to the main menu without altering the selection index. Reissuing `ENTER` enters whichever app was selected previously. Scripts assuming "count N steps from 0" will drift, giving the false impression of lost keystrokes. **Always verify current position via `screen=`, or navigate deterministically using `GOTO n`.**
- **⚠️ There is no left boundary wall.** `menuMove()` wraps across category boundaries, and moving left from group 0 wraps directly to the last group—the menu is completely **circular**. The assumption of "pressing left 40 times hits index 0" is invalid; 40 left presses evaluates to `(start - 40) mod 31`. To navigate reliably, read `menu=` via `STAT` and calculate: `steps = (target - current) % APP_COUNT`.
- **`STAT` now reports menu indices**: `menu=18/31 (Map) group=SIGNAL`. Before this telemetry was added, assuming starting at index 0 caused severe measurement errors.
- **Keystroke transmission cadence: Zero loss at ≥0.08s intervals.** Benchmarked by issuing 10 page turns across the 5 Weather pages (two full cycles) and 30 consecutive right-arrow keys across the main menu:

  | Interval | Result |
  |---|---|
  | 0.05s | Lost keys in 1 of 2 runs |
  | 0.08s ~ 0.50s | **Zero key loss** (verified across menus and page chains) |

  0.1s is a safe cadence; intervals above 0.3s are unnecessary. Note that the main menu redraws every frame with selector easing animations, but this does not drop incoming serial keys.
  Re-testing with menu index reporting confirmed that across three rounds of 10 right keys (starting at 22/23/24, ending at 1/2/3), `(22+10) mod 31 = 1` matched precisely without dropping a single key.

---

## Key Distinction: `GOTO n` Does Not Truly Enter the App

`GOTO` updates the `screen` variable to trigger rendering. While `enter()` was retrofitted for network-fetching views (ADS-B / Sats / Router / GitHub / Weather / LAN Scan / Typhoon / Files) to exercise network allocation behavior, **hardware-exclusive subsystems (LoRa claiming SPI, Spectrum enabling mic, BLE/Ducky starting stacks) intentionally skip `enter()`** to prevent debug commands from disrupting peripheral states. Therefore:

- To **inspect UI layouts** → `GOTO` is fast and sufficient.
- To **audit memory and lifecycle behavior** → All views (except the network fetch pages noted above) must be entered via `ENTER`, otherwise dynamic allocations never execute.

Heap deltas measured via `GOTO` giving "zero memory change" are illusions.

---

## Pitfall Caught: `MENU` Previously Stranded Heap Memory

**Symptom.** An audit script issuing `BACK` followed by `MENU` at each step observed heap steadily dropping by 7.5KB across 30 apps, falsely suggesting multiple leaks.

**Investigation.** Cycling in and out of a single app six times yielded Δ = 0—proving no leak existed. The variance lay entirely in the exit path:

```
Path A: Single BACK + jump via MENU   →  0 bytes reclaimed
Path B: Two BACK presses (normal exit) →  48,500 bytes reclaimed
```

**Root cause.** Map is a **subpage** of GNSS; pressing `` ` `` on a subpage returns to page 1 (subpage carousel logic in `main.cpp`). A second press is required to invoke `cleanupApp(SCREEN_GNSS)`. Previously, `MENU` performed a raw assignment `screen = SCREEN_MENU`, bypassing `cleanupApp()` and stranding the 48KB base map buffer until reboot.

**Fix** (`serial_cmd.cpp`): `MENU` now invokes `cleanupApp(screen)` prior to transitioning. `MENU` is classified as a navigation alias alongside `ENTER`/`BACK`; simulating a user returning to the menu should mirror pressing `` ` ``. For raw jumps without cleanup, `GOTO` remains the designated primitive.

Following the fix, both pathways reclaim 48,500 bytes, returning `largest` to 61,428.

**`GOTO` and `GNSSMAP` remain uncleaned, but emit warnings.** This escape hatch is useful—inspecting other pages during LoRa packet sniffing or BLE scanning without aborting capture sessions. However, silent omissions are hazardous: raw jumps now output an explicit warning:

```
[jump] Raw jump without cleanup: leaving screen=28. Held resources (48KB map / LoRa SPI /
       BLE stack) remain allocated until reboot; use MENU or BACK for clean exit.
```

When auditing memory, discard readings following this warning. ⚠️ **Do not use `GOTO` during memory profiling runs**; navigate using `ENTER`/`BACK`/`MENU`.

---

## Incidental Finding: Fragmentation Becomes the Bottleneck Before Total Free Heap

After a complete traversal across all apps:

```
heap    72,648 → 66,336     Down 8.7%
largest 61,428 → 24,564     Down 60%
```

**Total heap dropped marginally, while the largest contiguous block collapsed by 60%.** This directly impacts the system because the GNSS map requires 48KB of **contiguous** memory:

| State | `largest` Before Entry | Map Buffer Allocation | Fallback Tier |
|---|---|---|---|
| Fresh Boot | 61,428 | **48,500** | Full Color 16bpp |
| After App Sweep | 24,564 | **24,260** | 8bpp Reduced Color |

The fallback in `gnss.cpp` ("drop from 48KB to 24KB when unavailable") is not mere defensive programming—it is an actively traversed production path. `largest` in `STAT` is a far more critical health metric than total `heap`.

---

## Conclusion: No Memory Leaks

Following the `MENU` fix, 5 apps were subjected to 4–6 consecutive entry/exit cycles:

| App | Rounds | Delta per Round |
|---|---|---|
| LAN Scan | 6 | All 0 |
| NetProbe | 6 | All 0 |
| Weather | 6 | All 0 |
| Player | 4 | All 0 |
| Bluetooth | 4 | All 0 |

**Zero leaks.** Reductions observed on first entry represent **one-time permanent allocations** reused across all subsequent launches.

The only genuinely unrecoverable allocation is **~14KB upon initial BLE initialization**: launching Ducky/Bluetooth allocates 41,384 bytes, while exit recovers only 27,244 bytes. However, subsequent cycles show Δ = 0. This is standard ESP32 platform behavior—the hardware BT controller cannot be fully unloaded once initialized. **The cost is incurred only once.**

---

## Menu Navigation is Circular: Never Assume "35 Left Keys" Resets to Zero

Initial attempts to reset to index 0 by issuing repeated left-arrow commands shifted selections by 25 positions, inadvertently entering Ducky (which safely paused at script selection without executing payloads).

Reason: `menuMove` wraps across categories at group boundaries; issuing 35 left keys evaluates to a net shift of `35 mod 30 = 5`, not index 0.

The reliable approach is **calibration**—enter once, read `screen`, look up the current index in a reverse map, and step deterministically:

```python
send('MENU'); send('ENTER'); sleep(2)
cur = BACKMAP[stat().screen]          # screen -> app index reverse mapping
send('BACK'); send('MENU')
for _ in range((TARGET - cur) % 30): send('/')
```

`screen=` in `STAT` is the sole trustworthy ground truth. Keystroke-based index counters drift across wrapped boundaries; validating against `screen` on each round eliminates ambiguity.

---

## Reproduction

The core automation loop executes as follows:

```python
send('MENU'); [send(',') for _ in range(35)]        # Step back to index 0
for i, name in enumerate(APPS):
    h0 = stat().heap
    send('ENTER'); sleep(2.0)
    scr = stat().screen                              # Verify successful entry
    shot(f'{i:02d}_{name}.png')
    send('BACK'); send('MENU')
    h1 = stat().heap
    print(name, h1 - h0)
    send('/')                                        # Next app
```

Key considerations:

- **Skip USB Ducky** (menu index 25): As a USB HID device, it injects keystrokes into the connected development workstation.
- RF-intensive apps (Sniffer / Wardrive / Hotspot / LAN Scan) disconnect Wi-Fi. The watchdog in `loop()` reconnects upon exit, but transient reconnection buffers temporarily skew heap readings of adjacent apps.
- Wait at least 2.0 seconds after `ENTER` before sampling `STAT`; some allocations are asynchronous (e.g. 48KB map allocations begin only when tile streaming starts).

---

## Screen Sleep, Keypresses, and "Observation Erasing the Phenomenon"

### Summary Up Front

- **Physical Keypresses**: When the screen is asleep, the first keystroke wakes the display and **is consumed** (handled via early return in `handleKey()`).
- **Serial Commands**: **Not consumed**. `handleSerialCmd()` actively wakes the display before dispatching commands (`serial_cmd.cpp`, rationale: serial commands are deliberate remote instructions). Automation scripts do not need to send dummy wake-up keystrokes.
- Display sleep timeout defaults to 30 seconds (`sleepOptIdx = 2`), **not persisted to NVS**, returning to 30 seconds upon reboot.

### Debugging Record & Field Lessons

During investigation, intermittent keystroke loss was initially suspected to be caused by screen sleep consumption. Telemetry was added to output `screenOff` in `STAT`. After leaving the device idle for 53 seconds, queries returned `screenOff=0`, suggesting sleep had failed. Repeated resets and noise checks consistently pointed to sleep failure.

**Root cause: `STAT` itself woke the display before printing.** Consequently, the field always read 0. The observation mechanism altered the observed phenomenon—identical to how adding I/O logging masked the BLE teardown hang (see Lesson 1 in [ble-teardown.md](ble-teardown.md)).

An external observation resolved the contradiction: the physical LCD was visibly dark, yet `STAT` reported active. The contradiction pointed to the measurement tool rather than the firmware state.

This also surfaced a genuine bug: serial wakeups **failed to update `lastActivityMs`**, causing wakeups to expire within a single frame—the next iteration of `loop()` immediately satisfied sleep criteria, extinguishing the display after ~20ms. This was corrected by refreshing `lastActivityMs`.

### How to Inspect This State Now

`STAT` now reports **`screenOffOnArrival`**—capturing whether the display was dark at the exact moment the command arrived, prior to waking:

```
[stat] screenOffOnArrival=1 idle=0s sleepAt=30s
```

Verification (commands spaced 1 second apart):

| Timing | `screenOffOnArrival` | `idle` |
|---|---|---|
| Immediately after keypress | 0 | 0s |
| First command after 35s idle | **1** | 0s |
| Immediately following command | 0 | 1s |

Line 3 confirms the wake bug fix: prior to the fix, line 3 would read `1` because the screen extinguished immediately after waking.

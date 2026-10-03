**English** | [简体中文](README.zh-CN.md)

# Battery / Charging Logic Testbed

The charging detection and battery percentage estimation in `src/power_util.cpp` **cannot have their critical execution paths verified on the actual device**: the board must be plugged into USB to flash firmware and read serial output, but the most critical branches of this logic are precisely "after unplugging" and "after charging for one hour". Therefore, synthetic voltage and time series are used here to drive the real production code (not a copy, following the same principle as [tools/uisim](../uisim/README.md)).

## How to Run

```bash
cd tools/powertest && ./build.sh
```

## Test Scenarios

1. Discharging on battery → plugged in and charging for one hour → unplugged. Verifies whether estimated percentage starts from the measured discharge value, increases at ~0.5%/min, and immediately falls back to real-time measurement upon unplugging.
2. **Powered on while already plugged in**—a user-reported bug. Previously, the voltage-slope method could not detect this scenario (voltage remains flat on the constant-voltage charging plateau with no rising edge), erroneously reporting "on battery" and treating 4.2V lookup as 100%.

`stubs/` provides only two mocks: `M5Unified.h` allowing tests to control the clock (`g_fakeMs`) and battery voltage (`g_fakeMv`), and `Preferences.h` using a global variable as NVS to manipulate "battery percentage saved before last shutdown".

> This testbed is essential: the first implementation stored charging session state inside `powerBatteryLevel()`, which is called on-demand by callers (only when the screen redraws). Running this testbed immediately revealed that estimated values remained stuck at the baseline—something impossible to catch by watching the real device because the top status bar redraws continuously. The state was subsequently moved into the periodic `powerUpdate()`.

**English** | [简体中文](HARDWARE.zh-CN.md)

# Cardputer ADV Hardware Reference

> Quick reference for writing custom drivers. ADV uses the same screen as the original Cardputer, but **the keyboard and audio chips have changed**—do not copy driver code from the original Cardputer.

Source: Bruce firmware board configuration + M5GFX source code (`board_M5CardputerADV`).

## SoC / Storage
- ESP32-S3FN8 (Stamp-S3A), dual-core 240MHz, 8MB Flash, **no PSRAM**
- USB VID/PID: `0x303A / 0x1001` (Native USB-C, no dedicated USB-to-UART chip → requires `-DARDUINO_USB_CDC_ON_BOOT`)

## Display ST7789 (⚠️ Identical to Original Cardputer)
M5GFX 0.2.25+ automatically identifies `M5CardputerADV`, so manual configuration is usually not required. Manual parameters if needed:

| Parameter | Value |
|---|---|
| Driver | ST7789, 135×240 (rotation=1 → 240×135 landscape) |
| MOSI / SCLK / DC / CS / RST | 35 / 36 / 34 / 37 / 33 |
| Backlight BL | 38 |
| SPI Frequency | 40 MHz |
| offset_x / offset_y | 52 / 40 |
| invert | true |

## Keyboard (⚠️ Changed on ADV: TCA8418 I2C Keyboard Controller)
The original Cardputer uses direct GPIO matrix scanning; **ADV switched to the TCA8418 chip**, so keyboard code from the M5Cardputer library cannot be used on ADV.

| Parameter | Value |
|---|---|
| I2C Address | `0x34` |
| SDA / SCL | 8 / 9 (internal I2C bus) |
| INT Interrupt Pin | 11 |
| Driver Library | `adafruit/Adafruit TCA8418` |

> In v1 keyboard polling: read INT → fetch key row/col via I2C → map to character.

## Audio (⚠️ ADV Adds ES8311 Codec)
| Parameter | Value |
|---|---|
| Codec | ES8311, I2C address `0x18` |
| Amplifier | NS4150B → 1W speaker |
| Microphone | MEMS (high SNR) |

> M5Unified's `M5.Speaker` / `M5.Mic` work directly. Currently Mic feeds the FFT spectrum for the Spectrum app, and Speaker provides the countdown alarm and volume feedback beeps. (A WAV player was written and later removed since it went unused, and its double buffer permanently consumed 2×4KB—this board has no PSRAM, so saving that for other features was preferred.)

## Other Peripherals
| Peripheral | Parameter / Details |
|---|---|
| Battery ADC | GPIO 10 |
| 6-axis IMU | BMI270, internal I2C (`M5.Imu`) |
| IR Transmitter | **GPIO44** (TX, same as original; confirmed by official docs). ⚠️ Transmit only, **no receiver**, cannot learn codes from existing remotes. Note: Pins 3/4/5/6/13/15 are actually used by Cap LoRa/GPS, not IR |
| RGB LED | Data GPIO 21, **SK6812** (drive as standard 3-byte GRB, no RGBW needed). ⚠️ **Power PWR_EN shares GPIO 38 with screen backlight** (official pinout G38 = DISP_BL + RGB LED PWR_EN shared), and M5GFX uses LEDC PWM on G38 for backlight control. **Never call pinMode/digitalWrite on G38 yourself**—it will peg the backlight to full on, causing brightness adjustment to fail and auto-sleep unable to turn off the screen (symptom: "frozen but screen stays on"). The LED draws power from this backlight rail: it can light up when the screen is on, and turns off when the screen sleeps (shared hardware power rail, expected behavior). Drive only data pin 21 |
| microSD | CS=12, SCK=40, MISO=39, MOSI=14 |
| Grove external I2C (side port) | SDA=2, SCL=1 (`M5.Ex_I2C` / `Wire`) |
| Internal I2C | SDA=8, SCL=9 (`M5.In_I2C`)—keyboard 0x34, codec 0x18, IMU, power, Cap IO expander 0x43 are all on this bus |
| **4-pin on Cap (HY2.0-4P Grove port)** | **I2C, attached to internal bus (8/9)**, read via `M5.In_I2C`. ⚠️ Shares bus with keyboard/codec/IMU—with the LoRa Cap attached, external I2C Units (temperature/humidity/RFID...) can still be connected as long as addresses do not collide with internal components. Together with the side Grove (external), there are two usable I2C buses in total |

## Two Critical Software Caveats

**GNSS UART RX Buffer**: ATGM336H on the Cap continuously streams NMEA at 115200 baud (~11.5 KB/s), while HardwareSerial default RX buffer is only 256 bytes = **22ms** of data. A single main loop frame takes 30~50ms, meaning without an enlarged buffer, bytes are dropped every frame and NMEA sentences are cut in half. `GPSSerial.setRxBufferSize(4096)` must be called **before** `begin()` (calling it after has no effect).

**Connect antenna before powering on**: Cap LoRa-1262 RF front-end must not transmit into an open circuit; doing so will cause permanent hardware damage.

## Reference Links
- Bruce board configuration: https://github.com/BruceDevices/firmware/blob/main/boards/m5stack-cardputer/m5stack-cardputer.ini
- M5GFX board definition: https://github.com/m5stack/M5GFX/blob/master/src/M5GFX.cpp
- Official documentation: https://docs.m5stack.com/en/core/Cardputer-Adv
- Cap LoRa-1262 documentation (SX1262 pins + HY2.0-4P): https://docs.m5stack.com/en/cap/Cap_LoRa-1262

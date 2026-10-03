**English** | [简体中文](antenna-shopping.zh-CN.md)

# Procurement Guide: VHF Receive Antenna (161.975 / 162.025 MHz)

> This document is written for procurement or future reference and **contains full self-contained context without needing to read code**.
> Objective: Acquire an effective antenna for the AIS guard/monitoring mode on the LoRa page (`src/lora.cpp`, SCAN + VHF, press `a`).

## Application Context

M5Stack Cardputer ADV + Cap LoRa-1262 module (SX1262 transceiver). The stock antenna is a short 868/915MHz stub.
We want to receive maritime AIS signals—**161.975MHz (AIS1/ch87B) and 162.025MHz (AIS2/ch88B)**,
requiring a dedicated **receive** antenna for the 162MHz band. Receive-only; no transmission.

Why replacing is essential: 1/4 wavelength at 162MHz is **46.3cm**. The stock stub is only ~8cm, acting as ~1/23 wavelength on this band, resulting in mismatch losses exceeding 20dB. Real-world measurements on channel A1 sit around -100dBm. Sourcing a properly sized VHF antenna is the single most impactful hardware upgrade for this use case.

---

## ⚠️ Step 1: Verify Connector Type (Must be done before buying; getting this wrong voids the purchase)

**Checking only "pin or socket in the center" is insufficient**, because SMA and RP-SMA reverse center pins and sockets. You must **inspect both the threads and center contact simultaneously**:

| Chassis Jack Appearance | Type | Matching Antenna Plug to Buy |
|---|---|---|
| **External threads** + center **socket (hole)** | SMA Female (SMA-K) | SMA Male (SMA-J): Internal threads + center pin |
| **External threads** + center **pin** | **RP-SMA Female** | **RP-SMA Male**: Internal threads + center hole |
| Internal threads + center pin | SMA Male (SMA-J) | SMA Female (SMA-K) |
| Internal threads + center hole | **RP-SMA Male** | **RP-SMA Female** |

> 📌 **Early notes previously documented the Cap LoRa-1262 chassis connector as RP-SMA**, but you must **visually verify your physical hardware once** before ordering. On-board connectors typically feature external threads:
> External threads + center pin = RP-SMA. In this scenario, buy an **RP-SMA antenna**, which does not connect directly to standard handheld transceiver antennas (SMA) and requires an `SMA to RP-SMA` adapter.

**[Fill in before purchase] Connector on hardware is: ____________ , so antenna to purchase must be: ____________**

If confirmed to be RP-SMA, the most flexible and cost-effective approach is purchasing SMA antennas along with an `RP-SMA Male to SMA Female` adapter, as standard SMA antennas are far more abundant and economical.

---

## Strict Requirements (Disqualify if not met)

1. **Band must cover 162MHz**. Acceptable ratings:
   - `136–174MHz` (Standard 2m VHF handheld band) — Most common, 162MHz sits comfortably in upper third
   - `156–163MHz` (Marine VHF) — Ideal; matches AIS native band
   - `144–148MHz` (Amateur 2m band) — Marginally usable, tuned slightly too low
2. **Connector**: Per Step 1 above. **Do not mix up polarities**.
3. **Radiator physical length ≥ 35cm**. λ/4 at 162MHz = 46.3cm. Anything shorter than 35cm suffers extremely poor radiation efficiency on this band, **regardless of claimed dBi figures**.
4. **Must be a passive antenna**. Exclude active antennas with integrated LNAs or amplifiers requiring DC power—the device lacks bias-tee power feed circuitry.
5. 50Ω impedance.

## Priority Ranking

1. **Telescopic / Whip Antennas (Adjustable length) — Highest Priority.**
   Allows adjusting length in real time while watching device signal readings, tuning precisely to the real-world optimum near 46.3cm without relying on factory tuning claims.
2. Flexible rubber whips (38–50cm, such as NA-771 variants).
3. Mobile magnetic-mount VHF antenna + base + feedline. Delivers the best performance (metal ground plane effect), but requires feedline length ≤ 4m terminating in the required connector.

## Budget & Quantity

- Unit cost: 1–10 CNY (~$1–$3), total budget under 30 CNY (~$5)
- **Purchase 4–6 antennas of varying types/lengths rather than just one** — quality variance at this price tier is common; empirical filtering across multiple candidates is the intended strategy.
- Include 1–2 adapters of each type: `SMA-K to SMA-J`, `SMA-J to SMA-K`, `SMA to RP-SMA`, `SMA to SL16 (UHF/PL-259)`, and `SMA to alligator clips` (or raw SMA pigtails for custom wire experiments).

## Search Keywords

```
VHF telescopic antenna SMA
Handheld whip antenna 136-174MHz
Marine VHF antenna 156-163
Marine VHF antenna SMA
Magnetic mount VHF antenna 136-174
NA-771 antenna
```

## Questions to Clarify with Vendors

1. What is the exact frequency range? (Request exact numbers; reject "all-band" or "universal" claims)
2. What is the exact connector model? SMA-J / SMA-K / RP-SMA?
3. What is the total physical length? Minimum and maximum lengths for telescopic models?
4. Is it passive or active? Does it require DC power?
5. Is the impedance 50Ω?
6. (Magnetic mounts) How long is the coax cable and what plug terminates it?

**Switch vendors if they cannot clearly answer questions 1 and 2** — inability to specify these usually indicates generic white-label resellers.

## Explicitly Excluded

- Antennas <20cm ("mini", "stubby", or "stealth") — physically ineffective at 162MHz
- Short antennas with unrealistic gain claims (≥8dBi) — physically contradictory false specs
- Wi-Fi / Router antennas (2.4GHz / 5GHz)
- Car broadcast radio antennas (FM 88–108MHz)
- GPS / BeiDou antennas (1.5GHz, typically active)
- LoRa 868 / 915MHz antennas (the stock antenna being replaced)
- Decorative or dummy antennas

## Output Requirements (For Procurement)

Present 4–6 candidates, detailing each:

| Item | Details |
|---|---|
| Link / Store | |
| Unit Price | |
| Nominal Frequency Band | |
| Connector Model (Gender / Polarity) | |
| Physical Length (Range for telescopic) | |
| Passive / Active | |
| Vendor Confirmation Summary | Notes or screenshots |
| Recommendation & Identified Risks | |

Summarize how these 4–6 choices span distinct design approaches to avoid duplicate models.

---

## Post-Delivery Validation (Benchmarking on Hardware)

The firmware's AIS guard mode functions as an effective antenna comparator, providing far more trustworthy data than vendor ratings because it directly measures the exact frequencies and frontend hardware.

### 1. Weed Out Dead Antennas (5 seconds)

Enter LoRa → SCAN → press `v` to switch to VHF, and observe noise floor. **Connecting an antenna must noticeably elevate the noise floor** (as environmental noise enters through the radiator). If readings remain unchanged with or without the antenna, the unit lacks internal electrical connection and should be discarded.

### 2. Side-by-Side Comparison

Run each antenna through identical steps:

1. Press `v` for VHF → press `a` to start guard mode (automatically scans for a baseline reference in the first second).
2. Ensure **`DEBUG OFF`** is set (the debug bar obscures bottom telemetry), or monitor via USB serial.
3. Observe for 3 minutes and record the bottom line: `avg±N  d±N  up N  dn N  pk N`

Interpretation:

- The best antenna exhibits **highest `pk`, highest `up`, with `dn` remaining at 0**.
- `dn` catching up to `up` indicates the antenna is primarily picking up noise spikes. Note that `up` reflects energy threshold excursions rather than demodulated packet counts (this mode measures raw RSSI energy without FSK demodulation).
- `avg` represents continuous elevation of the AIS channel relative to the quiet baseline, and hovers near 0 in pure thermal noise.

For telescopic antennas, adjust extension lengths in steps to discover the optimal real-world length. While the theoretical quarter-wave length is 46.3cm, ground plane interactions and casing influence may shift this by several centimeters.

### 3. Measurement Precautions

- **Antennas must remain vertical**. AIS signals are vertically polarized; horizontal orientation incurs >10dB cross-polarization loss.
- **Maintain identical device positioning and orientation**. Shifting location often alters RF environments more than swapping antennas, invalidating comparative readings.
- VHF propagation relies on line-of-sight: position the device with open sightlines toward water bodies and away from reinforced concrete walls.

---

## ⚠️ Safety Warning

[`HARDWARE.md`](../HARDWARE.md) explicitly warns: "Connect antenna before powering on: the Cap LoRa-1262 frontend cannot withstand unloaded transmissions, which cause permanent damage."
This rule applies equally to **severe impedance mismatch**:

**After installing a VHF antenna, NEVER enter LoRa CHAT mode.** CHAT transmits live packets on 868MHz; transmitting into a 162MHz antenna presents a severe impedance mismatch approaching an open load, risking RF power amplifier burnout.

AIS guard mode (`a`) and SCAN operate purely in receive mode and are entirely safe.

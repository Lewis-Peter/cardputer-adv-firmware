**English** | [简体中文](README.zh-CN.md)

# GNSS Coordinate Conversion, Trip Statistics, and GSA Parsing Testbed

Core math and protocol parsing logic in `src/gnss.cpp`:
1. **Coordinate Conversion**:
   - Degrees Minutes Seconds (`formatDms`): Sign handling, 0 degrees, ±180 degrees, and 59.96" rounding carry protection (preventing invalid outputs like 60.0").
   - Maidenhead 6-character Grid (`toMaidenhead`): Verified against Shanghai (PM01rf), Greenwich (IO91xl), Sydney Opera House (QF56od), Times Square (FN30as), with extreme boundary clamping protection.
   - UTM Projection (`toUtm`): WGS-84 ellipsoid Gauss projection series expansion; verified against Shanghai (51N), Sydney (56S Southern Hemisphere false northing +10,000,000m), Greenwich (30N), within 1 meter error; 80°S ~ 84°N out-of-range protection and central meridian zone boundary handling.
2. **Trip Statistics and Spike Filtering** (`gnssTripProcessPoint`):
   - Stationary drift filtering (speed < 3.0 km/h, distance and duration remain 0).
   - Constant speed motion (36 km/h, distance and duration accumulate normally).
   - Transient spikes (1000m outlier jumps discarded, not counted in distance).
   - Signal loss recovery and Time To First Fix (TTFF) tracking.
3. **GSA Sentence Parsing** (`gnssProcessGSA`):
   - GPGSA / BDGSA active satellite list merging and statistics.
   - NMEA 4.10 systemId (1=GPS, 4=BDS) extension field parsing.
   - No-fix state (fix=1) clears active satellites, with corrupted packet protection.

## How to Run

```bash
cd tools/gnsstest && ./build.sh
# Enable AddressSanitizer and UndefinedBehaviorSanitizer:
SAN=1 ./build.sh
```

**English** | [简体中文](map-offline-todo.zh-CN.md)

# Offline Conventional Map Plan & Roadmap (GNSS Map Offline Plan)

> **Creation Date**: 2026-09-29  
> **Target Hardware**: M5Stack Cardputer (ESP32-S3FN8, no external PSRAM, 320KB SRAM, 8MB Flash)  
> **Related Code**: [`src/gnss.cpp`](../src/gnss.cpp), [`tools/download_tiles.py`](../tools/download_tiles.py)

---

## 1. Background & Design Decisions

### Why Replace Legacy Online-Only Satellite Imagery?
- **Outdoor Offline Pain Point**: As a portable handheld device, Cardputer often lacks stable Wi-Fi during outdoor or in-vehicle use. The legacy implementation's reliance on online satellite tiles resulted in loading hangs, dropped frames, or fallback to coarse world outlines without street details.
- **Demand for Standard Roadmaps**: Satellite imagery without labels offers limited legibility (trails, rivers, and place names are difficult to discern). Standard **light/dark street vector tiles (OpenStreetMap / Carto Dark / standard road networks)** are much more practical.
- **Breaking the Memory Wall**: On hardware lacking PSRAM, decoding PNG tiles requires a 32KB contiguous LZ77 sliding window, frequently triggering OOM crashes. Pre-processing tiles into standard JPEG on a PC and storing them on MicroSD enables Cardputer to stream decode using `tjpgd` requiring only ~3KB buffer, ensuring zero-OOM local instant loading.

---

## 2. Nationwide Map Data Volume & Feasibility Analysis

Calculations based on Web Mercator tile coverage for mainland China (~2.2% of global land area):

| Zoom Level | Scale Metric | Viewport Width | Total Tiles (China) | Storage Size (JPEG) | FAT32 Feasibility & Assessment |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Zoom 5** | National / Provinces | ~1,200 km | ~30 | ~300 KB | **Instant download, negligible size** |
| **Zoom 8** | Prefectures / Regions | ~150 km | ~1,500 | ~15 MB | **Completes in 1 minute** |
| **Zoom 11** | Counties / National Highways | ~18 km | ~95,000 | ~750 MB | **Strongly recommended for full offline storage** (Highways/provincial roads/waterways) |
| **Zoom 14** | Towns / Streets | ~2.3 km | ~6,000,000 | ~45 GB | ⚠️ **Exceeds FAT32 4.19M total file count limit** |
| **Zoom 16** | Neighborhoods / Alleys | ~570 m | ~96,000,000 | ~750 GB | ❌ **File count explosion; servers will rate limit; infeasible as loose files** |

### Conclusions & Recommended Strategy
- **Macro National Base Map**: Download all tiles for `Zoom 5 + Zoom 8 + Zoom 11` (~96,000 tiles, under 800MB), ensuring national highways and county boundaries everywhere.
- **Targeted Local Street Maps**: Download `Zoom 14 + Zoom 16` for home cities, travel destinations, or off-road exploration zones (~20,000–40,000 tiles per city, only 150–300MB).

---

## 3. Completed Features

- [x] **Firmware Offline Tile Indexing and Rendering** ([`src/gnss.cpp`](../src/gnss.cpp)):
  - Supports standard directories and naming conventions: `/map/{z}/{x}/{y}.jpg`, `/tiles/{z}/{x}/{y}.jpg`, `/map/{z}/{x}_{y}.jpg`.
  - When MicroSD is inserted, bypasses Wi-Fi checks and schedules `JOB_TILE` directly, loading within tens of milliseconds.
  - Hybrid fallback chain: reads SD if present; falls back to online HTTP fetch if SD tile is missing and Wi-Fi is connected; drops back to built-in vector mask if both fail.
- [x] **Automatic Coordinate System Calibration (WGS-84 vs GCJ-02)**:
  - Detects `/map/wgs84` or `/map/osm` marker files on SD; projects directly with standard WGS-84 when present, or applies GCJ-02 offset for domestic Chinese tile providers.
- [x] **Smart Dark Theme Protection & Contrast Enhancement**:
  - 16-point tile luminance sampling: dark themes like Carto Dark bypass darkening; light daytime street maps are softly dimmed to ensure GPS markers, trails, scale indicators, and status bars remain distinct.
- [x] **Zoom Tier Expansion**:
  - `ZOOMS[] = {0, 5, 8, 11, 14, 16}`, supporting zoom in down to street block level (z16, ~570m viewport width).
  - Status bar displays `z.. SD` indicator.
- [x] **Companion Offline Tile Tool** ([`tools/download_tiles.py`](../tools/download_tiles.py)):
  - Download by city name (automatic OSM Nominatim geocoding) or lat/lon + radius.
  - Styles supported: `dark` (CartoDB dark street network), `osm` (standard OSM street map), `amap` (AutoNavi vector streets), `amap-sat` (AutoNavi satellite).
  - Automatic PNG-to-JPEG conversion and coordinate marker file generation.

---

## 4. TODO List

### Phase 1: Data Preparation & Hardware Testing (Near-Term)
- [ ] **Prepare Reference National Base Map & Local Test Packages**:
  - [ ] Run `python3 tools/download_tiles.py --lat 35.0 --lon 105.0 --radius 2500km --zooms 5,8 --style dark --out ./map_china` to generate the national backbone package.
  - [ ] Download complete `z11, z14, z16` street networks for 1–2 target cities (e.g. Hangzhou / Beijing / Shenzhen).
  - [ ] Copy to TF card and verify zoom step (`[` / `]`) transitions and GPS marker alignment on device.
- [ ] **Pan / Scroll Map Navigation**:
  - [ ] Currently the map center locks to the GPS fix position; evaluate adding a free-browse mode (panning the map center via modifier key + arrow keys to explore surrounding areas).

### Phase 2: Single-File Archive Storage Exploration (Mid/Long-Term, Solving Small-File Overhead)
- [ ] **Single-File Archive Storage Investigation (.pak / .bin / Compact SQLite)**:
  - *Motivation*: Storing high-zoom tiles across dozens of cities leads to tens of thousands of loose files, resulting in slow FAT32 file operations and heavy inode overhead.
  - *Approach*: Design a lightweight **read-only single-file indexed format** (Magic Header + Index Table `[z, x, y, offset, length]` + JPEG Data Stream):
    - Load a small ~tens-of-KB index into RAM on boot (or binary search index on SD via `f.seek()`), then stream-decompress tiles with direct `f.seek(offset)`.
    - Add a `--pack` option to the PC script to output single archives like `china.pak` or `hangzhou.pak`.

### Phase 3: Outdoor & Tactical Navigation Extensions
- [ ] **GPX Track Import & Route Overlay**:
  - [ ] Support loading `.gpx` tracks from `/gpx/` on SD, rendering planned routes as bold highlighted polylines over offline maps.
  - [ ] Display micro icons or waypoint labels on the base map.
- [ ] **Heading-up Dynamic Orientation Evaluation**:
  - [ ] Currently maps are fixed North-up; evaluate providing an optional heading-up compass rotation mode when moving speed > 3 km/h.

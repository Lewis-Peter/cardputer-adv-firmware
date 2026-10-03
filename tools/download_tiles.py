#!/usr/bin/env python3
"""
Cardputer ADV - Offline Map Tile Downloader
Downloads traditional street/road map tiles (OSM, Carto Dark, AutoNavi)
and converts them into lightweight JPEG tiles organized in /map/{z}/{x}/{y}.jpg
ready to be copied to an SD card for Cardputer GNSS Map.

Usage Examples:
  # Download around coordinates (e.g. Beijing Tiananmen, 5km radius, zoom 11, 14, 16):
  python3 tools/download_tiles.py --lat 39.9042 --lon 116.4074 --radius 5km --style dark --out ./sd_map

  # Download by city name:
  python3 tools/download_tiles.py --city "Hangzhou" --style osm --out ./sd_map

  # Download for an SD card directly:
  python3 tools/download_tiles.py --lat 31.2304 --lon 121.4737 --radius 8km --out /media/sd/map
"""

import os
import sys
import math
import time
import argparse
import urllib.request
import urllib.parse
import json
from concurrent.futures import ThreadPoolExecutor
from io import BytesIO

try:
    from PIL import Image
except ImportError:
    print("[!] PIL/Pillow is required for image format conversion.")
    print("    Install it via: pip install Pillow")
    sys.exit(1)

# Default zoom levels matching Cardputer ZOOMS
DEFAULT_ZOOMS = [5, 8, 11, 14, 16]

SOURCES = {
    "dark": {
        "name": "CartoDB Dark Matter (Dark Cyber Street)",
        "url": "https://a.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png",
        "coord": "wgs84",
        "headers": {"User-Agent": "Mozilla/5.0 (Cardputer-Tile-Downloader)"}
    },
    "osm": {
        "name": "OpenStreetMap Standard (Light Street Map)",
        "url": "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
        "coord": "wgs84",
        "headers": {"User-Agent": "CardputerADV-TileDownloader/1.0"}
    },
    "amap": {
        "name": "AutoNavi Road Map (高德路网矢量)",
        "url": "http://wprd01.is.autonavi.com/appmaptile?style=7&x={x}&y={y}&z={z}",
        "coord": "gcj02",
        "headers": {"User-Agent": "Mozilla/5.0"}
    },
    "amap-sat": {
        "name": "AutoNavi Satellite (高德卫星影像)",
        "url": "http://webst01.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}",
        "coord": "gcj02",
        "headers": {"User-Agent": "Mozilla/5.0"}
    }
}

# Coordinate conversion WGS-84 <-> GCJ-02
GCJ_A = 6378245.0
GCJ_EE = 0.00669342162296594323

def out_of_china(lat, lon):
    return not (73.66 < lon < 135.05 and 3.86 < lat < 53.55)

def gcj_transform_lat(x, y):
    r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * math.sqrt(abs(x))
    r += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    r += (20.0 * math.sin(y * math.pi) + 40.0 * math.sin(y / 3.0 * math.pi)) * 2.0 / 3.0
    r += (160.0 * math.sin(y / 12.0 * math.pi) + 320.0 * math.sin(y * math.pi / 30.0)) * 2.0 / 3.0
    return r

def gcj_transform_lon(x, y):
    r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * math.sqrt(abs(x))
    r += (20.0 * math.sin(6.0 * x * math.pi) + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    r += (20.0 * math.sin(x * math.pi) + 40.0 * math.sin(x / 3.0 * math.pi)) * 2.0 / 3.0
    r += (150.0 * math.sin(x / 12.0 * math.pi) + 300.0 * math.sin(x / 30.0 * math.pi)) * 2.0 / 3.0
    return r

def wgs2gcj(lat, lon):
    if out_of_china(lat, lon):
        return lat, lon
    d_lat = gcj_transform_lat(lon - 105.0, lat - 35.0)
    d_lon = gcj_transform_lon(lon - 105.0, lat - 35.0)
    rad_lat = lat / 180.0 * math.pi
    magic = math.sin(rad_lat)
    magic = 1 - GCJ_EE * magic * magic
    sqrt_magic = math.sqrt(magic)
    d_lat = (d_lat * 180.0) / ((GCJ_A * (1 - GCJ_EE)) / (magic * sqrt_magic) * math.pi)
    d_lon = (d_lon * 180.0) / (GCJ_A / sqrt_magic * math.cos(rad_lat) * math.pi)
    return lat + d_lat, lon + d_lon

def deg2num(lat_deg, lon_deg, zoom):
    lat_rad = math.radians(lat_deg)
    n = 2.0 ** zoom
    xtile = int((lon_deg + 180.0) / 360.0 * n)
    ytile = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return xtile, ytile

def geocode_city(city_name):
    print(f"[*] Querying geocoding for '{city_name}'...")
    url = f"https://nominatim.openstreetmap.org/search?q={urllib.parse.quote(city_name)}&format=json&limit=1"
    req = urllib.request.Request(url, headers={"User-Agent": "CardputerTileDownloader/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode())
            if not data:
                print(f"[!] No location found for '{city_name}'")
                return None, None
            lat = float(data[0]["lat"])
            lon = float(data[0]["lon"])
            display_name = data[0].get("display_name", city_name)
            print(f"[+] Found: {display_name} ({lat:.4f}, {lon:.4f})")
            return lat, lon
    except Exception as e:
        print(f"[!] Geocoding error: {e}")
        return None, None

def download_and_convert_tile(z, x, y, source_cfg, out_dir, quality=80):
    dest_path = os.path.join(out_dir, str(z), str(x), f"{y}.jpg")
    if os.path.exists(dest_path) and os.path.getsize(dest_path) > 100:
        return True, "cached"

    url = source_cfg["url"].format(z=z, x=x, y=y)
    req = urllib.request.Request(url, headers=source_cfg.get("headers", {}))

    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = resp.read()
                # If already JPEG, check or re-compress
                im = Image.open(BytesIO(data))
                if im.mode != "RGB":
                    im = im.convert("RGB")
                os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                im.save(dest_path, "JPEG", quality=quality, optimize=True)
                return True, "downloaded"
        except Exception as e:
            time.sleep(0.5 * (attempt + 1))

    return False, "failed"

def main():
    parser = argparse.ArgumentParser(description="Cardputer ADV Offline Map Tile Downloader")
    parser.add_argument("--lat", type=float, help="Center latitude (WGS-84)")
    parser.add_argument("--lon", type=float, help="Center longitude (WGS-84)")
    parser.add_argument("--city", type=str, help="City name (auto-geocoded)")
    parser.add_argument("--radius", type=str, default="5km", help="Download radius, e.g. 5km, 10km (default: 5km)")
    parser.add_argument("--style", choices=["dark", "osm", "amap", "amap-sat"], default="dark",
                        help="Map style: dark (CartoDB dark), osm (standard OSM), amap (AutoNavi road), amap-sat (AutoNavi satellite)")
    parser.add_argument("--zooms", type=str, default="8,11,14,16",
                        help="Comma-separated zoom levels (default: 8,11,14,16)")
    parser.add_argument("--out", type=str, default="./map", help="Output directory (e.g. /media/sd/map or ./map)")
    parser.add_argument("--quality", type=int, default=80, help="JPEG quality (1-95, default: 80)")
    parser.add_argument("--threads", type=int, default=6, help="Download threads (default: 6)")

    args = parser.parse_args()

    lat = args.lat
    lon = args.lon
    if args.city:
        clat, clon = geocode_city(args.city)
        if clat is None:
            sys.exit(1)
        lat, lon = clat, clon

    if lat is None or lon is None:
        print("[!] Please provide --lat/--lon or --city. Run with -h for help.")
        sys.exit(1)

    # Parse radius in km
    radius_km = 5.0
    r_str = args.radius.lower().replace("km", "").strip()
    try:
        radius_km = float(r_str)
    except ValueError:
        pass

    zooms = [int(z.strip()) for z in args.zooms.split(",") if z.strip().isdigit()]
    source_cfg = SOURCES[args.style]

    # Convert coordinates if source is GCJ-02 (AutoNavi)
    tile_lat, tile_lon = lat, lon
    if source_cfg["coord"] == "gcj02":
        tile_lat, tile_lon = wgs2gcj(lat, lon)
        print(f"[*] Converted WGS-84 ({lat:.5f}, {lon:.5f}) -> GCJ-02 ({tile_lat:.5f}, {tile_lon:.5f})")

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    # Write marker file so Cardputer knows coordinate system
    if source_cfg["coord"] == "wgs84":
        marker_file = os.path.join(out_dir, "wgs84")
        with open(marker_file, "w") as mf:
            mf.write("wgs84\n")
        print(f"[+] Created coordinate marker: {marker_file}")
    else:
        # If GCJ-02, remove wgs84 marker if present
        marker_file = os.path.join(out_dir, "wgs84")
        if os.path.exists(marker_file):
            os.remove(marker_file)

    # Calculate tiles to download
    tasks = []
    lat_delta = radius_km / 111.0
    lon_delta = radius_km / (111.0 * max(0.1, math.cos(math.radians(lat))))

    for z in zooms:
        x_min, y_max = deg2num(tile_lat - lat_delta, tile_lon - lon_delta, z)
        x_max, y_min = deg2num(tile_lat + lat_delta, tile_lon + lon_delta, z)
        if x_min > x_max: x_min, x_max = x_max, x_min
        if y_min > y_max: y_min, y_max = y_max, y_min

        # Clamp max tiles for high zoom to avoid excessive downloads
        tile_count = (x_max - x_min + 1) * (y_max - y_min + 1)
        print(f"[*] Zoom {z:2d}: X=[{x_min}..{x_max}], Y=[{y_min}..{y_max}] ({tile_count} tiles)")
        for x in range(x_min, x_max + 1):
            for y in range(y_min, y_max + 1):
                tasks.append((z, x, y))

    print(f"\n[+] Total tiles to download: {len(tasks)}")
    print(f"    Target directory: {out_dir}")
    print(f"    Map Source: {source_cfg['name']}")

    if len(tasks) == 0:
        print("[!] No tiles to download.")
        return

    # Download in thread pool
    done_count = 0
    cached_count = 0
    failed_count = 0

    def worker(item):
        nonlocal done_count, cached_count, failed_count
        z, x, y = item
        ok, status = download_and_convert_tile(z, x, y, source_cfg, out_dir, quality=args.quality)
        if ok:
            if status == "cached":
                cached_count += 1
            else:
                done_count += 1
        else:
            failed_count += 1

        total = len(tasks)
        curr = done_count + cached_count + failed_count
        pct = (curr * 100) // total
        print(f"\rProgress: [{curr}/{total}] {pct}% (New: {done_count}, Cached: {cached_count}, Failed: {failed_count})", end="", flush=True)

    print("\nStarting tile download...")
    with ThreadPoolExecutor(max_workers=args.threads) as pool:
        pool.map(worker, tasks)

    print("\n\n" + "=" * 50)
    print("Download completed!")
    print(f"  Saved to: {out_dir}")
    print(f"  New: {done_count}, Cached: {cached_count}, Failed: {failed_count}")
    print("\n[Cardputer Usage Guide]")
    print(f"1. Copy the folder '{os.path.basename(out_dir)}' to your Cardputer TF/SD card root directory:")
    print("   /sdcard/map/{z}/{x}/{y}.jpg")
    print("2. Insert SD card into Cardputer and open GNSS Map (App: MAP).")
    print("3. You will see '[z.. SD]' in the top header, indicating offline traditional map is active!")
    print("=" * 50)

if __name__ == "__main__":
    main()

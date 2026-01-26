#!/usr/bin/env python3
"""
Camera Database Generator for V1 Simple

Downloads camera data from authoritative sources and generates SD card files.
Output files go in the current directory for easy copying to SD card.

Sources:
  - ALPR: OpenStreetMap via Overpass API (DeFlock community data)
  - Red Light: POI Factory
  - Speed: POI Factory

Usage:
    python3 tools/update_cameras.py              # Generate all 3 files
    python3 tools/update_cameras.py --type alpr  # Generate only ALPR
    python3 tools/update_cameras.py --output ./  # Specify output directory

Output files (NDJSON format):
    alpr.json        - ALPR/Flock cameras
    redlight_cam.json - Red light cameras  
    speed_cam.json    - Speed cameras
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    print("Installing requests...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "requests", "-q"])
    import requests

# API endpoints
OVERPASS_URL = "https://overpass-api.de/api/interpreter"

# POI Factory URLs (direct CSV downloads) - Note: HTTP only, no HTTPS support
POI_FACTORY_REDLIGHT = "http://www.poi-factory.com/sites/default/files/poifiles/redlightcamera.csv"
POI_FACTORY_SPEED = "http://www.poi-factory.com/sites/default/files/poifiles/speedcamera.csv"

def log(msg):
    print(f"[cameras] {msg}")

def download_alpr_from_osm() -> list:
    """Download ALPR cameras from OpenStreetMap via Overpass API (DeFlock data)"""
    log("Downloading ALPR cameras from OpenStreetMap...")
    
    # Query for surveillance cameras tagged as ALPR in USA
    # DeFlock community adds these to OSM with specific tags
    query = """
    [out:json][timeout:180];
    area["ISO3166-1"="US"]->.usa;
    (
      node["man_made"="surveillance"]["surveillance:type"="ALPR"](area.usa);
      node["man_made"="surveillance"]["surveillance"="ALPR"](area.usa);
      node["surveillance:type"="ALPR"](area.usa);
    );
    out body;
    """
    
    try:
        response = requests.post(
            OVERPASS_URL,
            data={'data': query},
            timeout=300,
            headers={'User-Agent': 'V1Simple/1.0 (camera database generator)'}
        )
        response.raise_for_status()
        data = response.json()
        
        cameras = []
        seen = set()
        
        for el in data.get('elements', []):
            lat, lon = el.get('lat'), el.get('lon')
            if not lat or not lon:
                continue
                
            key = f"{lat:.5f},{lon:.5f}"
            if key in seen:
                continue
            seen.add(key)
            
            cameras.append({
                "lat": round(lat, 6),
                "lon": round(lon, 6),
                "flg": 8192  # Bit 13 = ALPR
            })
        
        log(f"  ✓ {len(cameras)} ALPR cameras")
        return cameras
        
    except requests.exceptions.Timeout:
        log("  ✗ Overpass API timeout (try again later)")
        return []
    except Exception as e:
        log(f"  ✗ Error: {e}")
        return []

def parse_poi_factory_csv(content: str) -> list:
    """Parse POI Factory CSV format: lon,lat,name"""
    cameras = []
    seen = set()
    
    for line in content.strip().split('\n'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        
        parts = line.split(',', 2)
        if len(parts) < 2:
            continue
            
        try:
            # POI Factory format is: longitude, latitude, name
            lon = float(parts[0].strip().strip('"'))
            lat = float(parts[1].strip().strip('"'))
            
            # Basic validation
            if not (-180 <= lon <= 180 and -90 <= lat <= 90):
                continue
                
            key = f"{lat:.5f},{lon:.5f}"
            if key in seen:
                continue
            seen.add(key)
            
            cam = {"lat": round(lat, 6), "lon": round(lon, 6)}
            
            # Extract speed limit if in name
            if len(parts) > 2:
                name = parts[2].strip().strip('"')
                # Try to find speed limit in name (e.g., "Main St - 35 MPH")
                import re
                speed_match = re.search(r'(\d{2,3})\s*(?:mph|MPH)', name)
                if speed_match:
                    cam["spd"] = int(speed_match.group(1))
            
            cameras.append(cam)
        except (ValueError, IndexError):
            continue
    
    return cameras

def download_redlight_from_osm() -> list:
    """Download red light cameras from OpenStreetMap via Overpass API"""
    log("Downloading red light cameras from OpenStreetMap...")
    
    # Use enforcement relation type which is the standard OSM tagging for red light cameras
    # Area ID 3600148838 = United States (relation 148838 + 3600000000)
    query = """
    [out:json][timeout:300];
    area(3600148838)->.a;
    (
      relation["type"="enforcement"]["enforcement"="traffic_signals"](area.a);
    );
    out tags center;
    """
    
    try:
        response = requests.post(
            OVERPASS_URL,
            data={'data': query},
            timeout=360,
            headers={'User-Agent': 'V1Simple/1.0 (camera database generator)'}
        )
        response.raise_for_status()
        data = response.json()
        
        cameras = []
        seen = set()
        
        for el in data.get('elements', []):
            # Handle both nodes (lat/lon) and relations (center.lat/center.lon)
            if el.get('type') == 'relation' and 'center' in el:
                lat, lon = el['center'].get('lat'), el['center'].get('lon')
            else:
                lat, lon = el.get('lat'), el.get('lon')
            
            if not lat or not lon:
                continue
                
            key = f"{lat:.5f},{lon:.5f}"
            if key in seen:
                continue
            seen.add(key)
            
            cameras.append({
                "lat": round(lat, 6),
                "lon": round(lon, 6),
                "flg": 2  # Bit 1 = red light
            })
        
        log(f"  ✓ {len(cameras)} red light cameras from OSM")
        return cameras
        
    except requests.exceptions.Timeout:
        log("  ✗ Overpass API timeout")
        return []
    except Exception as e:
        log(f"  ✗ Error: {e}")
        return []

def download_speed_from_osm() -> list:
    """Download speed cameras from OpenStreetMap via Overpass API"""
    log("Downloading speed cameras from OpenStreetMap...")
    
    query = """
    [out:json][timeout:180];
    area["ISO3166-1"="US"]->.usa;
    (
      node["highway"="speed_camera"](area.usa);
      node["enforcement"="maxspeed"](area.usa);
    );
    out body;
    """
    
    try:
        response = requests.post(
            OVERPASS_URL,
            data={'data': query},
            timeout=300,
            headers={'User-Agent': 'V1Simple/1.0 (camera database generator)'}
        )
        response.raise_for_status()
        data = response.json()
        
        cameras = []
        seen = set()
        
        for el in data.get('elements', []):
            lat, lon = el.get('lat'), el.get('lon')
            if not lat or not lon:
                continue
                
            key = f"{lat:.5f},{lon:.5f}"
            if key in seen:
                continue
            seen.add(key)
            
            tags = el.get('tags', {})
            cam = {
                "lat": round(lat, 6),
                "lon": round(lon, 6),
                "flg": 1  # Bit 0 = speed
            }
            
            # Try to get speed limit
            maxspeed = tags.get('maxspeed', '')
            if maxspeed:
                import re
                speed_match = re.search(r'(\d+)', maxspeed)
                if speed_match:
                    cam["spd"] = int(speed_match.group(1))
            
            cameras.append(cam)
        
        log(f"  ✓ {len(cameras)} speed cameras from OSM")
        return cameras
        
    except requests.exceptions.Timeout:
        log("  ✗ Overpass API timeout")
        return []
    except Exception as e:
        log(f"  ✗ Error: {e}")
        return []

def download_redlight_from_poi_factory() -> list:
    """Download red light cameras from POI Factory"""
    log("Downloading red light cameras from POI Factory...")
    
    try:
        response = requests.get(
            POI_FACTORY_REDLIGHT,
            timeout=60,
            headers={'User-Agent': 'V1Simple/1.0 (camera database generator)'}
        )
        response.raise_for_status()
        
        cameras = parse_poi_factory_csv(response.text)
        
        # Add red light flag
        for cam in cameras:
            cam["flg"] = 2  # Bit 1 = red light
        
        log(f"  ✓ {len(cameras)} red light cameras")
        return cameras
        
    except Exception as e:
        log(f"  ✗ Error: {e}")
        return []

def download_speed_from_poi_factory() -> list:
    """Download speed cameras from POI Factory"""
    log("Downloading speed cameras from POI Factory...")
    
    try:
        response = requests.get(
            POI_FACTORY_SPEED,
            timeout=60,
            headers={'User-Agent': 'V1Simple/1.0 (camera database generator)'}
        )
        response.raise_for_status()
        
        cameras = parse_poi_factory_csv(response.text)
        
        # Add speed flag
        for cam in cameras:
            cam["flg"] = 1  # Bit 0 = speed
        
        log(f"  ✓ {len(cameras)} speed cameras")
        return cameras
        
    except Exception as e:
        log(f"  ✗ Error: {e}")
        return []

def save_cameras(cameras: list, filepath: Path, cam_type: str):
    """Save cameras to NDJSON file"""
    if not cameras:
        log(f"  ⚠ No {cam_type} cameras to save")
        return False
    
    with open(filepath, 'w') as f:
        for cam in cameras:
            f.write(json.dumps(cam, separators=(',', ':')) + '\n')
    
    size_kb = filepath.stat().st_size / 1024
    log(f"  → Saved {filepath.name} ({len(cameras)} cameras, {size_kb:.1f} KB)")
    return True

def main():
    parser = argparse.ArgumentParser(description='Generate V1 Simple camera database files')
    parser.add_argument('--type', choices=['all', 'alpr', 'redlight', 'speed'], default='all',
                       help='Camera type to download (default: all)')
    parser.add_argument('--output', type=str, default='.',
                       help='Output directory (default: current directory)')
    args = parser.parse_args()
    
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    today = datetime.now().strftime('%Y-%m-%d')
    log(f"Camera Database Generator - {today}")
    log(f"Output directory: {output_dir.absolute()}")
    print()
    
    success_count = 0
    
    # ALPR cameras
    if args.type in ('all', 'alpr'):
        cameras = download_alpr_from_osm()
        if save_cameras(cameras, output_dir / 'alpr.json', 'ALPR'):
            success_count += 1
        if args.type == 'all':
            time.sleep(2)  # Rate limit between API calls
    
    # Red light cameras - try OSM first, then POI Factory
    if args.type in ('all', 'redlight'):
        cameras = download_redlight_from_osm()
        if not cameras:
            log("  Trying POI Factory fallback...")
            cameras = download_redlight_from_poi_factory()
        if save_cameras(cameras, output_dir / 'redlight_cam.json', 'red light'):
            success_count += 1
        if args.type == 'all':
            time.sleep(2)
    
    # Speed cameras - try OSM first, then POI Factory
    if args.type in ('all', 'speed'):
        cameras = download_speed_from_osm()
        if not cameras:
            log("  Trying POI Factory fallback...")
            cameras = download_speed_from_poi_factory()
        if save_cameras(cameras, output_dir / 'speed_cam.json', 'speed'):
            success_count += 1
    
    print()
    if success_count > 0:
        log(f"✅ Generated {success_count} file(s) in {output_dir}")
        log("Copy these files to your SD card root directory")
    else:
        log("❌ No files generated - check network connection")
        return 1
    
    return 0

if __name__ == '__main__':
    sys.exit(main())


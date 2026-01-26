#!/usr/bin/env python3
"""
Download camera database from OpenStreetMap via Overpass API.

This script downloads ALPR, red light, and speed camera data from OpenStreetMap
and converts it to NDJSON format compatible with the V1 Simple camera_manager.

Usage:
    python3 download_cameras.py                    # Download all camera types for US
    python3 download_cameras.py --state CA         # Download for specific state
    python3 download_cameras.py --type alpr        # Download only ALPR cameras
    python3 download_cameras.py --output cameras.json  # Custom output filename

Output file can be:
    1. Copied to SD card as /cameras.json
    2. Uploaded via web UI at /integrations
"""

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    print("Error: 'requests' module not found. Install with: pip3 install requests")
    sys.exit(1)

OVERPASS_URL = "https://overpass-api.de/api/interpreter"

# Camera type flags (matches camera_manager.h)
CAMERA_FLAG_REDLIGHT = 1
CAMERA_FLAG_SPEED = 2
CAMERA_FLAG_REDLIGHT_SPEED = 3
CAMERA_FLAG_ALPR = 4

# US State codes for filtering
US_STATES = {
    'AL': 'Alabama', 'AK': 'Alaska', 'AZ': 'Arizona', 'AR': 'Arkansas',
    'CA': 'California', 'CO': 'Colorado', 'CT': 'Connecticut', 'DE': 'Delaware',
    'FL': 'Florida', 'GA': 'Georgia', 'HI': 'Hawaii', 'ID': 'Idaho',
    'IL': 'Illinois', 'IN': 'Indiana', 'IA': 'Iowa', 'KS': 'Kansas',
    'KY': 'Kentucky', 'LA': 'Louisiana', 'ME': 'Maine', 'MD': 'Maryland',
    'MA': 'Massachusetts', 'MI': 'Michigan', 'MN': 'Minnesota', 'MS': 'Mississippi',
    'MO': 'Missouri', 'MT': 'Montana', 'NE': 'Nebraska', 'NV': 'Nevada',
    'NH': 'New Hampshire', 'NJ': 'New Jersey', 'NM': 'New Mexico', 'NY': 'New York',
    'NC': 'North Carolina', 'ND': 'North Dakota', 'OH': 'Ohio', 'OK': 'Oklahoma',
    'OR': 'Oregon', 'PA': 'Pennsylvania', 'RI': 'Rhode Island', 'SC': 'South Carolina',
    'SD': 'South Dakota', 'TN': 'Tennessee', 'TX': 'Texas', 'UT': 'Utah',
    'VT': 'Vermont', 'VA': 'Virginia', 'WA': 'Washington', 'WV': 'West Virginia',
    'WI': 'Wisconsin', 'WY': 'Wyoming', 'DC': 'District of Columbia'
}


def build_overpass_query(camera_types: list, area_filter: str = None) -> str:
    """Build Overpass QL query for camera data."""
    
    # Area selection
    if area_filter and area_filter.upper() in US_STATES:
        state_name = US_STATES[area_filter.upper()]
        area_line = f'area["name"="{state_name}"]["admin_level"="4"]->.searchArea;'
    else:
        # Entire US
        area_line = 'area["ISO3166-1"="US"]->.searchArea;'
    
    # Build query parts for each camera type
    query_parts = []
    
    if 'alpr' in camera_types:
        query_parts.append('node["surveillance:type"="ALPR"](area.searchArea);')
        query_parts.append('way["surveillance:type"="ALPR"](area.searchArea);')
    
    if 'redlight' in camera_types:
        query_parts.append('node["highway"="speed_camera"]["enforcement"="traffic_signals"](area.searchArea);')
        query_parts.append('node["highway"="traffic_signals"]["camera"="yes"](area.searchArea);')
    
    if 'speed' in camera_types:
        query_parts.append('node["highway"="speed_camera"](area.searchArea);')
        query_parts.append('way["highway"="speed_camera"](area.searchArea);')
    
    query = f"""[out:json][timeout:600];
{area_line}
(
  {chr(10).join('  ' + p for p in query_parts)}
);
out center;
"""
    return query


def parse_osm_element(element: dict, camera_types: list) -> dict:
    """Parse an OSM element into camera record format."""
    
    # Get coordinates
    if element['type'] == 'node':
        lat = element['lat']
        lon = element['lon']
    elif element['type'] == 'way' and 'center' in element:
        lat = element['center']['lat']
        lon = element['center']['lon']
    else:
        return None
    
    tags = element.get('tags', {})
    
    # Determine camera type
    surveillance_type = tags.get('surveillance:type', '').upper()
    highway_tag = tags.get('highway', '')
    enforcement = tags.get('enforcement', '')
    
    if surveillance_type == 'ALPR':
        flag = CAMERA_FLAG_ALPR
    elif enforcement == 'traffic_signals' or tags.get('camera') == 'yes':
        flag = CAMERA_FLAG_REDLIGHT
    elif highway_tag == 'speed_camera':
        # Check if it's also red light
        if 'red_light' in tags.get('enforcement', ''):
            flag = CAMERA_FLAG_REDLIGHT_SPEED
        else:
            flag = CAMERA_FLAG_SPEED
    else:
        flag = CAMERA_FLAG_SPEED  # Default
    
    record = {
        'lat': round(lat, 6),
        'lon': round(lon, 6),
        'flg': flag
    }
    
    # Add speed limit if available
    maxspeed = tags.get('maxspeed', '')
    if maxspeed:
        try:
            # Parse speed limit (might be "35 mph" or just "35")
            speed = int(''.join(c for c in maxspeed.split()[0] if c.isdigit()))
            record['spd'] = speed
            # Check units
            if 'km' in maxspeed.lower():
                record['unt'] = 'kmh'
        except (ValueError, IndexError):
            pass
    
    # Add direction if available
    direction = tags.get('direction', tags.get('camera:direction', ''))
    if direction:
        try:
            dir_val = int(float(direction))
            if 0 <= dir_val <= 360:
                record['dir'] = [dir_val]
        except ValueError:
            # Try parsing cardinal directions
            cardinals = {'N': 0, 'NE': 45, 'E': 90, 'SE': 135, 
                        'S': 180, 'SW': 225, 'W': 270, 'NW': 315}
            if direction.upper() in cardinals:
                record['dir'] = [cardinals[direction.upper()]]
    
    return record


def download_cameras(camera_types: list, state: str = None, verbose: bool = False) -> list:
    """Download camera data from Overpass API."""
    
    query = build_overpass_query(camera_types, state)
    
    if verbose:
        print(f"Query:\n{query}\n")
    
    print(f"Downloading {', '.join(camera_types)} cameras" + 
          (f" for {state.upper()}" if state else " for entire US") + "...")
    print("This may take a few minutes...")
    
    try:
        response = requests.post(
            OVERPASS_URL,
            data={'data': query},
            timeout=600  # 10 minute timeout
        )
        response.raise_for_status()
    except requests.exceptions.Timeout:
        print("Error: Request timed out. Try a smaller area (--state)")
        sys.exit(1)
    except requests.exceptions.RequestException as e:
        print(f"Error: {e}")
        sys.exit(1)
    
    data = response.json()
    elements = data.get('elements', [])
    print(f"Received {len(elements)} elements from OSM")
    
    # Parse elements
    cameras = []
    for el in elements:
        record = parse_osm_element(el, camera_types)
        if record:
            cameras.append(record)
    
    # Deduplicate by location (within ~10m)
    unique_cameras = []
    seen = set()
    for cam in cameras:
        # Round to ~10m precision for dedup
        key = (round(cam['lat'], 4), round(cam['lon'], 4), cam['flg'])
        if key not in seen:
            seen.add(key)
            unique_cameras.append(cam)
    
    print(f"Parsed {len(unique_cameras)} unique cameras")
    
    return unique_cameras


def save_ndjson(cameras: list, output_path: str, camera_types: list, state: str = None):
    """Save cameras to NDJSON file."""
    
    # Create metadata
    type_names = []
    if 'alpr' in camera_types:
        type_names.append('ALPR')
    if 'redlight' in camera_types:
        type_names.append('Red Light')
    if 'speed' in camera_types:
        type_names.append('Speed')
    
    area_name = US_STATES.get(state.upper(), "US") if state else "US"
    
    meta = {
        '_meta': {
            'name': f"OSM {'/'.join(type_names)} ({area_name})",
            'date': datetime.now().strftime('%Y-%m-%d'),
            'count': len(cameras),
            'source': 'OpenStreetMap via Overpass API'
        }
    }
    
    with open(output_path, 'w') as f:
        f.write(json.dumps(meta) + '\n')
        for cam in cameras:
            f.write(json.dumps(cam) + '\n')
    
    print(f"\nSaved to: {output_path}")
    print(f"Total cameras: {len(cameras)}")
    
    # Count by type
    type_counts = {}
    type_names = {1: 'Red Light', 2: 'Speed', 3: 'Red Light + Speed', 4: 'ALPR'}
    for cam in cameras:
        t = cam.get('flg', 2)
        type_counts[t] = type_counts.get(t, 0) + 1
    
    for t, count in sorted(type_counts.items()):
        print(f"  - {type_names.get(t, 'Unknown')}: {count}")


def main():
    parser = argparse.ArgumentParser(
        description='Download camera database from OpenStreetMap',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 download_cameras.py                         # All US cameras
  python3 download_cameras.py --state CA              # California only
  python3 download_cameras.py --type alpr --state TX  # Texas ALPR only
  python3 download_cameras.py --output /path/to/sd/cameras.json

After downloading, copy the file to your SD card as /cameras.json
or upload via the web UI at http://192.168.4.1/integrations
"""
    )
    
    parser.add_argument('--type', '-t', 
                       choices=['all', 'alpr', 'redlight', 'speed'],
                       default='all',
                       help='Camera type to download (default: all)')
    
    parser.add_argument('--state', '-s',
                       type=str,
                       help='US state code (e.g., CA, TX, NY)')
    
    parser.add_argument('--output', '-o',
                       type=str,
                       default='cameras.json',
                       help='Output filename (default: cameras.json)')
    
    parser.add_argument('--verbose', '-v',
                       action='store_true',
                       help='Show verbose output')
    
    args = parser.parse_args()
    
    # Validate state
    if args.state and args.state.upper() not in US_STATES:
        print(f"Error: Unknown state code '{args.state}'")
        print(f"Valid codes: {', '.join(sorted(US_STATES.keys()))}")
        sys.exit(1)
    
    # Determine camera types
    if args.type == 'all':
        camera_types = ['alpr', 'redlight', 'speed']
    else:
        camera_types = [args.type]
    
    # Download
    cameras = download_cameras(camera_types, args.state, args.verbose)
    
    if not cameras:
        print("No cameras found!")
        sys.exit(1)
    
    # Save
    save_ndjson(cameras, args.output, camera_types, args.state)
    
    print("\n✓ Done! Copy this file to your SD card as /cameras.json")
    print("  or upload via the V1 Simple web UI.")


if __name__ == '__main__':
    main()

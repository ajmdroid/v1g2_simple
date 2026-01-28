#!/usr/bin/env python3
"""
Download camera database from OpenStreetMap via Overpass API.

This script downloads ALPR, red light, and speed camera data from OpenStreetMap
and converts it to NDJSON format compatible with the V1 Simple camera_manager.

Features:
- Downloads camera locations from OpenStreetMap
- Road snapping: snaps cameras to nearest road centerline
- Corridor enrichment: adds road bearing for accurate filtering

Usage:
    python3 download_cameras.py                    # Download all camera types for US
    python3 download_cameras.py --state CA         # Download for specific state
    python3 download_cameras.py --type alpr        # Download only ALPR cameras
    python3 download_cameras.py --no-snap          # Skip road snapping (faster)
    python3 download_cameras.py --output cameras.json  # Custom output filename

Output file can be:
    1. Copied to SD card as /cameras.json
    2. Uploaded via web UI at /integrations
"""

import argparse
import json
import math
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    print("Error: 'requests' module not found. Install with: pip3 install requests")
    sys.exit(1)

# Multiple Overpass API endpoints (try in order if one fails)
OVERPASS_ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",      # Main public instance (Germany)
    "https://overpass.kumi.systems/api/interpreter", # Kumi Systems (Switzerland)
]

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

# Default corridor parameters
DEFAULT_CORRIDOR_WIDTH_M = 35  # Typical 2-lane road width + margin
DEFAULT_BEARING_TOLERANCE = 30  # Degrees

# Road snap search radius in meters
ROAD_SNAP_RADIUS_M = 50


def haversine_distance(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Calculate distance in meters between two lat/lon points."""
    R = 6371000  # Earth radius in meters
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    delta_phi = math.radians(lat2 - lat1)
    delta_lambda = math.radians(lon2 - lon1)
    
    a = math.sin(delta_phi/2)**2 + \
        math.cos(phi1) * math.cos(phi2) * math.sin(delta_lambda/2)**2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1-a))
    
    return R * c


def calculate_bearing(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Calculate bearing in degrees from point 1 to point 2."""
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    delta_lambda = math.radians(lon2 - lon1)
    
    x = math.sin(delta_lambda) * math.cos(phi2)
    y = math.cos(phi1) * math.sin(phi2) - \
        math.sin(phi1) * math.cos(phi2) * math.cos(delta_lambda)
    
    bearing = math.degrees(math.atan2(x, y))
    return (bearing + 360) % 360


def point_to_segment_distance(px: float, py: float, 
                               x1: float, y1: float, 
                               x2: float, y2: float) -> tuple:
    """
    Find the closest point on a line segment to a given point.
    Returns (distance, closest_lat, closest_lon, segment_bearing).
    Uses simple planar math (good enough for short distances).
    """
    dx = x2 - x1
    dy = y2 - y1
    
    if dx == 0 and dy == 0:
        # Segment is a point
        dist = haversine_distance(py, px, y1, x1)
        bearing = 0
        return (dist, y1, x1, bearing)
    
    # Calculate projection parameter t
    t = max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)))
    
    # Closest point on segment
    closest_x = x1 + t * dx
    closest_y = y1 + t * dy
    
    dist = haversine_distance(py, px, closest_y, closest_x)
    bearing = calculate_bearing(y1, x1, y2, x2)
    
    return (dist, closest_y, closest_x, bearing)


def snap_to_nearest_road(lat: float, lon: float, verbose: bool = False) -> dict:
    """
    Query OSM for nearby roads and snap camera to nearest road centerline.
    Returns dict with snap coordinates and road bearing, or None if no road found.
    """
    # Build Overpass query for nearby roads
    # Search for highways within ROAD_SNAP_RADIUS_M
    query = f"""[out:json][timeout:30];
way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified)$"]
  (around:{ROAD_SNAP_RADIUS_M},{lat},{lon});
out geom;
"""
    
    # Try each endpoint for road snapping
    data = None
    for endpoint in OVERPASS_ENDPOINTS:
        try:
            response = requests.post(
                endpoint,
                data={'data': query},
                timeout=30,
                headers={'User-Agent': 'V1Simple-CameraDownloader/1.0'}
            )
            response.raise_for_status()
            data = response.json()
            break
        except requests.exceptions.RequestException:
            continue
    
    if data is None:
        if verbose:
            print(f"  Road query failed on all endpoints")
        return None
    
    ways = data.get('elements', [])
    
    if not ways:
        return None
    
    # Find closest road segment
    best_dist = float('inf')
    best_snap = None
    
    for way in ways:
        geometry = way.get('geometry', [])
        if len(geometry) < 2:
            continue
        
        # Check each segment in the way
        for i in range(len(geometry) - 1):
            p1 = geometry[i]
            p2 = geometry[i + 1]
            
            dist, snap_lat, snap_lon, bearing = point_to_segment_distance(
                lon, lat,  # Point (camera)
                p1['lon'], p1['lat'],  # Segment start
                p2['lon'], p2['lat']   # Segment end
            )
            
            if dist < best_dist:
                best_dist = dist
                best_snap = {
                    'slat': round(snap_lat, 6),
                    'slon': round(snap_lon, 6),
                    'rbr': int(round(bearing)),
                    'cwm': DEFAULT_CORRIDOR_WIDTH_M,
                    'btol': DEFAULT_BEARING_TOLERANCE
                }
    
    return best_snap


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


def query_overpass_with_retry(query: str, max_retries: int = 3, verbose: bool = False) -> dict:
    """
    Query Overpass API with retry logic and multiple endpoints.
    Tries each endpoint with exponential backoff on failure.
    """
    last_error = None
    
    for attempt in range(max_retries):
        for endpoint in OVERPASS_ENDPOINTS:
            try:
                if verbose or attempt > 0:
                    endpoint_name = endpoint.split('/')[2]
                    print(f"  Trying {endpoint_name}..." + (f" (attempt {attempt+1})" if attempt > 0 else ""))
                
                response = requests.post(
                    endpoint,
                    data={'data': query},
                    timeout=180,  # 3 minute timeout per request
                    headers={'User-Agent': 'V1Simple-CameraDownloader/1.0'}
                )
                response.raise_for_status()
                return response.json()
                
            except requests.exceptions.Timeout:
                last_error = "timeout"
                if verbose:
                    print(f"    Timeout on {endpoint.split('/')[2]}")
                continue
                
            except requests.exceptions.HTTPError as e:
                status = e.response.status_code if e.response else 'unknown'
                last_error = f"HTTP {status}"
                if verbose:
                    print(f"    HTTP {status} on {endpoint.split('/')[2]}")
                # 429 = rate limited, 504 = gateway timeout - try another endpoint
                if status in [429, 502, 503, 504]:
                    continue
                # Other errors might be query-related
                raise
                
            except requests.exceptions.RequestException as e:
                last_error = str(e)
                if verbose:
                    print(f"    Error: {e}")
                continue
        
        # All endpoints failed this attempt, wait before retry
        if attempt < max_retries - 1:
            wait_time = (attempt + 1) * 10  # 10s, 20s, 30s...
            print(f"  All endpoints failed ({last_error}), waiting {wait_time}s before retry...")
            time.sleep(wait_time)
    
    raise requests.exceptions.RequestException(f"All endpoints failed after {max_retries} attempts: {last_error}")


def download_cameras(camera_types: list, state: str = None, verbose: bool = False) -> list:
    """Download camera data from Overpass API."""
    
    query = build_overpass_query(camera_types, state)
    
    if verbose:
        print(f"Query:\n{query}\n")
    
    print(f"Downloading {', '.join(camera_types)} cameras" + 
          (f" for {state.upper()}" if state else " for entire US") + "...")
    print("This may take a few minutes...")
    
    try:
        data = query_overpass_with_retry(query, max_retries=3, verbose=verbose)
    except requests.exceptions.RequestException as e:
        print(f"Error: {e}")
        return None  # Return None instead of exiting - allows state-by-state to continue
    
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


def enrich_with_road_data(cameras: list, verbose: bool = False) -> list:
    """
    Enrich camera records with road corridor data via OSM road snapping.
    This adds road bearing and snap coordinates for accurate filtering.
    """
    total = len(cameras)
    enriched = 0
    failed = 0
    
    print(f"\nEnriching {total} cameras with road data...")
    print("(This queries OSM for each camera - may take a while)")
    
    for i, cam in enumerate(cameras):
        # Progress indicator every 10 cameras
        if (i + 1) % 10 == 0 or i == 0:
            print(f"  Processing {i + 1}/{total}... ({enriched} enriched, {failed} failed)", 
                  end='\r')
        
        snap_data = snap_to_nearest_road(cam['lat'], cam['lon'], verbose)
        
        if snap_data:
            cam.update(snap_data)
            enriched += 1
        else:
            failed += 1
        
        # Rate limiting - be nice to Overpass API
        # Roughly 1 request per 100ms
        time.sleep(0.1)
    
    print(f"\n  Enriched: {enriched}/{total} cameras ({failed} had no nearby road)")
    
    return cameras


def save_ndjson(cameras: list, output_path: str, camera_types: list, state: str = None):
    """Save cameras to NDJSON file."""
    
    # Count corridor-enriched cameras
    enriched_count = sum(1 for cam in cameras if 'rbr' in cam)
    
    # Create metadata
    type_names = []
    if 'alpr' in camera_types:
        type_names.append('ALPR')
    if 'redlight' in camera_types:
        type_names.append('Red Light')
    if 'speed' in camera_types:
        type_names.append('Speed')
    
    if state:
        area_name = US_STATES.get(state.upper(), state.upper())
    else:
        area_name = "US (All States)"
    
    meta = {
        '_meta': {
            'name': f"OSM {'/'.join(type_names)} ({area_name})",
            'date': datetime.now().strftime('%Y-%m-%d'),
            'count': len(cameras),
            'enriched': enriched_count,
            'source': 'OpenStreetMap via Overpass API'
        }
    }
    
    with open(output_path, 'w') as f:
        f.write(json.dumps(meta) + '\n')
        for cam in cameras:
            f.write(json.dumps(cam) + '\n')
    
    print(f"\nSaved to: {output_path}")
    print(f"Total cameras: {len(cameras)}")
    if enriched_count > 0:
        print(f"Road-enriched: {enriched_count} ({enriched_count * 100 // len(cameras)}%)")
    
    # Count by type
    type_counts = {}
    type_names = {1: 'Red Light', 2: 'Speed', 3: 'Red Light + Speed', 4: 'ALPR'}
    for cam in cameras:
        t = cam.get('flg', 2)
        type_counts[t] = type_counts.get(t, 0) + 1
    
    for t, count in sorted(type_counts.items()):
        print(f"  - {type_names.get(t, 'Unknown')}: {count}")


def download_all_states(camera_types: list, no_snap: bool, verbose: bool, 
                        output_dir: str = 'camera_data/states') -> list:
    """
    Download cameras state-by-state and bundle together.
    More reliable than a single US-wide query.
    """
    import os
    
    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    
    all_cameras = []
    successful_states = []
    failed_states = []
    
    state_codes = sorted(US_STATES.keys())
    total_states = len(state_codes)
    
    print(f"\n{'='*60}")
    print(f"Downloading cameras for all {total_states} US states")
    print(f"Camera types: {', '.join(camera_types)}")
    print(f"Road snapping: {'disabled' if no_snap else 'enabled'}")
    print(f"{'='*60}\n")
    
    for i, state in enumerate(state_codes):
        state_name = US_STATES[state]
        print(f"\n[{i+1}/{total_states}] {state} - {state_name}")
        print("-" * 40)
        
        state_file = os.path.join(output_dir, f"{state.lower()}.json")
        
        # Check if state file already exists (for resume capability)
        if os.path.exists(state_file):
            print(f"  Found existing {state_file}, loading...")
            try:
                with open(state_file, 'r') as f:
                    state_cameras = []
                    for line in f:
                        data = json.loads(line)
                        if '_meta' not in data:
                            state_cameras.append(data)
                    all_cameras.extend(state_cameras)
                    successful_states.append(state)
                    print(f"  Loaded {len(state_cameras)} cameras from cache")
                    continue
            except Exception as e:
                print(f"  Error loading cache: {e}, re-downloading...")
        
        try:
            # Download this state
            cameras = download_cameras(camera_types, state, verbose)
            
            if cameras is None:
                # API error - add to failed list
                print(f"  ✗ {state}: Download failed")
                failed_states.append(state)
            elif cameras:
                # Enrich with road data (unless --no-snap)
                if not no_snap:
                    cameras = enrich_with_road_data(cameras, verbose)
                
                # Save state file for caching/resume
                save_ndjson(cameras, state_file, camera_types, state)
                
                all_cameras.extend(cameras)
                successful_states.append(state)
                print(f"  ✓ {state}: {len(cameras)} cameras")
            else:
                print(f"  - {state}: No cameras found")
                successful_states.append(state)  # No error, just empty
                
        except Exception as e:
            print(f"  ✗ {state}: Error - {e}")
            failed_states.append(state)
        
        # Longer delay between states to avoid rate limiting
        if i < total_states - 1:
            wait_time = 5 if not failed_states else 15  # Longer wait after failures
            print(f"  Waiting {wait_time}s before next state...")
            time.sleep(wait_time)
    
    print(f"\n{'='*60}")
    print(f"Download Complete!")
    print(f"  Successful: {len(successful_states)} states")
    if failed_states:
        print(f"  Failed: {len(failed_states)} states: {', '.join(failed_states)}")
        print(f"  (Run again to retry failed states - cached states will be skipped)")
    print(f"  Total cameras: {len(all_cameras)}")
    print(f"{'='*60}")
    
    return all_cameras


def main():
    parser = argparse.ArgumentParser(
        description='Download camera database from OpenStreetMap',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 download_cameras.py                         # All US cameras (single query)
  python3 download_cameras.py --all-states            # Download state-by-state (recommended!)
  python3 download_cameras.py --state CA              # California only
  python3 download_cameras.py --type alpr --state TX  # Texas ALPR only
  python3 download_cameras.py --no-snap               # Skip road snapping (faster)
  python3 download_cameras.py --output /path/to/sd/cameras.json

After downloading, copy the file to your SD card as /cameras.json
or upload via the web UI at http://192.168.4.1/integrations

Road snapping enriches each camera with:
  - Road bearing (for heading-based filtering)
  - Snap coordinates (road centerline position)
  - Corridor width (for cross-track distance filtering)
This helps filter false alerts from parallel roads.

RECOMMENDED: Use --all-states for reliable downloading of the full US database.
This downloads each state individually (with caching for resume capability).
"""
    )
    
    parser.add_argument('--type', '-t', 
                       choices=['all', 'alpr', 'redlight', 'speed'],
                       default='all',
                       help='Camera type to download (default: all)')
    
    parser.add_argument('--state', '-s',
                       type=str,
                       help='US state code (e.g., CA, TX, NY)')
    
    parser.add_argument('--all-states',
                       action='store_true',
                       help='Download all states individually (recommended for full US)')
    
    parser.add_argument('--output', '-o',
                       type=str,
                       default='cameras.json',
                       help='Output filename (default: cameras.json)')
    
    parser.add_argument('--no-snap', 
                       action='store_true',
                       help='Skip road snapping (faster, but less accurate filtering)')
    
    parser.add_argument('--verbose', '-v',
                       action='store_true',
                       help='Show verbose output')
    
    args = parser.parse_args()
    
    # Validate state
    if args.state and args.state.upper() not in US_STATES:
        print(f"Error: Unknown state code '{args.state}'")
        print(f"Valid codes: {', '.join(sorted(US_STATES.keys()))}")
        sys.exit(1)
    
    # Can't use both --state and --all-states
    if args.state and args.all_states:
        print("Error: Cannot use both --state and --all-states")
        sys.exit(1)
    
    # Determine camera types
    if args.type == 'all':
        camera_types = ['alpr', 'redlight', 'speed']
    else:
        camera_types = [args.type]
    
    # Download - either all states or single query
    if args.all_states:
        cameras = download_all_states(camera_types, args.no_snap, args.verbose)
        state = None
    else:
        cameras = download_cameras(camera_types, args.state, args.verbose)
        state = args.state
        
        if cameras and not args.no_snap:
            cameras = enrich_with_road_data(cameras, args.verbose)
        elif args.no_snap:
            print("\nSkipping road snapping (--no-snap specified)")
    
    if not cameras:
        print("No cameras found!")
        sys.exit(1)
    
    # Save final bundled output
    save_ndjson(cameras, args.output, camera_types, state)
    
    print("\n✓ Done! Copy this file to your SD card as /cameras.json")
    print("  or upload via the V1 Simple web UI.")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Camera enrichment pipeline using PostGIS.

This script reads camera NDJSON files, loads them into PostGIS,
runs spatial enrichment queries to snap cameras to roads and create
corridor segments, then exports enriched NDJSON files.

Usage:
    python enrich_cameras.py input.json                    # Enrich single file
    python enrich_cameras.py --input-dir ../camera_data/   # Enrich all files in dir
    python enrich_cameras.py input.json --dry-run          # Test without writing
"""

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path

try:
    import psycopg2
    from psycopg2.extras import execute_values, RealDictCursor
except ImportError:
    print("Error: psycopg2 not found. Install with: pip install psycopg2-binary")
    sys.exit(1)


# Default configuration
DEFAULT_CONFIG = {
    'db_host': os.environ.get('POSTGRES_HOST', 'localhost'),
    'db_port': int(os.environ.get('POSTGRES_PORT', 5432)),
    'db_name': os.environ.get('POSTGRES_DB', 'osm'),
    'db_user': os.environ.get('POSTGRES_USER', 'osm'),
    'db_pass': os.environ.get('POSTGRES_PASSWORD', 'osm'),
    'search_radius_m': float(os.environ.get('SEARCH_RADIUS_M', 120)),
    'corridor_half_length_m': float(os.environ.get('CORRIDOR_HALF_LENGTH_M', 60)),
    'corridor_half_width_m': float(os.environ.get('CORRIDOR_HALF_WIDTH_M', 35)),
    'batch_size': int(os.environ.get('BATCH_SIZE', 5000)),
}


def get_db_connection(config):
    """Create PostgreSQL connection."""
    return psycopg2.connect(
        host=config['db_host'],
        port=config['db_port'],
        dbname=config['db_name'],
        user=config['db_user'],
        password=config['db_pass'],
    )


def check_roads_table(conn):
    """Verify roads table exists and has data."""
    with conn.cursor() as cur:
        cur.execute("""
            SELECT COUNT(*) FROM information_schema.tables 
            WHERE table_name = 'roads'
        """)
        if cur.fetchone()[0] == 0:
            return 0, "Table 'roads' does not exist. Run OSM import first."
        
        cur.execute("SELECT COUNT(*) FROM roads")
        count = cur.fetchone()[0]
        if count == 0:
            return 0, "Table 'roads' is empty. Run OSM import first."
        
        return count, None


def load_cameras_from_ndjson(filepath):
    """Load cameras from NDJSON file, preserving original data."""
    cameras = []
    metadata = None
    
    with open(filepath, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            
            try:
                data = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"  Warning: Skipping invalid JSON on line {line_num}: {e}")
                continue
            
            # Check for metadata line
            if '_meta' in data:
                metadata = data
                continue
            
            # Validate required fields
            if 'lat' not in data or 'lon' not in data:
                print(f"  Warning: Skipping line {line_num} - missing lat/lon")
                continue
            
            cameras.append({
                'line_num': line_num,
                'original': data,
            })
    
    return cameras, metadata


def clear_staging_table(conn):
    """Clear the staging table for a fresh run."""
    with conn.cursor() as cur:
        cur.execute("TRUNCATE TABLE cameras_staging RESTART IDENTITY")
    conn.commit()


def load_cameras_to_staging(conn, cameras, batch_size=5000):
    """Batch insert cameras into staging table."""
    print(f"  Loading {len(cameras)} cameras into staging table...")
    
    with conn.cursor() as cur:
        # Prepare data for batch insert
        values = []
        for cam in cameras:
            values.append((
                f"cam_{cam['line_num']}",  # camera_id
                cam['original']['lat'],
                cam['original']['lon'],
                json.dumps(cam['original']),
            ))
        
        # Batch insert
        for i in range(0, len(values), batch_size):
            batch = values[i:i + batch_size]
            execute_values(
                cur,
                """
                INSERT INTO cameras_staging (camera_id, lat, lon, original_json)
                VALUES %s
                ON CONFLICT (camera_id) DO UPDATE SET
                    lat = EXCLUDED.lat,
                    lon = EXCLUDED.lon,
                    original_json = EXCLUDED.original_json,
                    processed_at = NULL
                """,
                batch,
            )
            print(f"    Inserted {min(i + batch_size, len(values))}/{len(values)}", end='\r')
        
        # Update geometry column
        cur.execute("""
            UPDATE cameras_staging 
            SET geom = ST_Transform(ST_SetSRID(ST_MakePoint(lon, lat), 4326), 3857)
            WHERE geom IS NULL
        """)
    
    conn.commit()
    print(f"  Loaded {len(cameras)} cameras into staging table    ")


def run_enrichment(conn, config):
    """Run the batch enrichment SQL."""
    print(f"  Running enrichment (radius={config['search_radius_m']}m, corridor={config['corridor_half_length_m']}m)...")
    
    with conn.cursor() as cur:
        # Run batch enrichment
        cur.execute(
            "SELECT * FROM enrich_cameras_batch(%s, %s)",
            (config['search_radius_m'], config['corridor_half_length_m'])
        )
        result = cur.fetchone()
    
    conn.commit()
    
    return {
        'processed': result[0] or 0,
        'matched': result[1] or 0,
        'no_match': result[2] or 0,
    }


def export_enriched_cameras(conn, config):
    """Export enriched cameras from staging table."""
    cameras = []
    
    with conn.cursor(cursor_factory=RealDictCursor) as cur:
        cur.execute("""
            SELECT 
                camera_id,
                original_json,
                ST_Y(corridor_p1) AS p1_lat,
                ST_X(corridor_p1) AS p1_lon,
                ST_Y(corridor_p2) AS p2_lat,
                ST_X(corridor_p2) AS p2_lon,
                bearing,
                road_name,
                road_class,
                maxspeed,
                oneway,
                snap_distance_m
            FROM cameras_staging
            ORDER BY id
        """)
        
        for row in cur:
            # Start with original data
            cam = row['original_json']
            
            # Add enrichment if matched
            if row['p1_lat'] is not None:
                cam['p1'] = [round(row['p1_lat'], 6), round(row['p1_lon'], 6)]
                cam['p2'] = [round(row['p2_lat'], 6), round(row['p2_lon'], 6)]
                cam['brg'] = int(round(row['bearing'])) if row['bearing'] else None
                cam['w'] = config['corridor_half_width_m']
                
                # Optional: speed info
                if row['maxspeed']:
                    cam['maxspeed_raw'] = row['maxspeed']
                    # Try to parse mph
                    try:
                        speed_str = row['maxspeed'].replace(' mph', '').replace('mph', '')
                        cam['psl'] = int(speed_str)
                    except (ValueError, AttributeError):
                        pass
                
                # Optional: oneway
                if row['oneway']:
                    cam['oneway'] = row['oneway']
            
            cameras.append(cam)
    
    return cameras


def write_enriched_ndjson(cameras, metadata, output_path, stats):
    """Write enriched cameras to NDJSON file."""
    
    # Update metadata
    if metadata is None:
        metadata = {'_meta': {}}
    
    metadata['_meta']['enriched_date'] = datetime.now().strftime('%Y-%m-%d')
    metadata['_meta']['enriched_count'] = stats['matched']
    metadata['_meta']['no_match_count'] = stats['no_match']
    metadata['_meta']['total_count'] = len(cameras)
    metadata['_meta']['enrichment_source'] = 'PostGIS/OSM'
    
    # Write output
    with open(output_path, 'w') as f:
        f.write(json.dumps(metadata) + '\n')
        for cam in cameras:
            f.write(json.dumps(cam) + '\n')
    
    return output_path


def process_file(input_path, output_path, conn, config, dry_run=False):
    """Process a single camera file."""
    print(f"\nProcessing: {input_path}")
    
    # Load cameras
    cameras, metadata = load_cameras_from_ndjson(input_path)
    if not cameras:
        print("  No valid cameras found, skipping.")
        return None
    
    print(f"  Found {len(cameras)} cameras")
    
    if dry_run:
        print("  [DRY RUN] Would process and write to:", output_path)
        return {'processed': len(cameras), 'matched': 0, 'no_match': 0}
    
    # Clear staging and load cameras
    clear_staging_table(conn)
    load_cameras_to_staging(conn, cameras, config['batch_size'])
    
    # Run enrichment
    stats = run_enrichment(conn, config)
    print(f"  Results: {stats['matched']} matched, {stats['no_match']} no match")
    
    # Export results
    enriched = export_enriched_cameras(conn, config)
    
    # Write output
    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)
    write_enriched_ndjson(enriched, metadata, output_path, stats)
    print(f"  Wrote: {output_path}")
    
    return stats


def main():
    parser = argparse.ArgumentParser(
        description='Enrich camera data with OSM road corridor information',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Environment variables:
  POSTGRES_HOST          Database host (default: localhost)
  POSTGRES_PORT          Database port (default: 5432)
  POSTGRES_DB            Database name (default: osm)
  POSTGRES_USER          Database user (default: osm)
  POSTGRES_PASSWORD      Database password (default: osm)
  SEARCH_RADIUS_M        Max distance to snap to road (default: 120)
  CORRIDOR_HALF_LENGTH_M Corridor segment length each side (default: 60)
  CORRIDOR_HALF_WIDTH_M  Corridor width for matching (default: 35)

Examples:
  # Process single file
  python enrich_cameras.py ../camera_data/cameras.json
  
  # Process all files in directory
  python enrich_cameras.py --input-dir ../camera_data/
  
  # Custom output directory
  python enrich_cameras.py input.json --output-dir ./enriched/
  
  # Adjust search radius
  SEARCH_RADIUS_M=200 python enrich_cameras.py input.json
"""
    )
    
    parser.add_argument('input_file', nargs='?',
                       help='Input NDJSON camera file')
    parser.add_argument('--input-dir', '-i',
                       help='Process all .json files in directory')
    parser.add_argument('--output-dir', '-o',
                       default='../../camera_data_enriched',
                       help='Output directory (default: ../../camera_data_enriched)')
    parser.add_argument('--dry-run', '-n', action='store_true',
                       help='Test run without writing output')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
    
    args = parser.parse_args()
    
    # Validate input
    if not args.input_file and not args.input_dir:
        parser.error("Must specify input_file or --input-dir")
    
    config = DEFAULT_CONFIG.copy()
    
    # Connect to database
    print("Connecting to PostGIS...")
    try:
        conn = get_db_connection(config)
    except Exception as e:
        print(f"Error: Cannot connect to database: {e}")
        print("Make sure PostGIS container is running: docker compose up -d postgis")
        sys.exit(1)
    
    # Check roads table
    road_count, error = check_roads_table(conn)
    if error:
        print(f"Error: {error}")
        print("Run the OSM import first: ./import_osm.sh")
        sys.exit(1)
    print(f"Found {road_count:,} roads in database")
    
    # Determine files to process
    if args.input_dir:
        input_dir = Path(args.input_dir)
        files = list(input_dir.glob('*.json'))
        print(f"Found {len(files)} files in {input_dir}")
    else:
        files = [Path(args.input_file)]
    
    # Process each file
    total_stats = {'processed': 0, 'matched': 0, 'no_match': 0}
    output_dir = Path(args.output_dir)
    
    for input_path in files:
        output_path = output_dir / input_path.name
        
        stats = process_file(
            str(input_path), 
            str(output_path), 
            conn, 
            config, 
            args.dry_run
        )
        
        if stats:
            total_stats['processed'] += stats['processed']
            total_stats['matched'] += stats['matched']
            total_stats['no_match'] += stats['no_match']
    
    # Summary
    print(f"\n{'='*50}")
    print("Enrichment Complete")
    print(f"  Files processed: {len(files)}")
    print(f"  Total cameras: {total_stats['processed']}")
    print(f"  Matched to roads: {total_stats['matched']}")
    print(f"  No match (kept original): {total_stats['no_match']}")
    if total_stats['processed'] > 0:
        match_pct = total_stats['matched'] * 100 / total_stats['processed']
        print(f"  Match rate: {match_pct:.1f}%")
    print(f"{'='*50}")
    
    conn.close()


if __name__ == '__main__':
    main()

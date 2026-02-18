# Offline camera enrichment (roads-only, disk-lean)

This pipeline keeps disk use low by filtering to highways only, importing with osm2pgsql **flex** schema (roads table only), and merging road corridor metadata into the camera NDJSON files.

## What it produces
- Enriched NDJSON in `camera_data_enriched/<name>.json` with added fields:
  - `p1`, `p2`: corridor endpoints `[lat, lon]`
  - `w`: half-width meters (default 35)
  - `brg`: bearing degrees 0..359 (p1 -> p2)
  - `maxspeed`, `maxspeed_fwd`, `maxspeed_back`, `oneway`, `road_class`, `road_name`, `road_ref`

## Prereqs
- Docker Desktop with at least **80–100GB** disk (full US) or ~20GB (state/region).
- Python 3 + `pip install -r requirements.txt`.

## 1) Start PostGIS
```bash
cd tools/enrich_osm
docker compose up -d postgis
```

## 2) Import roads (disk-lean)
- Default: full US from Geofabrik, roads-only via osmium filter.
- Optional env vars:
  - `PBF_URL` custom extract URL
  - `CACHE_MB` (default 1024)
  - `USE_FLAT_NODES=1` to store flat-nodes at `data/nodes.cache` (bigger disk, faster)

```bash
# Full US (roads-only)
US_FULL=1 ./import_osm.sh

# Smaller: state/region
PBF_URL=https://download.geofabrik.de/north-america/us/california-latest.osm.pbf ./import_osm.sh
```

Pipeline steps:
1. Download PBF to `data/input.osm.pbf`
2. `osmium tags-filter` keeps only `highway=*` to `data/roads.osm.pbf`
3. `osm2pgsql` flex imports into table `roads`, drops slim tables, builds GiST index.

## 3) Load cameras into PostGIS
```bash
./load_camera_ndjson_to_pg.py ../camera_data/alpr.json --table cameras_stage
```
(ALPR-only pipeline)

## 4) Enrich (nearest road, corridor)
```bash
psql -v stage_table=cameras_stage \
     -v out_table=cameras_enriched \
     -v radius_m=120 -v span_m=60 -v width_m=35 \
     -f enrich_cameras.sql
```
- `radius_m`: max search distance to a road
- `span_m`: half-length along the road to build corridor endpoints
- `width_m`: corridor half-width

## 5) Export merged NDJSON
```bash
mkdir -p ../camera_data_enriched
./export_and_merge.py --table cameras_enriched --output ../camera_data_enriched/alpr.json
```

## Sanity test (tiny run)
1. Use a small extract (e.g., city PBF) and set `PBF_URL` to it.
2. Limit camera load: `--limit 1000`
3. Run steps 1–5 above. Confirm output has `p1/p2/brg/w`.

## Disk footprint (full US, roads-only)
- Input PBF: ~10–11GB
- Osmium roads PBF: ~2–3GB
- PostGIS roads table + indexes: ~10–15GB
- Flat-nodes (optional): ~25–30GB
- Camera outputs remain small (tens of MB). Only enriched NDJSON goes on-device.

The import:
- Downloads `us-latest.osm.pbf` from Geofabrik (~10GB)
- Starts PostGIS container
- Imports roads using osm2pgsql with flex output
- Creates spatial indexes
- Takes 30-60 minutes on a modern machine

**Note:** You only need to run this once. The data persists in a Docker volume.

### Step 2: Enrich Camera Files

```bash
# Single file
python enrich_cameras.py ../../camera_data/cameras.json

# All files in a directory  
python enrich_cameras.py --input-dir ../../camera_data/

# Custom output directory
python enrich_cameras.py input.json --output-dir ./my_output/

# Dry run (test without writing)
python enrich_cameras.py input.json --dry-run
```

### Step 3: Copy to Device

The enriched files are written to `../../camera_data_enriched/` by default. Copy them to the device's SD card.

### Step 3b: Convert Enriched NDJSON To Runtime `.bin`

Firmware runtime consumes VCAM binary datasets (`*.bin`), not NDJSON.

```bash
# ALPR-only runtime schema
python convert_to_binary.py ../../camera_data_enriched/alpr.json ../../camera_data_enriched/alpr.bin
```

Use runtime-compatible filename on SD root:
- `alpr.bin`

## Enriched Output Format

Original camera record:
```json
{"lat": 33.7490, "lon": -84.3880, "flg": 4, "spd": 35}
```

Enriched camera record:
```json
{
  "lat": 33.7490,
  "lon": -84.3880,
  "flg": 4,
  "spd": 35,
  "p1": [33.7485, -84.3875],
  "p2": [33.7495, -84.3885],
  "brg": 45,
  "w": 35,
  "psl": 35,
  "maxspeed_raw": "35 mph",
  "oneway": "yes"
}
```

### New Fields

| Field | Type | Description |
|-------|------|-------------|
| `p1` | `[lat, lon]` | Corridor endpoint 1 |
| `p2` | `[lat, lon]` | Corridor endpoint 2 |
| `brg` | `int` | Bearing from p1→p2 (0-359°) |
| `w` | `int` | Corridor half-width in meters |
| `psl` | `int` | Posted speed limit (mph, if parseable) |
| `maxspeed_raw` | `string` | Raw OSM maxspeed tag |
| `oneway` | `string` | Oneway tag if present |

If no road is found within the search radius, the camera keeps its original fields and no corridor fields are added.

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `POSTGRES_HOST` | localhost | Database host |
| `POSTGRES_PORT` | 5432 | Database port |
| `POSTGRES_DB` | osm | Database name |
| `POSTGRES_USER` | osm | Database user |
| `POSTGRES_PASSWORD` | osm | Database password |
| `SEARCH_RADIUS_M` | 120 | Max distance to snap camera to road |
| `CORRIDOR_HALF_LENGTH_M` | 60 | Corridor segment length each side of snap point |
| `CORRIDOR_HALF_WIDTH_M` | 35 | Corridor width for device matching |

### Example: Adjust Search Radius

```bash
SEARCH_RADIUS_M=200 python enrich_cameras.py cameras.json
```

## Managing the Database

```bash
# Start PostGIS (runs in background)
docker compose up -d postgis

# Stop PostGIS
docker compose down

# View logs
docker compose logs -f postgis

# Connect to database
docker compose exec postgis psql -U osm -d osm

# Check road count
docker compose exec postgis psql -U osm -d osm -c "SELECT COUNT(*) FROM roads"

# Delete all data (re-import required)
docker compose down -v
```

## Performance

Benchmarks on M3 MacBook Pro:

| Operation | Time |
|-----------|------|
| Download US PBF | ~15 min (depends on connection) |
| Import to PostGIS | ~45 min |
| Enrich 71K cameras | ~2 min |

The enrichment uses:
- Batch inserts (5000 per batch)
- GiST spatial indexes with KNN operator
- Bounding box prefilter before distance calculation

## Troubleshooting

### "Cannot connect to database"

Make sure PostGIS is running:
```bash
docker compose up -d postgis
docker compose logs postgis
```

### "Table 'roads' does not exist"

Run the OSM import:
```bash
./import_osm.sh
```

### Import is very slow

- Ensure you have at least 4GB RAM available
- SSD storage significantly improves performance
- The first import creates indexes which takes time

### Low match rate

If many cameras aren't matching roads:
1. Increase search radius: `SEARCH_RADIUS_M=200`
2. Check if cameras are in rural areas with fewer OSM roads
3. Verify the PBF file covers the camera locations

## Files

```
tools/enrich_osm/
├── README.md              # This file
├── docker-compose.yml     # PostGIS + osm2pgsql containers
├── schema.sql             # Database schema and functions
├── osm2pgsql.lua          # Flex output style for road import
├── import_osm.sh          # Download + import script
├── enrich_cameras.py      # Main enrichment script
└── data/                  # Created at runtime
    └── us-latest.osm.pbf  # Downloaded OSM extract
```

## How It Works

1. **OSM Import**: osm2pgsql reads the PBF file and extracts road geometries (ways with `highway=*`) into PostGIS using the flex output format defined in `osm2pgsql.lua`.

2. **Camera Loading**: Cameras are batch-inserted into a staging table with their lat/lon converted to PostGIS points in EPSG:3857 (Web Mercator).

3. **Enrichment SQL**: For each camera, a LATERAL subquery finds the nearest road using the `<->` KNN operator with a bounding box prefilter. `ST_ClosestPoint` snaps the camera to the road, and `ST_LineInterpolatePoint` creates corridor endpoints.

4. **Export**: Results are joined back to original camera data and written as NDJSON.

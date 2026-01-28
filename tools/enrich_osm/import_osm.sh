#!/bin/bash
# Import OSM PBF data into PostGIS
# 
# This script downloads the US OSM extract and imports road data.
# The import takes ~30-60 minutes for the full US dataset.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${SCRIPT_DIR}/data"
PBF_URL="${PBF_URL:-https://download.geofabrik.de/north-america/us-latest.osm.pbf}"
PBF_FILE="${DATA_DIR}/us-latest.osm.pbf"

echo "========================================"
echo "OSM Import Pipeline"
echo "========================================"

# Create data directory
mkdir -p "${DATA_DIR}"

# Check if PBF file exists
if [ ! -f "${PBF_FILE}" ]; then
    echo ""
    echo "Downloading US OSM extract..."
    echo "URL: ${PBF_URL}"
    echo "This is ~10GB and may take a while..."
    echo ""
    
    # Download with progress and resume support
    curl -L -C - -o "${PBF_FILE}.tmp" "${PBF_URL}"
    mv "${PBF_FILE}.tmp" "${PBF_FILE}"
    
    echo "Download complete!"
else
    echo "Found existing PBF file: ${PBF_FILE}"
    echo "Delete it to re-download."
fi

# Verify file size (should be several GB)
FILE_SIZE=$(stat -f%z "${PBF_FILE}" 2>/dev/null || stat -c%s "${PBF_FILE}")
if [ "${FILE_SIZE}" -lt 1000000000 ]; then
    echo "Warning: PBF file seems too small (${FILE_SIZE} bytes)"
    echo "Expected ~10GB for US extract"
fi

echo ""
echo "Starting PostGIS container..."
docker compose up -d postgis

echo "Waiting for PostGIS to be ready..."
sleep 5

# Wait for health check
for i in {1..30}; do
    if docker compose exec -T postgis pg_isready -U osm -d osm > /dev/null 2>&1; then
        echo "PostGIS is ready!"
        break
    fi
    echo "  Waiting... ($i/30)"
    sleep 2
done

echo ""
echo "Creating spatial indexes on roads table (if exists)..."
docker compose exec -T postgis psql -U osm -d osm -c "
    CREATE INDEX IF NOT EXISTS idx_roads_geom ON roads USING GIST(geom);
    CREATE INDEX IF NOT EXISTS idx_roads_highway ON roads(highway);
" 2>/dev/null || true

echo ""
echo "Running osm2pgsql import..."
echo "This will take 30-60 minutes for the full US dataset."
echo ""

# Run osm2pgsql with the import profile
docker compose --profile import up osm2pgsql

echo ""
echo "Creating indexes..."
docker compose exec -T postgis psql -U osm -d osm -c "
    -- Ensure indexes exist after import
    CREATE INDEX IF NOT EXISTS idx_roads_geom ON roads USING GIST(geom);
    CREATE INDEX IF NOT EXISTS idx_roads_highway ON roads(highway);
    
    -- Analyze for query planner
    ANALYZE roads;
"

# Get stats
ROAD_COUNT=$(docker compose exec -T postgis psql -U osm -d osm -t -c "SELECT COUNT(*) FROM roads" | tr -d ' ')

echo ""
echo "========================================"
echo "Import Complete!"
echo "========================================"
echo "Roads imported: ${ROAD_COUNT}"
echo ""
echo "Next steps:"
echo "  1. Copy camera files to process"
echo "  2. Run: python enrich_cameras.py ../camera_data/cameras.json"
echo ""

#!/bin/bash
# Offline OSM roads-only import for camera enrichment (disk-lean)
# - Prefilters to highways with osmium to shrink the PBF
# - Imports with osm2pgsql flex (roads-only table) and drops slim tables

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${SCRIPT_DIR}/data"
RAW_PBF="${DATA_DIR}/input.osm.pbf"
ROADS_PBF="${DATA_DIR}/roads.osm.pbf"
PBF_URL="${PBF_URL:-https://download.geofabrik.de/north-america/us-latest.osm.pbf}"

OSMIUM_IMAGE="${OSMIUM_IMAGE:-ubuntu:22.04}"
OSMIUM_PLATFORM="${OSMIUM_PLATFORM:-}"
OSM2PGSQL_IMAGE="${OSM2PGSQL_IMAGE:-ubuntu:22.04}"
OSM2PGSQL_PLATFORM="${OSM2PGSQL_PLATFORM:-}"

CACHE_MB=${CACHE_MB:-1024}
USE_FLAT_NODES=${USE_FLAT_NODES:-0}
FLAT_NODES_PATH="/data/nodes.cache"

echo "========================================"
echo "OSM roads-only import"
echo "Source: ${PBF_URL}"
echo "Cache: ${CACHE_MB} MB | Flat-nodes: ${USE_FLAT_NODES}"
echo "========================================"

mkdir -p "${DATA_DIR}"

echo "Starting PostGIS..."
docker compose up -d postgis

echo "Waiting for PostGIS health..."
for i in {1..30}; do
    if docker compose exec -T postgis pg_isready -U osm -d osm >/dev/null 2>&1; then
        echo "PostGIS is ready"
        break
    fi
    sleep 2
done

echo "Downloading PBF (roads source)..."
if [ ! -f "${RAW_PBF}" ]; then
    curl -L -C - -o "${RAW_PBF}.tmp" "${PBF_URL}"
    mv "${RAW_PBF}.tmp" "${RAW_PBF}"
else
    echo "Using existing ${RAW_PBF}"
fi

RAW_SIZE=$(stat -f%z "${RAW_PBF}" 2>/dev/null || stat -c%s "${RAW_PBF}")
if [ "${RAW_SIZE}" -lt 5000000 ]; then
    echo "ERROR: PBF looks too small (${RAW_SIZE} bytes)."
    exit 1
fi

echo "Prefiltering highways with osmium (roads-only)..."
if [ -f "${ROADS_PBF}" ]; then
    echo "Using existing ${ROADS_PBF}"
else
    docker run --rm -e DEBIAN_FRONTEND=noninteractive -v "${DATA_DIR}:/data" ${OSMIUM_PLATFORM:+--platform ${OSMIUM_PLATFORM}} "${OSMIUM_IMAGE}" \
        sh -c "apt-get update -qq && apt-get install -y -qq osmium-tool && osmium tags-filter /data/input.osm.pbf w/highway -o /data/roads.osm.pbf --overwrite"
fi

echo "Importing roads with osm2pgsql flex..."
OSM2PGSQL_OPTS=(
    osm2pgsql
    --create
    --slim
    --drop
    --database=postgresql://osm:osm@v1simple_osm_postgis:5432/osm
    --output=flex
    --style=/osm2pgsql.lua
    --cache=${CACHE_MB}
)

if [ "${USE_FLAT_NODES}" = "1" ]; then
    OSM2PGSQL_OPTS+=(--flat-nodes=${FLAT_NODES_PATH})
fi

OSM2PGSQL_OPTS+=(/data/roads.osm.pbf)

# Build command string for sh -c
OSM2PGSQL_CMD="${OSM2PGSQL_OPTS[*]}"

docker run --rm \
    --network enrich_osm_default \
    -e DEBIAN_FRONTEND=noninteractive \
    -v "${DATA_DIR}:/data" \
    -v "${SCRIPT_DIR}/osm2pgsql.lua:/osm2pgsql.lua" \
    ${OSM2PGSQL_PLATFORM:+--platform ${OSM2PGSQL_PLATFORM}} ${OSM2PGSQL_IMAGE} \
    sh -c "apt-get update -qq && apt-get install -y -qq osm2pgsql && ${OSM2PGSQL_CMD}"

echo "Creating spatial index on roads..."
docker compose exec -T postgis psql -U osm -d osm -c "CREATE INDEX IF NOT EXISTS idx_roads_geom ON roads USING GIST(geom);"

ROAD_COUNT=$(docker compose exec -T postgis psql -U osm -d osm -t -c "SELECT COUNT(*) FROM roads;")
echo "Import Complete!"
echo "========================================"
echo "Roads imported: ${ROAD_COUNT}"
echo ""
echo "Next steps:"
echo "  1. Load cameras: ./load_camera_ndjson_to_pg.py ../../camera_data/speed_cam.json"
echo "  2. Enrich: psql -v stage_table=cameras_stage -v out_table=cameras_enriched -v radius_m=120 -v span_m=60 -v width_m=35 -f enrich_cameras.sql"
echo "  3. Export: ./export_and_merge.py --table cameras_enriched --output ../../camera_data_enriched/speed_cam.json"
echo ""

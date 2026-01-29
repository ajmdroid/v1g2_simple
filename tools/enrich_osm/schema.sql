-- Schema for OSM road data enrichment
-- This file is auto-loaded by PostGIS container on first start

-- Enable required extensions
CREATE EXTENSION IF NOT EXISTS postgis;

-- Note: roads table is created by osm2pgsql flex import
-- Camera staging and enriched tables are created by Python/SQL scripts

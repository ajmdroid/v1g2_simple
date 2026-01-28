-- Schema for OSM road data enrichment
-- This file is auto-loaded by PostGIS container on first start

-- Enable required extensions
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS pg_trgm;

-- Roads table (populated by osm2pgsql flex output)
-- Note: osm2pgsql will create this, but we define indexes after import
-- Table structure is defined in osm2pgsql.lua

-- Camera staging table for batch processing
CREATE TABLE IF NOT EXISTS cameras_staging (
    id SERIAL PRIMARY KEY,
    camera_id TEXT UNIQUE,  -- Original identifier or line number
    lat DOUBLE PRECISION NOT NULL,
    lon DOUBLE PRECISION NOT NULL,
    geom GEOMETRY(Point, 3857),  -- Will be populated from lat/lon
    original_json JSONB NOT NULL,
    
    -- Enrichment results (NULL until processed)
    snap_geom GEOMETRY(Point, 3857),
    road_osm_id BIGINT,
    road_name TEXT,
    road_class TEXT,
    corridor_p1 GEOMETRY(Point, 4326),
    corridor_p2 GEOMETRY(Point, 4326),
    bearing DOUBLE PRECISION,
    maxspeed TEXT,
    oneway TEXT,
    snap_distance_m DOUBLE PRECISION,
    processed_at TIMESTAMP
);

-- Index for camera staging
CREATE INDEX IF NOT EXISTS idx_cameras_staging_geom ON cameras_staging USING GIST(geom);

-- Function to create corridor endpoints from a snap point on a road
CREATE OR REPLACE FUNCTION create_corridor(
    road_geom GEOMETRY,
    snap_point GEOMETRY,
    corridor_half_length_m DOUBLE PRECISION DEFAULT 60.0
)
RETURNS TABLE(
    p1 GEOMETRY,
    p2 GEOMETRY,
    bearing DOUBLE PRECISION
) AS $$
DECLARE
    line_fraction DOUBLE PRECISION;
    total_length DOUBLE PRECISION;
    frac_offset DOUBLE PRECISION;
    frac1 DOUBLE PRECISION;
    frac2 DOUBLE PRECISION;
BEGIN
    -- Get the fraction along the line where the snap point is
    line_fraction := ST_LineLocatePoint(road_geom, snap_point);
    total_length := ST_Length(road_geom);
    
    -- Calculate fraction offset for corridor half-length
    IF total_length > 0 THEN
        frac_offset := corridor_half_length_m / total_length;
    ELSE
        frac_offset := 0;
    END IF;
    
    -- Clamp fractions to [0, 1]
    frac1 := GREATEST(0, line_fraction - frac_offset);
    frac2 := LEAST(1, line_fraction + frac_offset);
    
    -- Get corridor endpoints
    p1 := ST_Transform(ST_LineInterpolatePoint(road_geom, frac1), 4326);
    p2 := ST_Transform(ST_LineInterpolatePoint(road_geom, frac2), 4326);
    
    -- Calculate bearing from p1 to p2
    bearing := degrees(ST_Azimuth(
        ST_Transform(ST_LineInterpolatePoint(road_geom, frac1), 4326),
        ST_Transform(ST_LineInterpolatePoint(road_geom, frac2), 4326)
    ));
    
    -- Normalize bearing to 0-359
    IF bearing < 0 THEN
        bearing := bearing + 360;
    END IF;
    
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

-- Main enrichment function - processes all unprocessed cameras
CREATE OR REPLACE FUNCTION enrich_cameras(
    search_radius_m DOUBLE PRECISION DEFAULT 120.0,
    corridor_half_length_m DOUBLE PRECISION DEFAULT 60.0
)
RETURNS TABLE(
    processed INTEGER,
    matched INTEGER,
    no_match INTEGER
) AS $$
DECLARE
    rec RECORD;
    road RECORD;
    corridor RECORD;
    v_processed INTEGER := 0;
    v_matched INTEGER := 0;
    v_no_match INTEGER := 0;
BEGIN
    -- Process each unprocessed camera
    FOR rec IN 
        SELECT * FROM cameras_staging 
        WHERE processed_at IS NULL
    LOOP
        -- Find nearest road using KNN with bounding box prefilter
        SELECT 
            r.osm_id,
            r.name,
            r.highway,
            r.maxspeed,
            r.oneway,
            r.geom,
            ST_ClosestPoint(r.geom, rec.geom) AS snap_point,
            ST_Distance(r.geom, rec.geom) AS dist_m
        INTO road
        FROM roads r
        WHERE r.geom && ST_Expand(rec.geom, search_radius_m)  -- Bounding box prefilter
        ORDER BY r.geom <-> rec.geom  -- KNN operator
        LIMIT 1;
        
        IF road IS NOT NULL AND road.dist_m <= search_radius_m THEN
            -- Get corridor endpoints
            SELECT * INTO corridor 
            FROM create_corridor(road.geom, road.snap_point, corridor_half_length_m);
            
            -- Update camera record
            UPDATE cameras_staging SET
                snap_geom = road.snap_point,
                road_osm_id = road.osm_id,
                road_name = road.name,
                road_class = road.highway,
                corridor_p1 = corridor.p1,
                corridor_p2 = corridor.p2,
                bearing = corridor.bearing,
                maxspeed = road.maxspeed,
                oneway = road.oneway,
                snap_distance_m = road.dist_m,
                processed_at = NOW()
            WHERE id = rec.id;
            
            v_matched := v_matched + 1;
        ELSE
            -- No match found within radius
            UPDATE cameras_staging SET
                processed_at = NOW()
            WHERE id = rec.id;
            
            v_no_match := v_no_match + 1;
        END IF;
        
        v_processed := v_processed + 1;
    END LOOP;
    
    processed := v_processed;
    matched := v_matched;
    no_match := v_no_match;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- Batch enrichment - more efficient for large datasets
CREATE OR REPLACE FUNCTION enrich_cameras_batch(
    search_radius_m DOUBLE PRECISION DEFAULT 120.0,
    corridor_half_length_m DOUBLE PRECISION DEFAULT 60.0,
    batch_size INTEGER DEFAULT 1000
)
RETURNS TABLE(
    total_processed BIGINT,
    total_matched BIGINT,
    total_no_match BIGINT
) AS $$
BEGIN
    -- Use a single UPDATE with LATERAL join for efficiency
    WITH matched AS (
        UPDATE cameras_staging c
        SET
            snap_geom = sub.snap_point,
            road_osm_id = sub.osm_id,
            road_name = sub.name,
            road_class = sub.highway,
            corridor_p1 = sub.p1,
            corridor_p2 = sub.p2,
            bearing = sub.bearing,
            maxspeed = sub.maxspeed,
            oneway = sub.oneway,
            snap_distance_m = sub.dist_m,
            processed_at = NOW()
        FROM (
            SELECT 
                c2.id,
                r.osm_id,
                r.name,
                r.highway,
                r.maxspeed,
                r.oneway,
                ST_ClosestPoint(r.geom, c2.geom) AS snap_point,
                ST_Distance(r.geom, c2.geom) AS dist_m,
                corr.p1,
                corr.p2,
                corr.bearing
            FROM cameras_staging c2
            CROSS JOIN LATERAL (
                SELECT 
                    r2.osm_id, r2.name, r2.highway, r2.maxspeed, r2.oneway, r2.geom
                FROM roads r2
                WHERE r2.geom && ST_Expand(c2.geom, search_radius_m)
                ORDER BY r2.geom <-> c2.geom
                LIMIT 1
            ) r
            CROSS JOIN LATERAL create_corridor(r.geom, ST_ClosestPoint(r.geom, c2.geom), corridor_half_length_m) corr
            WHERE c2.processed_at IS NULL
              AND ST_Distance(r.geom, c2.geom) <= search_radius_m
        ) sub
        WHERE c.id = sub.id
        RETURNING c.id
    )
    SELECT COUNT(*) INTO total_matched FROM matched;
    
    -- Mark remaining as no-match
    WITH no_match AS (
        UPDATE cameras_staging
        SET processed_at = NOW()
        WHERE processed_at IS NULL
        RETURNING id
    )
    SELECT COUNT(*) INTO total_no_match FROM no_match;
    
    total_processed := total_matched + total_no_match;
    
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

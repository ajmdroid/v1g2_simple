-- Enrich cameras in :stage_table into :out_table using nearest road
-- Usage:
--   psql postgresql://osm:osm@localhost:5432/osm \
--        -v stage_table=cameras_stage -v out_table=cameras_enriched \
--        -v radius_m=120 -v span_m=60 -v width_m=35 \
--        -f enrich_cameras.sql

-- Drop output table if exists
DROP TABLE IF EXISTS :out_table;

-- Build enriched cameras
CREATE TABLE :out_table AS
WITH nearest AS (
    SELECT DISTINCT ON (c.id)
        c.id,
        c.src,
        r.way_id AS osm_id,
        r.highway,
        r.name,
        r.ref,
        r.oneway,
        r.maxspeed,
        r.maxspeed_forward,
        r.maxspeed_backward,
        ST_ClosestPoint(r.geom, c.geom) AS snap_pt,
        ST_DistanceSphere(r.geom, c.geom) AS dist_m,
        r.geom AS road_geom
    FROM :stage_table c
    JOIN roads r ON r.geom && ST_Expand(c.geom, :radius_m / 111320.0)
    WHERE ST_DWithin(r.geom::geography, c.geom::geography, :radius_m)
    ORDER BY c.id, r.geom <-> c.geom
),
segments AS (
    SELECT
        n.*,
        ST_LineLocatePoint(n.road_geom, n.snap_pt) AS frac,
        NULLIF(ST_Length(n.road_geom::geography), 0) AS road_len_m
    FROM nearest n
),
windowed AS (
    SELECT
        s.*,
        GREATEST(0.0, s.frac - ((:span_m)::double precision / NULLIF(s.road_len_m,0))) AS f1,
        LEAST(1.0, s.frac + ((:span_m)::double precision / NULLIF(s.road_len_m,0))) AS f2
    FROM segments s
),
pts AS (
    SELECT
        w.*,
        ST_LineInterpolatePoint(w.road_geom, w.f1) AS p1_geom,
        ST_LineInterpolatePoint(w.road_geom, w.f2) AS p2_geom
    FROM windowed w
)
SELECT
    p.id,
    -- Merge corridor and road metadata into original JSON
    p.src
        || jsonb_build_object(
            'p1', ARRAY[round(ST_Y(p.p1_geom)::numeric, 6), round(ST_X(p.p1_geom)::numeric, 6)],
            'p2', ARRAY[round(ST_Y(p.p2_geom)::numeric, 6), round(ST_X(p.p2_geom)::numeric, 6)],
            'w', (:width_m)::int,
            'brg', (floor(degrees(ST_Azimuth(p.p1_geom, p.p2_geom)))::int + 360) % 360
        )
        || jsonb_strip_nulls(jsonb_build_object(
            'maxspeed', p.maxspeed,
            'maxspeed_fwd', p.maxspeed_forward,
            'maxspeed_back', p.maxspeed_backward,
            'oneway', p.oneway,
            'road_class', p.highway,
            'road_name', p.name,
            'road_ref', p.ref
        )) AS enriched,
    p.p1_geom,
    p.p2_geom,
    p.snap_pt,
    p.dist_m
FROM pts p;

-- Indexes
CREATE INDEX ON :out_table USING GIST(p1_geom);
CREATE INDEX ON :out_table USING GIST(p2_geom);
CREATE INDEX ON :out_table USING GIST(snap_pt);

-- Stats
ANALYZE :out_table;

SELECT count(*) AS enriched_count FROM :out_table;

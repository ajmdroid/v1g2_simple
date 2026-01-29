-- osm2pgsql flex output configuration
-- Extracts roads with relevant tags for camera enrichment
-- Compatible with osm2pgsql 1.6+ (Ubuntu 22.04 repo version)

-- Highway classes we care about (roads where cameras might be placed)
local highway_classes = {
    'motorway', 'motorway_link',
    'trunk', 'trunk_link', 
    'primary', 'primary_link',
    'secondary', 'secondary_link',
    'tertiary', 'tertiary_link',
    'residential',
    'unclassified',
    'service',
    'living_street',
}

-- Convert to set for fast lookup
local highway_set = {}
for _, v in ipairs(highway_classes) do
    highway_set[v] = true
end

-- Minimal roads table: one record per highway way
-- In osm2pgsql 1.6.x flex mode, geometry column uses 'geometry' type directly
local roads = osm2pgsql.define_way_table('roads', {
    { column = 'highway', type = 'text', not_null = true },
    { column = 'name', type = 'text' },
    { column = 'ref', type = 'text' },
    { column = 'oneway', type = 'text' },
    { column = 'maxspeed', type = 'text' },
    { column = 'maxspeed_forward', type = 'text' },
    { column = 'maxspeed_backward', type = 'text' },
    { column = 'geom', type = 'linestring', projection = 4326 },
})

function osm2pgsql.process_way(object)
    local highway = object.tags.highway
    
    -- Only process ways with highway tag in our list
    if not highway or not highway_set[highway] then
        return
    end
    
    -- Insert the road - geometry is passed via { create = ... } in older API
    roads:add_row({
        highway = highway,
        name = object.tags.name,
        ref = object.tags.ref,
        oneway = object.tags.oneway,
        maxspeed = object.tags.maxspeed,
        maxspeed_forward = object.tags['maxspeed:forward'],
        maxspeed_backward = object.tags['maxspeed:backward'],
        geom = { create = 'line' },
    })
end

-- We don't need nodes or relations for this use case
function osm2pgsql.process_node(object)
end

function osm2pgsql.process_relation(object)
end

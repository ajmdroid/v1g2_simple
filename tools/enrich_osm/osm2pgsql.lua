-- osm2pgsql flex output configuration
-- Extracts roads with relevant tags for camera enrichment

local roads = osm2pgsql.define_way_table('roads', {
    { column = 'osm_id', sql_type = 'bigint', create_only = true },
    { column = 'name', type = 'text' },
    { column = 'highway', type = 'text', not_null = true },
    { column = 'maxspeed', type = 'text' },
    { column = 'oneway', type = 'text' },
    { column = 'ref', type = 'text' },
    { column = 'lanes', type = 'text' },
    { column = 'surface', type = 'text' },
    { column = 'geom', type = 'linestring', projection = 3857, not_null = true },
})

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

function osm2pgsql.process_way(object)
    local highway = object.tags.highway
    
    -- Only process ways with highway tag in our list
    if not highway or not highway_set[highway] then
        return
    end
    
    -- Insert the road
    roads:insert({
        osm_id = object.id,
        name = object.tags.name,
        highway = highway,
        maxspeed = object.tags.maxspeed,
        oneway = object.tags.oneway,
        ref = object.tags.ref,
        lanes = object.tags.lanes,
        surface = object.tags.surface,
        geom = object:as_linestring(),
    })
end

-- We don't need nodes or relations for this use case
function osm2pgsql.process_node(object)
end

function osm2pgsql.process_relation(object)
end

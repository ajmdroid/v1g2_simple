# Road Map Binary Format (`road_map.bin`)

Compact spatial index of simplified US road geometry for lockout-zone
road snapping on the ESP32. Stored on SD card, loaded into PSRAM at boot.
All runtime queries are pure pointer math with zero SD I/O.

**Source**: OpenStreetMap motorway / trunk / primary roads  
**Simplification**: Ramer-Douglas-Peucker (configurable, default 100 m)  
**Target size**: ~2-4.5 MB for the lower 48 states  
**Generator**: `scripts/build_road_map.py`

---

## Overview

```text
┌─────────────────────────┐  offset 0
│  Header (64 B)          │
├─────────────────────────┤  gridIndexOffset (64)
│  Road Grid Index        │  rows × cols × 8 bytes
├─────────────────────────┤  segDataOffset
│  Segment Data           │  variable-length records
└─────────────────────────┘  fileSize
```

Each segment is assigned to exactly one grid cell, using the cell containing
its midpoint. The ESP32 reader searches the 3×3 neighbourhood around the query
point to find nearby segments. Segments whose bounding box exceeds
`cellSize × 0.9` are split during generation.

The header keeps two legacy reserved fields at offsets `56` and `60` so older
files still parse cleanly. Current firmware and current builder output leave
those fields at zero.

---

## Header (64 bytes)

| Offset | Size | Type     | Field               | Description |
|--------|------|----------|---------------------|-------------|
| 0      | 4    | char[4]  | `magic`             | `"RMAP"` |
| 4      | 1    | uint8    | `version`           | Format version (`2`) |
| 5      | 1    | uint8    | `flags`             | Reserved (0) |
| 6      | 1    | uint8    | `roadClassCount`    | Number of included road classes (3) |
| 7      | 1    | uint8    | `reserved`          | 0 |
| 8      | 4    | int32    | `minLatE5`          | Bounding box south (× 10⁵) |
| 12     | 4    | int32    | `maxLatE5`          | Bounding box north |
| 16     | 4    | int32    | `minLonE5`          | Bounding box west |
| 20     | 4    | int32    | `maxLonE5`          | Bounding box east |
| 24     | 2    | uint16   | `gridRows`          | Number of grid rows |
| 26     | 2    | uint16   | `gridCols`          | Number of grid columns |
| 28     | 4    | int32    | `cellSizeE5`        | Cell size in E5 (50000 = 0.5°) |
| 32     | 4    | uint32   | `totalSegments`     | Total segments in file |
| 36     | 4    | uint32   | `totalPoints`       | Total points across all segments |
| 40     | 2    | uint16   | `toleranceCm`       | RDP tolerance (cm); 10000 = 100 m |
| 42     | 2    | uint16   | `reserved2`         | 0 |
| 44     | 4    | uint32   | `gridIndexOffset`   | Byte offset to grid index (64) |
| 48     | 4    | uint32   | `segDataOffset`     | Byte offset to segment data |
| 52     | 4    | uint32   | `fileSize`          | Total file size in bytes |
| 56     | 4    | uint32   | `cameraIndexOffset` | Legacy reserved field, currently `0` |
| 60     | 4    | uint32   | `cameraCount`       | Legacy reserved field, currently `0` |

All multi-byte values are **little-endian**.

---

## Grid Index

Starts at `gridIndexOffset` (immediately after the header).  
Layout: `gridRows × gridCols` entries, row-major order.

| Offset | Size | Type   | Field        | Description |
|--------|------|--------|--------------|-------------|
| 0      | 4    | uint32 | `dataOffset` | Byte offset from segment-data start to this cell's records |
| 4      | 2    | uint16 | `count`      | Number of segments in this cell |
| 6      | 2    | uint16 | `reserved`   | 0 |

Empty cells have `count = 0` and `dataOffset = 0`.

**Cell lookup**:

```c
int row = (latE5 - minLatE5) / cellSizeE5;
int col = (lonE5 - minLonE5) / cellSizeE5;
// clamp to [0, rows-1] and [0, cols-1]
uint32_t idx_offset = gridIndexOffset + (row * gridCols + col) * 8;
```

---

## Segment Data

Starts at `segDataOffset`. Each segment is a variable-length record.

| Offset | Size | Type   | Field        | Description |
|--------|------|--------|--------------|-------------|
| 0      | 1    | uint8  | `roadClass`  | 0 = motorway, 1 = trunk, 2 = primary |
| 1      | 1    | uint8  | `flags`      | bit 0 = one-way |
| 2      | 2    | uint16 | `pointCount` | Number of points (≥ 2) |
| 4      | 4    | int32  | `lat0E5`     | First point latitude (absolute E5) |
| 8      | 4    | int32  | `lon0E5`     | First point longitude (absolute E5) |
| 12     | N×4  | int16² | `deltas`     | Subsequent points: `{dLatE5, dLonE5}` pairs |
| 12+N×4 | 1    | uint8  | `speedMph`   | Speed limit (mph), 0 = unknown **(v2 only)** |

**Segment size (v2)** = `12 + (pointCount - 1) × 4 + 1` bytes.  
**Segment size (v1)** = `12 + (pointCount - 1) × 4` bytes (no speed byte).

Delta values are relative to the previous point and clamped to
`[-32768, 32767]` in E5 (≈ ±36 km). Segments exceeding this are
pre-split by the generator.

---

## Road Class Enum

| Value | OSM `highway=`              | Description |
|-------|-----------------------------|-------------|
| 0     | `motorway`, `motorway_link` | Interstate / freeway |
| 1     | `trunk`, `trunk_link`       | US highway |
| 2     | `primary`, `primary_link`   | State highway |

---

## ESP32 Reader Algorithm

At boot, the entire file is loaded into PSRAM via `ps_malloc()`.
All queries are pure pointer math with zero SD I/O at runtime.

### Road Snap (`snapToRoad`)

1. Compute target cell `(row, col)` from query `latE5, lonE5`.
2. For each cell in the 3×3 neighbourhood:
   a. Look up grid entry; skip if `count == 0`.
   b. Walk variable-length segment records.
   c. Compute minimum point-to-polyline distance per edge.
   d. Track nearest segment, snap point, and road heading.
3. If nearest distance is below the snap radius:
   - Return snap point, road class, heading, speed limit, and oneway flag.
4. Otherwise, return `valid == false`.

---

## File Size Estimates

| Configuration            | Approx Size |
|--------------------------|-------------|
| Motorway+Trunk, 100m tol | ~4 MB       |
| + speed limits (v2 byte) | +0.2 MB     |
| Motorway+Trunk+Primary   | ~4.5 MB     |

Fits within the ESP32-S3's 8 MB PSRAM with headroom for audio,
fonts, BLE buffers, and other allocations.

---

## Version History

| Version | Changes |
|---------|---------|
| 1       | Initial format: road segments with grid spatial index |
| 2       | Trailing `speedMph` byte per segment; legacy reserved fields retained at offsets 56/60 |

---

## Builder Usage

```bash
# Roads only (v2 with speed limits)
python scripts/build_road_map.py --pbf us-latest.osm.pbf

# Quick test (Colorado)
python scripts/build_road_map.py --download --region test
```

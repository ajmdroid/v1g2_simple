# Road Map Binary Format (`road_map.bin`)

Compact spatial index of simplified US road geometry for lockout zone
road-snapping on the ESP32. Stored on SD card, read at lockout promotion
time to snap zone centres to the nearest road and derive road heading.

**Source**: OpenStreetMap motorway / trunk / primary roads  
**Simplification**: Ramer-Douglas-Peucker (configurable, default 100 m)  
**Target size**: ~1–3 MB for all 48 US states  
**Generator**: `scripts/build_road_map.py`

---

## Overview

```
┌─────────────────┐  offset 0
│  Header (64 B)  │
├─────────────────┤  offset 64
│   Grid Index    │  rows × cols × 8 bytes
├─────────────────┤  offset 64 + grid_size
│  Segment Data   │  variable length
└─────────────────┘  offset = fileSize
```

Each segment is assigned to exactly one grid cell (the cell containing
its midpoint). The ESP32 reader searches the 3×3 neighbourhood around
the query point to find all nearby segments. Segments whose bounding
box exceeds `cellSize × 0.9` are split during generation.

---

## Header (64 bytes)

| Offset | Size | Type     | Field              | Description |
|--------|------|----------|--------------------|-------------|
| 0      | 4    | char[4]  | `magic`            | `"RMAP"` |
| 4      | 1    | uint8    | `version`          | Format version (1) |
| 5      | 1    | uint8    | `flags`            | Reserved (0) |
| 6      | 1    | uint8    | `roadClassCount`   | Number of included road classes (3) |
| 7      | 1    | uint8    | `reserved`         | 0 |
| 8      | 4    | int32    | `minLatE5`         | Bounding box south (× 10⁵) |
| 12     | 4    | int32    | `maxLatE5`         | Bounding box north |
| 16     | 4    | int32    | `minLonE5`         | Bounding box west |
| 20     | 4    | int32    | `maxLonE5`         | Bounding box east |
| 24     | 2    | uint16   | `gridRows`         | Number of grid rows |
| 26     | 2    | uint16   | `gridCols`         | Number of grid columns |
| 28     | 4    | int32    | `cellSizeE5`       | Cell size in E5 (50000 = 0.5°) |
| 32     | 4    | uint32   | `totalSegments`    | Total segments in file |
| 36     | 4    | uint32   | `totalPoints`      | Total points across all segments |
| 40     | 2    | uint16   | `toleranceCm`      | RDP tolerance (cm); 10000 = 100 m |
| 42     | 2    | uint16   | `reserved2`        | 0 |
| 44     | 4    | uint32   | `gridIndexOffset`  | Byte offset to grid index (64) |
| 48     | 4    | uint32   | `segDataOffset`    | Byte offset to segment data |
| 52     | 4    | uint32   | `fileSize`         | Total file size in bytes |
| 56     | 8    | uint8[8] | `reserved3`        | Pad to 64 bytes |

All multi-byte values are **little-endian**.

---

## Grid Index

Starts at `gridIndexOffset` (immediately after header).  
Layout: `gridRows × gridCols` entries, row-major order.

| Offset | Size | Type   | Field        | Description |
|--------|------|--------|--------------|-------------|
| 0      | 4    | uint32 | `dataOffset` | Byte offset from `segDataOffset` to this cell's segments |
| 4      | 2    | uint16 | `segCount`   | Number of segments in this cell |
| 6      | 2    | uint16 | `reserved`   | 0 |

Empty cells have `segCount = 0` and `dataOffset = 0`.

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

**Segment size** = `12 + (pointCount - 1) × 4` bytes.

Delta values are relative to the previous point and clamped to
`[-32768, 32767]` in E5 (≈ ±36 km). Segments exceeding this are
pre-split by the generator.

---

## Road Class Enum

| Value | OSM `highway=`            | Description |
|-------|---------------------------|-------------|
| 0     | `motorway`, `motorway_link` | Interstate / freeway |
| 1     | `trunk`, `trunk_link`       | US highway |
| 2     | `primary`, `primary_link`   | State highway |

---

## ESP32 Reader Algorithm

At lockout promotion time:

1. Open `/road_map.bin` on SD (graceful no-op if absent).
2. Read 64-byte header; verify `magic == "RMAP"` and `version == 1`.
3. Compute target cell `(row, col)` from candidate `latE5, lonE5`.
4. For each cell in the 3×3 neighbourhood of `(row, col)`:
   a. Read 8-byte grid index entry.
   b. If `segCount == 0`, skip.
   c. Seek to `segDataOffset + dataOffset`.
   d. Read and iterate `segCount` segments.
   e. For each segment, compute minimum point-to-polyline distance.
   f. Track nearest segment + snap point + road heading.
5. If nearest distance < snap radius (e.g. 50 m):
   - Update lockout centre to snap point.
   - Set `headingDeg` from road bearing at snap point.
   - Set `directionMode = DIRECTION_FORWARD`.
6. Otherwise, fall through to GPS-derived heading (existing behaviour).

---

## File Size Estimates

| Tolerance | Motorway+Trunk+Primary | Motorway+Trunk only |
|-----------|------------------------|---------------------|
| 100 m     | ~3–5 MB                | ~1–2 MB             |
| 200 m     | ~1.5–3 MB              | ~0.8–1.5 MB         |
| 300 m     | ~1–2 MB                | ~0.5–1 MB           |

Adjust `--tolerance` and `--classes` to hit the target file size.

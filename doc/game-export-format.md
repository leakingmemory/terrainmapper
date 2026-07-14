# TerrainMapper Game Export Format

Tiled terrain, railway track, and road data for game engine consumption.

All coordinates are in **EPSG:25833** (UTM zone 33N, ETRS89) — a metric
projection covering Norway. X increases eastward, Y increases northward.
Elevations are metres above sea level.

All binary files use **little-endian** byte order.

## Directory layout

```
<export_root>/
  manifest.json
  tiles/
    0/                        ← LOD level
      123_456/                ← col_row
        terrain.hm32
        landcover.u8            ← optional (present if AR50 was loaded)
        tracks.bin
        roads.bin
        meta.json
    1/
      61_228/
        ...
    2/
      ...
    3/
      ...
```

## LOD levels

Each tile is 256 × 256 pixels. The ground area covered depends on the
LOD level, which is assigned by distance to the nearest railway track:

| LOD | Distance to rail | Resolution | Tile ground extent |
|-----|------------------|------------|--------------------|
| 0   | 0–200 m          | 10 m/px    | 2,560 m            |
| 1   | 200 m – 1 km     | 20 m/px    | 5,120 m            |
| 2   | 1–5 km           | 40 m/px    | 10,240 m           |
| 3   | 5–20 km          | 80 m/px    | 20,480 m           |

Higher-LOD (finer) tiles take priority: if a region is covered by a LOD 0
tile, no coarser tile will duplicate that coverage. At LOD boundaries the
game engine is responsible for blending or stitching between different
resolutions.

## terrain.hm32

Raw 256 × 256 array of IEEE 754 **float32** values (262,144 bytes).

- Row-major order, north-to-south: row 0 is the northern edge of the
  tile, row 255 is the southern edge.
- Each value is elevation in metres above sea level.
- Sample points are at pixel centres: the first sample is at
  `(originX + 0.5 * resolution, originY + extent - 0.5 * resolution)`.
- Nodata gaps (areas outside DTM coverage) are filled by iterative
  nearest-neighbour interpolation during export. A value of **-9999.0**
  indicates a gap that could not be filled.

### Loading example (Python)

```python
import numpy as np

tile = np.fromfile("tiles/0/123_456/terrain.hm32", dtype="<f4")
heightmap = tile.reshape(256, 256)
```

### Loading example (C)

```c
float heightmap[256][256];
FILE* f = fopen("tiles/0/123_456/terrain.hm32", "rb");
fread(heightmap, sizeof(float), 256 * 256, f);
fclose(f);
/* heightmap[row][col], row 0 = north */
```

## landcover.u8

Optional per-tile land-cover raster. Present only when an AR50 land-cover
dataset was loaded before exporting; older/plain exports omit the file, and
consumers should treat a missing file as "no land-cover data".

Raw 256 × 256 array of **uint8** values (65,536 bytes), on the **same grid as
`terrain.hm32`** (row-major, row 0 = north, pixel centres). Each byte is a
Norwegian **AR50 `artype`** land-cover code:

| Code | Class        |
|------|--------------|
| 0    | Unclassified / no data (outside AR50 coverage) |
| 10   | Built-up     |
| 20   | Agriculture  |
| 30   | Forest       |
| 50   | Open land    |
| 60   | Bog / wetland|
| 70   | Glacier      |
| 80   | Freshwater   |
| 81   | Sea          |

Rasterised from the AR50 polygons (EPSG:4258) into each tile's grid, so the
resolution follows the tile's LOD (10 m at LOD 0 … 80 m at LOD 3).

## tracks.bin

Binary file containing railway track segments that intersect this tile.
Track segments that cross tile boundaries are included in full (not
clipped) — the game engine should clip or deduplicate as needed. Track
segments are identified by a globally unique `trackId` that is consistent
across tiles.

### Layout

```
Offset  Type      Field
──────  ────      ─────
0       uint32    numSegments

Per segment (repeated numSegments times):
+0      uint32    trackId         globally unique track identifier
+4      uint8     trackType       0 = main line
                                  1 = siding
                                  2 = yard track
+5      uint8     medium          0x20 (' ') = surface
                                  0x55 ('U') = underground/tunnel
                                  0x4C ('L') = bridge
                                  0x42 ('B') = bridge (alt.)
                                  0x54 ('T') = metro/tube
+6      uint8     electrified     0 = no, 1 = yes
+7      uint8     reserved        always 0
+8      uint32    numVertices

Per vertex (repeated numVertices times):
+0      float32   x               easting  (EPSG:25833)
+4      float32   y               northing (EPSG:25833)
+8      float32   z               elevation (metres above sea level)

Then, after the vertex block (repeated numVertices times):
+0      uint16    speed           line speed km/h at this vertex
                                  (0 = unknown; main-line only)
```

Speed is matched from OpenStreetMap `maxspeed` on `railway_tracks` (nearest
vertex within 30 m, non-service ways only) and is piecewise-constant along the
line. Sidings and yard tracks have no speed data, so every entry is 0.

### Track types

- **Main line** (`trackType = 0`): sourced from the Norwegian national
  railway register (Banelenke GML). Elevation is profiled from DTM10
  with smoothing, plausibility filtering (≥ 2.6 m), track bed offset
  (+0.6 m), and tunnel interpolation applied.
- **Siding** (`trackType = 1`): sourced from OpenStreetMap
  (`service=siding`). Elevation from DTM10 with plausibility filter and
  track bed offset, but no smoothing.
- **Yard** (`trackType = 2`): sourced from OpenStreetMap
  (`service=yard` or `usage=industrial`). Same treatment as sidings.

### Medium values

Medium describes the physical environment of the track segment:

| Value | Char | Meaning |
|-------|------|---------|
| 0x20  | ' '  | Surface (open air, on embankment or in cutting) |
| 0x55  | 'U'  | Underground — tunnel. Elevation is interpolated linearly between portal elevations. |
| 0x4C  | 'L'  | Bridge |
| 0x42  | 'B'  | Bridge (alternative code) |
| 0x54  | 'T'  | Metro / tube section |

Main-line tracks are split into separate segments at medium transitions,
so all vertices within a single segment share the same medium value.

### Elevation processing pipeline

Main-line track elevation goes through:

1. **Raw DTM sampling** at 50 m intervals along the geometry
2. **Plausibility filter**: elevations below 2.0 m are rejected as
   coastal DTM artefacts
3. **Track bed offset**: +0.6 m added (0.3 m ballast + 0.25 m sleeper
   \+ 0.05 m rail head)
4. **Gaussian smoothing** with gradient limiting (max 2.5 % grade) to
   simulate railway earthworks
5. **Post-smoothing clamp**: no surface point below 2.6 m
6. **Tunnel interpolation**: linear interpolation between portal
   elevations for underground sections

## roads.bin

Binary file containing road segments that intersect this tile. Same
deduplication note as tracks — segments are not clipped to tile bounds.

### Layout

```
Offset  Type      Field
──────  ────      ─────
0       uint32    numSegments

Per segment (repeated numSegments times):
+0      uint8     kategori        road category as ASCII character:
                                  'E' = Europavei (European route)
                                  'R' = Riksvei (national road)
                                  'F' = Fylkesvei (county road)
                                  'K' = Kommunal vei (municipal road)
                                  'P' = Privat vei (private road)
+1      uint8[3]  reserved        always 0
+4      uint32    nummer           road number (e.g. 6 for E6)
+8      uint32    numVertices

Per vertex (repeated numVertices times):
+0      float32   x               easting  (EPSG:25833)
+4      float32   y               northing (EPSG:25833)
+8      float32   z               elevation (metres above sea level)
```

Road elevation comes from the source GML geometry (3D coordinates from
the national road register), not from DTM sampling.

## meta.json

Per-tile JSON metadata. Example:

```json
{
  "lod": 0,
  "col": 123,
  "row": 456,
  "originX": 314880.000000,
  "originY": 7488000.000000,
  "extent": 2560.000000,
  "resolution": 10.000000,
  "pixels": 256,
  "trackSegments": 3,
  "roadSegments": 12,
  "stations": [
    {
      "name": "Oslo S",
      "x": 315210.5,
      "y": 7488432.0,
      "z": 12.3,
      "type": "I",
      "line": "Bergensbanen"
    }
  ],
  "connections": [
    {
      "trackA": 14,
      "trackB": 207,
      "x": 315300.0,
      "y": 7488500.0,
      "z": 11.8
    }
  ]
}
```

### Fields

| Field          | Type    | Description |
|----------------|---------|-------------|
| lod            | int     | LOD level (0–3) |
| col            | int     | Tile column in this LOD's grid |
| row            | int     | Tile row in this LOD's grid |
| originX        | float   | Easting of tile's south-west corner (EPSG:25833) |
| originY        | float   | Northing of tile's south-west corner (EPSG:25833) |
| extent         | float   | Tile size in metres (both X and Y) |
| resolution     | float   | Ground metres per heightmap pixel |
| pixels         | int     | Heightmap dimension (always 256) |
| trackSegments  | int     | Number of track segments in tracks.bin |
| roadSegments   | int     | Number of road segments in roads.bin |
| stations       | array   | Railway stations within this tile |
| connections    | array   | Track-to-track connection points (switches) |

### Station object

| Field | Type   | Description |
|-------|--------|-------------|
| name  | string | Station name |
| x, y  | float  | EPSG:25833 coordinates |
| z     | float  | Elevation (m) |
| type  | string | `"S"` = station, `"I"` = interchange |
| line  | string | Railway line name (e.g. "Bergensbanen") |

### Connection object

Connections represent points where a siding or yard track meets a main
line (within 50 m snap tolerance). The game engine can place switch or
turnout models at these locations.

| Field  | Type   | Description |
|--------|--------|-------------|
| trackA | uint32 | Track ID of the main line segment |
| trackB | uint32 | Track ID of the siding/yard segment |
| x, y   | float  | EPSG:25833 coordinates of the snap point |
| z      | float  | Elevation at the connection (m) |

## manifest.json

Top-level file describing the entire export. Example:

```json
{
  "crs": "EPSG:25833",
  "origin": [-43520.000000, 6400000.000000],
  "bounds": [-43520.000000, 6400000.000000, 1136640.000000, 7920000.000000],
  "lodLevels": [
    {"lod": 0, "resolution": 10, "tileExtent": 2560, "tilePixels": 256, "count": 3200},
    {"lod": 1, "resolution": 20, "tileExtent": 5120, "tilePixels": 256, "count": 2400},
    {"lod": 2, "resolution": 40, "tileExtent": 10240, "tilePixels": 256, "count": 1800},
    {"lod": 3, "resolution": 80, "tileExtent": 20480, "tilePixels": 256, "count": 900}
  ],
  "tiles": [
    {"lod": 0, "col": 123, "row": 456, "x": 314880.0, "y": 7488000.0, "path": "tiles/0/123_456/"},
    ...
  ],
  "stats": {
    "totalTiles": 8300,
    "railLines": 42,
    "mainLineSegments": 1200,
    "sidingSegments": 3500,
    "totalTrackKm": 4200,
    "stationCount": 350,
    "connectionCount": 2800,
    "roadSegments": 45000
  }
}
```

### Fields

| Field     | Type   | Description |
|-----------|--------|-------------|
| crs       | string | Coordinate reference system (always EPSG:25833) |
| origin    | [x, y] | South-west corner of the bounding box |
| bounds    | [minX, minY, maxX, maxY] | Full bounding box (rail network + 20 km buffer) |
| lodLevels | array  | LOD level definitions with tile counts |
| tiles     | array  | Every tile with its position and directory path |
| stats     | object | Summary statistics for the whole export |

### Tile entry

| Field | Type   | Description |
|-------|--------|-------------|
| lod   | int    | LOD level |
| col   | int    | Grid column |
| row   | int    | Grid row |
| x     | float  | Origin easting (EPSG:25833) |
| y     | float  | Origin northing (EPSG:25833) |
| path  | string | Relative path to tile directory |

## Coordinate system notes

EPSG:25833 is UTM zone 33N on the ETRS89 datum. Coordinates are in
metres. For game engine use, the coordinates can be treated as a flat
Cartesian grid — at Norwegian latitudes the UTM distortion is negligible
for rendering purposes.

To convert a world position to a tile index:

```
col = floor(x / tileExtent)
row = floor(y / tileExtent)
```

To convert a world position to a heightmap pixel within a tile:

```
localX = x - (col * tileExtent)
localY = y - (row * tileExtent)
px = floor(localX / resolution)
py = 255 - floor(localY / resolution)    // row 0 = north
elevation = heightmap[py * 256 + px]
```

## Data sources

- **Terrain**: Kartverket DTM10 (10 m resolution digital terrain model)
- **Main railway lines**: Bane NOR / Kartverket Banenettverk GML
  (Banelenke and Stasjonsnode layers)
- **Sidings and yard tracks**: OpenStreetMap (ODbL v1.0)
  (c) OpenStreetMap contributors
- **Roads**: Statens vegvesen / Kartverket NVDB VegnettPluss GML

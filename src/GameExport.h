#pragma once

#include "ProfileData.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─── Track segment for export ──────────────────────────────────────
struct ExportTrack {
    uint32_t trackId;
    uint8_t  trackType;   // 0=main, 1=siding, 2=yard
    uint8_t  medium;      // ' ', 'U', 'L', 'T'
    uint8_t  electrified; // 0=no, 1=yes
    std::string lineName; // railway line name (e.g. "Bergensbanen")
    std::vector<float> x, y, z; // EPSG:25833 coordinates
    std::vector<uint16_t> speed; // per-vertex OSM maxspeed km/h (0=unknown)
};

// ─── Road segment for export ───────────────────────────────────────
struct ExportRoad {
    char     kategori;    // 'E', 'R', 'F', 'K', 'P'
    uint32_t nummer;
    std::vector<float> x, y, z;
};

// ─── Building for export ───────────────────────────────────────────
struct ExportBuilding {
    uint8_t  kind;        // 0=other,1=residential,2=commercial,3=industrial
    uint8_t  roof;        // 0=flat,1=gabled,2=hipped,3=pyramidal,4=skillion
    float    baseZ;       // ground elevation (m, sampled from DTM)
    float    height;      // wall/eaves height (m)
    std::vector<float> x, y; // exterior-ring footprint, EPSG:25833
};

// ─── Station for export ────────────────────────────────────────────
struct ExportStation {
    std::string name;
    float x, y, z;       // EPSG:25833
    char type;            // 'S' or 'I'
    std::string lineName;
};

// ─── Track connection point ────────────────────────────────────────
struct TrackConnection {
    uint32_t trackIdA;
    uint32_t trackIdB;
    float x, y, z;       // snap point
};

// ─── LOD level definition ──────────────────────────────────────────
struct LodLevel {
    int lod;
    double resolution;    // metres per pixel
    double tileExtent;    // metres (256 * resolution)
    double minDist;       // minimum distance to rail (metres)
    double maxDist;       // maximum distance to rail (metres)
};

// ─── Tile descriptor ───────────────────────────────────────────────
struct ExportTile {
    int lod;
    int col, row;
    double originX, originY; // south-west corner in EPSG:25833
    double extent;           // tile size in metres
    double resolution;       // metres per pixel
};

// ─── GameExporter ──────────────────────────────────────────────────
class GameExporter {
public:
    using ProgressCb = std::function<bool(int pct, const std::string& msg)>;

    // Run the full export pipeline
    bool Export(const std::string& outputDir,
               const std::string& railwayPath,
               const std::string& roadsPath,
               const std::string& osmDataPath,
               const std::vector<std::string>& zipPaths,
               const std::string& ar50Path = "",
               ProgressCb progress = nullptr);

private:
    ProfileData m_profileData;
    std::string m_ar50Path;   // AR50 land-cover .gdb path ("" = none)
    std::vector<ExportTrack> m_tracks;
    std::vector<ExportRoad> m_roads;
    std::vector<ExportBuilding> m_buildings;
    std::vector<ExportStation> m_stations;
    std::vector<TrackConnection> m_connections;
    std::vector<ExportTile> m_tiles;
    uint32_t m_nextTrackId = 1;

    // Rail segment bounding boxes for distance queries
public:
    struct RailBBox { double minX, minY, maxX, maxY; };
private:
    std::vector<RailBBox> m_railBBoxes;

    // Uniform grid indexing rail bboxes by cell, so MinDistToRail only checks
    // nearby segments instead of all of them (GenerateTileGrid calls it per
    // grid cell over the whole country). Built by BuildRailIndex().
    double m_railGridMinX = 0, m_railGridMinY = 0, m_railGridCell = 0;
    int m_railGridCols = 0, m_railGridRows = 0;
    std::vector<std::vector<int>> m_railGrid;
    void BuildRailIndex();

    // Precomputed 2D bounds per track/road, and a uniform grid indexing roads
    // (millions of segments) by cell — so per-tile clipping is fast instead of
    // an O(tiles x roads) rescan. Built once by BuildVectorIndex().
    struct BBox2D { float minX, minY, maxX, maxY; };
    std::vector<BBox2D> m_trackBBox;  // parallel to m_tracks
    std::vector<BBox2D> m_roadBBox;   // parallel to m_roads
    std::vector<BBox2D> m_buildingBBox; // parallel to m_buildings
    double m_gridMinX = 0, m_gridMinY = 0, m_gridCell = 0;
    int m_gridCols = 0, m_gridRows = 0;
    std::vector<std::vector<int>> m_roadGrid; // cell (r*cols+c) -> road indices
    void BuildVectorIndex();

    // Phase 1: collect rail geometry and build spatial index
    bool CollectRailGeometry(const std::string& railwayPath,
                             const std::string& osmDataPath,
                             ProgressCb progress);

    // Phase 2: profile all main lines
    bool ProfileMainLines(const std::string& railwayPath,
                          const std::string& roadsPath,
                          ProgressCb progress);

    // Phase 3: profile sidings from OSM
    bool ProfileSidings(const std::string& osmDataPath,
                        ProgressCb progress);

    // Phase 3b: match OSM maxspeed onto main-line track vertices
    bool MatchSpeedLimits(const std::string& osmDataPath,
                          ProgressCb progress);

    // Collect OSM building footprints (+ DTM base elevation) for export.
    bool CollectBuildings(const std::string& osmDataPath,
                          ProgressCb progress);

    // Phase 4: generate tile grid
    bool GenerateTileGrid(ProgressCb progress);

    // Phase 5: write terrain heightmaps
    bool WriteTerrainTiles(const std::string& outputDir,
                           ProgressCb progress);

    // Phase 6: clip and write vector data per tile
    bool WriteVectorData(const std::string& outputDir,
                         ProgressCb progress);
    // Writes one tile's tracks.bin/roads.bin/meta.json. Reads only immutable
    // geometry + indexes, so it is safe to call concurrently across tiles.
    // `seenRoads` is a per-thread scratch buffer (size m_roads.size()) used to
    // de-duplicate road candidates from the grid; `tileId` is its marker value.
    void WriteOneVectorTile(const ExportTile& tile, const std::string& outputDir,
                            std::vector<int>& seenRoads, int tileId) const;

    // Phase 7: write manifest
    bool WriteManifest(const std::string& outputDir,
                       ProgressCb progress);

    // Minimum distance from a point to any rail segment bbox
    double MinDistToRail(double x, double y) const;

    // Snap siding endpoints to nearest main-line vertex
    void SnapConnections();

    // LOD level definitions
    static const std::vector<LodLevel>& GetLodLevels();

    // Overall bounding box of rail network + buffer
    double m_boundsMinX = 1e18, m_boundsMinY = 1e18;
    double m_boundsMaxX = -1e18, m_boundsMaxY = -1e18;
};

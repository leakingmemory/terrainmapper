#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class GDALDataset;

struct ProfilePoint {
    double km;          // distance along line in km (from startposisjon)
    double elevation;   // metres above sea level
    double x, y;        // EPSG:25833 coordinates
    char medium;        // ' '=surface, U=underground, L/B=bridge, T=metro
    bool interpolated;  // true if elevation was interpolated (tunnel)
};

struct ProfileStation {
    double km;          // sporkilometer from GML
    double elevation;   // sampled from DTM
    std::string name;
    char type;          // S=station, I=interchange
};

struct TrackJunction {
    double km;            // distance along the profiled line
    double elevation;     // from profile
    double x, y;          // EPSG:25833 coordinate
    enum class Type {
        Switch,           // 3 ways — single turnout
        DoubleSwitch,     // 4+ ways with small divergence — crossover
        DiamondCrossing,  // 4+ ways with large angle — tracks cross
        Overpass,         // this line goes over another track
        Underpass,        // this line goes under another track
        RoadLevelCrossing,  // road at grade
        RoadOverpass,     // road goes over railway
        RoadUnderpass,    // road goes under railway
    };
    Type type;
    std::string description;  // other line name or road ID
    int numSwitches = 0;
};

struct ProfileStats {
    std::string lineName;
    double totalLengthKm;
    double minElev, maxElev;
    double totalClimb, totalDescent;  // cumulative metres
    double maxGradePct;               // steepest grade in percent
    double tunnelLengthKm;
    double bridgeLengthKm;
    int segmentCount;
    int stationCount;
};

struct TileIndexEntry {
    double minX, minY, maxX, maxY;  // bounding box in EPSG:25833
    std::string vsiPath;
    std::string projectionWkt;
    double gt[6];                   // geotransform
    int rasterW, rasterH;
};

struct ProfileResult {
    std::vector<ProfilePoint> points;
    std::vector<ProfileStation> stations;
    std::vector<TrackJunction> junctions;
    ProfileStats stats;
};

class OGRCoordinateTransformation;

// Cached raster tile for elevation sampling
struct CachedTile {
    int indexEntry = -1;           // which TileIndexEntry this corresponds to
    std::vector<float> elevation;  // raster data
    int w = 0, h = 0;
    double gt[6] = {};
    double invGt[6] = {};
    std::string projectionWkt;
    bool needsTransform = false;   // true if tile CRS != EPSG:25833
    OGRCoordinateTransformation* toTileCrs = nullptr;  // cached transform
    ~CachedTile();
    CachedTile() = default;
    CachedTile(CachedTile&& o) noexcept;
    CachedTile& operator=(CachedTile&& o) noexcept;
    CachedTile(const CachedTile&) = delete;
    CachedTile& operator=(const CachedTile&) = delete;
};

class ProfileData {
public:
    // Build tile index from the loaded zip files
    // Returns number of tiles indexed
    int BuildTileIndex(const std::vector<std::string>& zipPaths,
                       std::function<bool(int current, int total)> progress = nullptr);

    // Get list of distinct line names from the railway GML
    std::vector<std::string> GetLineNames(const std::string& railwayPath) const;

    // Progress callback: (percentage 0-100, stage description) → continue?
    using ProgressCb = std::function<bool(int pct, const std::string& msg)>;

    // Build a profile for the named line
    ProfileResult BuildProfile(const std::string& railwayPath,
                               const std::string& roadsPath,
                               const std::string& lineName,
                               ProgressCb progress = nullptr) const;

    // Sample elevation at an EPSG:25833 coordinate
    // Returns false if no tile covers the point
    bool SampleElevation(double x25833, double y25833, float& elevOut) const;

    // Parameterized smoothing: Gaussian terrain smoothing + gradient limiting
    // + vertical curve smoothing.  Used by both railway and road profiles.
    static void SmoothWithParams(std::vector<ProfilePoint>& points,
                                 double elevSigmaKm,
                                 double maxGrade,
                                 double gradeSigmaKm);

private:
    std::vector<TileIndexEntry> m_tileIndex;

    // LRU tile cache (mutable because sampling is logically const). Each cached
    // DTM cell is a full float raster (~100 MB). 8 balances memory against
    // reload thrashing when sampling geographically scattered siding tracks.
    mutable std::vector<CachedTile> m_tileCache;
    static constexpr int kMaxCachedTiles = 8;

    // Load a tile's raster data into cache, evicting LRU if needed
    const CachedTile* LoadTileIntoCache(int tileIdx) const;

    // Railway-specific: smooth + tunnel interpolation
    void SmoothProfile(std::vector<ProfilePoint>& points) const;
    void InterpolateTunnels(std::vector<ProfilePoint>& points) const;

    // Compute stats from assembled profile
    ProfileStats ComputeStats(const std::string& lineName,
                              const std::vector<ProfilePoint>& points,
                              const std::vector<ProfileStation>& stations) const;

    // Find track junctions and road crossings along the profiled line
    std::vector<TrackJunction> FindTrackJunctions(
        const std::string& railwayPath,
        const std::string& roadsPath,
        const std::string& lineName,
        const std::vector<ProfilePoint>& points) const;
};

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

    // Build a profile for the named line
    ProfileResult BuildProfile(const std::string& railwayPath,
                               const std::string& lineName) const;

private:
    std::vector<TileIndexEntry> m_tileIndex;

    // LRU tile cache (mutable because sampling is logically const)
    mutable std::vector<CachedTile> m_tileCache;
    static constexpr int kMaxCachedTiles = 8;

    // Sample elevation at an EPSG:25833 coordinate
    // Returns false if no tile covers the point
    bool SampleElevation(double x25833, double y25833, float& elevOut) const;

    // Load a tile's raster data into cache, evicting LRU if needed
    const CachedTile* LoadTileIntoCache(int tileIdx) const;

    // Interpolate elevation through tunnel sections
    void InterpolateTunnels(std::vector<ProfilePoint>& points) const;

    // Compute stats from assembled profile
    ProfileStats ComputeStats(const std::string& lineName,
                              const std::vector<ProfilePoint>& points,
                              const std::vector<ProfileStation>& stations) const;
};

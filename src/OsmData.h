#pragma once

#include <functional>
#include <string>
#include <vector>

struct TileBounds {
    double latMin, latMax;
    double lonMin, lonMax;
};

class OsmData {
public:
    // Progress callback: (percentage 0-100, stage description) -> continue?
    using ProgressCb = std::function<bool(int pct, const std::string& msg)>;

    // Compute a WGS84 bounding box that covers all loaded tiles.
    // Uses the lat/lon bounds already computed by MainFrame.
    struct BBox {
        double lonMin, latMin, lonMax, latMax;
    };
    static BBox ComputeTileBBox(const std::vector<TileBounds>& tileBounds);

    // Extract and convert OSM data for the given bounding box.
    // - osmPbfPath: path to an OSM PBF file (regional or planet)
    // - bbox: WGS84 bounding box to extract
    // - outputGpkg: path for the output GPKG (multiple layers)
    // - progress: optional callback
    // Returns total feature count, or -1 on error.
    static int Extract(const std::string& osmPbfPath,
                       const BBox& bbox,
                       const std::string& outputGpkg,
                       ProgressCb progress = nullptr);

    // Parse the "key"=>"value","key2"=>"value2" format in other_tags
    static std::string GetOtherTag(const char* otherTags, const char* key);
};

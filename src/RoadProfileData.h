#pragma once

#include "ProfileData.h"

#include <string>
#include <tuple>
#include <vector>

struct RoadId {
    char kategori = 'F';   // E, R, F, K, P
    int nummer = 0;

    std::string Label() const;
    bool operator<(const RoadId& o) const;
    bool operator==(const RoadId& o) const;
};

struct RoadIntersection {
    double km;            // distance along the profiled road
    double elevation;     // road surface elevation
    double x, y;          // EPSG:25833 coordinate
    RoadId crossingRoad;  // if road-road intersection
    std::string railLine; // if road-rail intersection
    bool isRailway;       // true = rail, false = road
    enum class CrossType {
        LevelCrossing,    // same grade
        Overpass,         // profiled road goes over
        Underpass,        // profiled road goes under
        Unknown
    };
    CrossType crossType = CrossType::Unknown;
};

struct RoadProfileResult {
    RoadId roadId;
    std::vector<ProfilePoint> points;
    std::vector<RoadIntersection> intersections;
    ProfileStats stats;
};

class RoadProfileData {
public:
    // Get list of distinct roads from the GPKG, sorted by category then number
    std::vector<RoadId> GetRoadList(const std::string& roadsPath) const;

    // Progress callback: (percentage 0-100, stage description) → continue?
    using ProgressCb = std::function<bool(int pct, const std::string& msg)>;

    // Build a profile for the given road
    RoadProfileResult BuildRoadProfile(
        const std::string& roadsPath,
        const std::string& railwayPath,
        const RoadId& road,
        ProfileData& profileData,
        ProgressCb progress = nullptr) const;

private:
    // Chain road segments into a continuous polyline
    // Returns (x, y, z) coordinates in EPSG:25833 with embedded elevation
    std::vector<std::tuple<double, double, double>> ChainSegments(
        const std::string& roadsPath,
        const RoadId& road) const;

    // Find intersections with other roads and railways
    std::vector<RoadIntersection> FindIntersections(
        const std::string& roadsPath,
        const std::string& railwayPath,
        const RoadId& excludeRoad,
        const std::vector<std::pair<double, double>>& chainedCoords,
        const std::vector<ProfilePoint>& profilePoints,
        ProfileData& profileData) const;

    // Smoothing parameters by road category
    struct SmoothParams {
        double elevSigmaKm;
        double maxGrade;
        double gradeSigmaKm;
        double sampleSpacing;  // metres
    };
    static SmoothParams GetSmoothParams(char kategori);
};

#pragma once

#include "ProfileData.h"

#include <string>
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
    double elevation;     // from DTM
    double x, y;          // EPSG:25833 coordinate
    RoadId crossingRoad;  // if road-road intersection
    std::string railLine; // if road-rail intersection
    bool isRailway;       // true = rail, false = road
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

    // Build a profile for the given road
    RoadProfileResult BuildRoadProfile(
        const std::string& roadsPath,
        const std::string& railwayPath,
        const RoadId& road,
        ProfileData& profileData) const;

private:
    // Chain road segments into a continuous polyline
    // Returns coordinates in EPSG:25833
    std::vector<std::pair<double, double>> ChainSegments(
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

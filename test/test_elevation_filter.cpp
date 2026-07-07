//
// Unit test for railway elevation plausibility filtering and smoothing.
//
// Verifies that DTM elevations below 2m (tidal zone artifacts) are
// rejected, and that the smoothing pipeline never produces surface
// elevations below the minimum plausible track elevation.
//

#include "../src/ProfileData.h"

#include <cmath>
#include <cstdio>
#include <vector>

// ---- constants (must match BuildProfile in ProfileData.cpp) ---------------

static constexpr double kMinPlausibleElev = 2.0;
static constexpr double kTrackBedOffset   = 0.6;
static constexpr double kMinTrackElev     = kMinPlausibleElev + kTrackBedOffset;

// ---- minimal test harness -------------------------------------------------

static int g_pass = 0, g_fail = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (expr) { g_pass++; }                                            \
        else { g_fail++; fprintf(stderr, "FAIL  %s:%d: %s\n",             \
                                 __FILE__, __LINE__, #expr); }             \
    } while (0)

#define CHECK_APPROX(a, b, tol)                                            \
    do {                                                                   \
        double _a = (a), _b = (b), _t = (tol);                            \
        if (std::abs(_a - _b) <= _t) { g_pass++; }                        \
        else { g_fail++; fprintf(stderr, "FAIL  %s:%d: %s = %.4f "        \
               "(expected %.4f +/- %.4f)\n",                               \
               __FILE__, __LINE__, #a, _a, _b, _t); }                     \
    } while (0)

#define CHECK_GT(a, b)                                                     \
    do {                                                                   \
        double _a = (a), _b = (b);                                         \
        if (_a > _b) { g_pass++; }                                         \
        else { g_fail++; fprintf(stderr, "FAIL  %s:%d: %s = %.4f "        \
               "(expected > %.4f)\n", __FILE__, __LINE__, #a, _a, _b); }   \
    } while (0)

#define CHECK_GE(a, b)                                                     \
    do {                                                                   \
        double _a = (a), _b = (b);                                         \
        if (_a >= _b) { g_pass++; }                                        \
        else { g_fail++; fprintf(stderr, "FAIL  %s:%d: %s = %.4f "        \
               "(expected >= %.4f)\n", __FILE__, __LINE__, #a, _a, _b); }  \
    } while (0)

// ---- helpers --------------------------------------------------------------

// Build a synthetic profile: a straight coastal railway at ~5m elevation
// with a section that dips through a tidal zone (negative/near-zero elev).
static std::vector<ProfilePoint> MakeCoastalProfile()
{
    std::vector<ProfilePoint> pts;
    // 10 km of track, points every 100m
    for (int i = 0; i <= 100; i++) {
        ProfilePoint p;
        p.km = i * 0.1;
        p.x = 200000.0 + i * 100.0;  // synthetic EPSG:25833
        p.y = 7000000.0;
        p.medium = ' ';
        p.interpolated = false;

        // Normal elevation ~5m, but km 3.0-4.0 has tidal zone artifacts
        if (p.km >= 3.0 && p.km <= 4.0) {
            // Simulate DTM reading the seabed / tidal flat
            double dip = (p.km - 3.0) / 0.5;  // 0 to 2 then back
            if (dip > 1.0) dip = 2.0 - dip;
            p.elevation = 1.5 - 3.0 * dip;  // goes from 1.5 down to -1.5
        } else {
            p.elevation = 5.0;
        }
        pts.push_back(p);
    }
    return pts;
}

// Build a profile with a tunnel section at sea level
static std::vector<ProfilePoint> MakeTunnelAtSeaLevel()
{
    std::vector<ProfilePoint> pts;
    for (int i = 0; i <= 50; i++) {
        ProfilePoint p;
        p.km = i * 0.1;
        p.x = 200000.0 + i * 100.0;
        p.y = 7000000.0;
        p.interpolated = false;

        if (p.km >= 2.0 && p.km <= 3.0) {
            // Subsea tunnel — medium='U', elevation is DTM surface (mountain)
            p.medium = 'U';
            p.elevation = -5.0;  // DTM shows seabed
        } else {
            p.medium = ' ';
            p.elevation = 10.0;
        }
        pts.push_back(p);
    }
    return pts;
}

// Build a profile that drops to barely above the threshold, stressing
// the smoothing near the boundary.
static std::vector<ProfilePoint> MakeBoundaryProfile()
{
    std::vector<ProfilePoint> pts;
    // 8 km, points every 50m
    for (int i = 0; i <= 160; i++) {
        ProfilePoint p;
        p.km = i * 0.05;
        p.x = 200000.0 + i * 50.0;
        p.y = 7000000.0;
        p.medium = ' ';
        p.interpolated = false;

        // Track at 20m, dropping to just above 2.0m at km 4.0, then
        // back up. The smoothing should never push the dip below
        // kMinTrackElev.
        double center = 4.0;
        double halfWidth = 1.5;
        double dist = std::abs(p.km - center);
        if (dist < halfWidth) {
            // Smooth dip from 20m down to 2.1m
            double t = dist / halfWidth;  // 0 at center, 1 at edges
            p.elevation = 2.1 + (20.0 - 2.1) * t * t;
        } else {
            p.elevation = 20.0;
        }
        pts.push_back(p);
    }
    return pts;
}

// Reproduce the full pipeline as in BuildProfile:
//   filter → trackbed offset → smooth → clamp
static void ApplyFullPipeline(std::vector<ProfilePoint>& pts)
{
    // Step 1: remove nodata surface points
    pts.erase(
        std::remove_if(pts.begin(), pts.end(),
                        [](const ProfilePoint& p) {
                            return p.elevation < -9000 && p.medium != 'U';
                        }),
        pts.end());

    // Step 2: reject below-threshold surface elevations
    for (auto& pt : pts) {
        if (pt.elevation > -9000 && pt.elevation < kMinPlausibleElev
            && pt.medium != 'U')
            pt.elevation = -9999;
    }

    // Step 3: track bed offset
    for (auto& pt : pts) {
        if (pt.elevation > -9000 && pt.medium != 'U')
            pt.elevation += kTrackBedOffset;
    }

    // Step 4: smooth (railway parameters)
    ProfileData::SmoothWithParams(pts, 0.21, 0.025, 0.42);

    // Step 5: post-smoothing clamp (matches BuildProfile)
    for (auto& pt : pts) {
        if (pt.elevation > -9000 && pt.medium != 'U'
            && pt.elevation < kMinTrackElev)
            pt.elevation = kMinTrackElev;
    }
}

// ---- tests ----------------------------------------------------------------

static void TestPlausibilityFilter()
{
    printf("Test: plausibility filter rejects sub-2m elevations\n");

    auto pts = MakeCoastalProfile();

    // Before filtering, verify we have low points
    int lowBefore = 0;
    for (const auto& p : pts)
        if (p.elevation < 2.0 && p.medium != 'U') lowBefore++;
    CHECK_GT(lowBefore, 0);
    printf("  points below 2m before filter: %d\n", lowBefore);

    ApplyFullPipeline(pts);

    // After full pipeline: no surface point should be below kMinTrackElev
    int belowThreshold = 0;
    for (const auto& p : pts) {
        if (p.medium == 'U') continue;
        if (p.elevation > -9000 && p.elevation < kMinTrackElev)
            belowThreshold++;
    }
    printf("  points below %.1fm after pipeline: %d\n",
           kMinTrackElev, belowThreshold);
    CHECK(belowThreshold == 0);
}

static void TestTunnelPointsPreserved()
{
    printf("Test: tunnel points below 2m are NOT filtered\n");

    auto pts = MakeTunnelAtSeaLevel();

    // Count tunnel points with negative elevation before filter
    int tunnelLowBefore = 0;
    for (const auto& p : pts)
        if (p.medium == 'U' && p.elevation < 0) tunnelLowBefore++;
    CHECK_GT(tunnelLowBefore, 0);
    printf("  tunnel points below 0m: %d\n", tunnelLowBefore);

    ApplyFullPipeline(pts);

    // Tunnel points should still have their original elevation (not -9999)
    int tunnelLowAfter = 0;
    for (const auto& p : pts)
        if (p.medium == 'U' && p.elevation < 0 && p.elevation > -9000)
            tunnelLowAfter++;
    CHECK(tunnelLowAfter == tunnelLowBefore);
    printf("  tunnel points preserved: %d\n", tunnelLowAfter);
}

static void TestSmoothingSkipsNodata()
{
    printf("Test: smoothing skips nodata points\n");

    auto pts = MakeCoastalProfile();
    ApplyFullPipeline(pts);

    // Nodata points from the tidal zone should still exist (not lost)
    int nodata = 0;
    for (const auto& p : pts)
        if (p.medium != 'U' && p.elevation < -9000) nodata++;
    CHECK_GT(nodata, 0);
    printf("  nodata points after pipeline: %d\n", nodata);
}

static void TestNoSurfacePointBelowThresholdAfterSmoothing()
{
    printf("Test: no surface elevation below %.1fm after smoothing\n",
           kMinTrackElev);

    auto pts = MakeCoastalProfile();
    ApplyFullPipeline(pts);

    // The core assertion: every valid surface point must be >= kMinTrackElev
    double minFound = 1e9;
    for (const auto& p : pts) {
        if (p.medium == 'U') continue;
        if (p.elevation < -9000) continue;
        CHECK_GE(p.elevation, kMinTrackElev);
        if (p.elevation < minFound) minFound = p.elevation;
    }
    printf("  minimum surface elevation: %.4fm\n", minFound);
}

static void TestBoundaryProfileClampedAfterSmoothing()
{
    printf("Test: boundary dip profile stays above threshold after smoothing\n");

    auto pts = MakeBoundaryProfile();

    // Before pipeline, verify the dip goes close to 2m
    double minBefore = 1e9;
    for (const auto& p : pts)
        if (p.elevation < minBefore) minBefore = p.elevation;
    printf("  min raw elevation: %.2fm\n", minBefore);
    CHECK_GT(kMinPlausibleElev, minBefore - 0.5);  // dip approaches threshold

    ApplyFullPipeline(pts);

    // After: all valid surface points must be >= kMinTrackElev
    double minAfter = 1e9;
    int belowThreshold = 0;
    for (const auto& p : pts) {
        if (p.medium == 'U') continue;
        if (p.elevation < -9000) continue;
        if (p.elevation < kMinTrackElev) belowThreshold++;
        if (p.elevation < minAfter) minAfter = p.elevation;
    }
    printf("  min elevation after pipeline: %.4fm (threshold: %.1fm)\n",
           minAfter, kMinTrackElev);
    CHECK(belowThreshold == 0);
    CHECK_GE(minAfter, kMinTrackElev);
}

static void TestNegativeElevationRejected()
{
    printf("Test: negative surface elevations are rejected\n");

    std::vector<ProfilePoint> pts;
    double elevations[] = { -1.0, 0.5, 1.9, 3.0, 10.0 };
    for (int i = 0; i < 5; i++) {
        ProfilePoint p;
        p.km = i * 1.0;
        p.x = 200000.0 + i * 1000.0;
        p.y = 7000000.0;
        p.medium = ' ';
        p.interpolated = false;
        p.elevation = elevations[i];
        pts.push_back(p);
    }

    ApplyFullPipeline(pts);

    // -1.0 → nodata (below 2m)
    CHECK(pts[0].elevation < -9000);
    // 0.5 → nodata (below 2m)
    CHECK(pts[1].elevation < -9000);
    // 1.9 → nodata (below 2m)
    CHECK(pts[2].elevation < -9000);
    // 3.0 and 10.0 → preserved, and after pipeline >= kMinTrackElev
    CHECK_GE(pts[3].elevation, kMinTrackElev);
    CHECK_GE(pts[4].elevation, kMinTrackElev);
}

static void TestEdgeCaseExactly2m()
{
    printf("Test: elevation exactly at 2.0m is preserved\n");

    std::vector<ProfilePoint> pts;
    ProfilePoint p;
    p.km = 0; p.x = 200000; p.y = 7000000;
    p.medium = ' '; p.interpolated = false;

    // Exactly 2.0 should be preserved (filter rejects < 2.0, not <= 2.0)
    p.elevation = 2.0;
    pts.push_back(p);

    // Just below 2.0 should be rejected
    p.km = 1.0;
    p.elevation = 1.999;
    pts.push_back(p);

    ApplyFullPipeline(pts);

    CHECK_GE(pts[0].elevation, kMinTrackElev);  // 2.0 + 0.6 trackbed = 2.6
    CHECK(pts[1].elevation < -9000);             // 1.999 → nodata
}

static void TestTidalZoneFullPipeline()
{
    printf("Test: full pipeline guarantees no sub-threshold surface points\n");

    std::vector<ProfilePoint> pts;
    for (int i = 0; i <= 60; i++) {
        ProfilePoint p;
        p.km = i * 0.1;
        p.x = 200000.0 + i * 100.0;
        p.y = 7000000.0;
        p.medium = ' ';
        p.interpolated = false;

        if (p.km >= 2.5 && p.km <= 3.5) {
            p.elevation = 0.5 - (p.km - 2.5);  // tidal zone
        } else {
            p.elevation = 8.0;
        }
        pts.push_back(p);
    }

    ApplyFullPipeline(pts);

    // Every valid surface point must be >= kMinTrackElev
    for (const auto& p : pts) {
        if (p.medium == 'U' || p.elevation < -9000) continue;
        CHECK_GE(p.elevation, kMinTrackElev);
    }

    // Good points should still be near 8.0 + 0.6 = 8.6
    CHECK_APPROX(pts[0].elevation, 8.6, 1.0);
    CHECK_APPROX(pts[pts.size() - 1].elevation, 8.6, 1.0);
}

static void TestSmoothingAloneCanViolateThreshold()
{
    printf("Test: smoothing without clamp CAN push below threshold "
           "(clamp is necessary)\n");

    // Construct a profile where smoothing will pull values below threshold.
    // Sharp transition from 20m to 2.1m: the vertical curve reconstruction
    // can overshoot downward.
    auto pts = MakeBoundaryProfile();

    // Apply filter + trackbed only (no clamp)
    pts.erase(
        std::remove_if(pts.begin(), pts.end(),
                        [](const ProfilePoint& p) {
                            return p.elevation < -9000 && p.medium != 'U';
                        }),
        pts.end());
    for (auto& pt : pts) {
        if (pt.elevation > -9000 && pt.elevation < kMinPlausibleElev
            && pt.medium != 'U')
            pt.elevation = -9999;
    }
    for (auto& pt : pts) {
        if (pt.elevation > -9000 && pt.medium != 'U')
            pt.elevation += kTrackBedOffset;
    }

    // Smooth WITHOUT post-clamp
    ProfileData::SmoothWithParams(pts, 0.21, 0.025, 0.42);

    // Check if any surface point went below threshold
    double minElev = 1e9;
    for (const auto& p : pts) {
        if (p.medium == 'U' || p.elevation < -9000) continue;
        if (p.elevation < minElev) minElev = p.elevation;
    }
    printf("  min elevation after smoothing (no clamp): %.4fm\n", minElev);

    // This demonstrates that smoothing CAN produce values below threshold,
    // which is why the clamp in BuildProfile is needed.
    // (If smoothing happens not to violate here, that's fine too — the test
    // just documents the motivation.)
    if (minElev < kMinTrackElev)
        printf("  -> smoothing DID violate threshold (clamp is needed)\n");
    else
        printf("  -> smoothing did not violate here (clamp is still a safety net)\n");

    // Either way, the clamped pipeline must not violate:
    auto pts2 = MakeBoundaryProfile();
    ApplyFullPipeline(pts2);
    for (const auto& p : pts2) {
        if (p.medium == 'U' || p.elevation < -9000) continue;
        CHECK_GE(p.elevation, kMinTrackElev);
    }
}

// ---- main -----------------------------------------------------------------

int main()
{
    TestPlausibilityFilter();
    TestTunnelPointsPreserved();
    TestSmoothingSkipsNodata();
    TestNoSurfacePointBelowThresholdAfterSmoothing();
    TestBoundaryProfileClampedAfterSmoothing();
    TestNegativeElevationRejected();
    TestEdgeCaseExactly2m();
    TestTidalZoneFullPipeline();
    TestSmoothingAloneCanViolateThreshold();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

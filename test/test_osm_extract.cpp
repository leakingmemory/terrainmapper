//
// Integration test for the OSM extraction pipeline.
//
// Runs OsmData::Extract on a small PBF extract of central Oslo and
// verifies that the resulting GPKG contains the expected feature types
// and plausible counts.
//
// The test data (oslo_central.osm.pbf) is (c) OpenStreetMap contributors,
// licensed under ODbL v1.0 — see test/data/LICENSE.
//

#include "../src/OsmData.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// ---- minimal test harness -------------------------------------------------

static int g_pass = 0, g_fail = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (expr) { g_pass++; }                                            \
        else { g_fail++; fprintf(stderr, "FAIL  %s:%d: %s\n",             \
                                 __FILE__, __LINE__, #expr); }             \
    } while (0)

#define CHECK_GE(a, b)                                                     \
    do {                                                                   \
        auto _a = (a); auto _b = (b);                                      \
        if (_a >= _b) { g_pass++; }                                        \
        else { g_fail++; fprintf(stderr, "FAIL  %s:%d: %s = %d (expected >= %d)\n", \
                                 __FILE__, __LINE__, #a, (int)_a, (int)_b); }  \
    } while (0)

#define CHECK_GT(a, b)                                                     \
    do {                                                                   \
        auto _a = (a); auto _b = (b);                                      \
        if (_a > _b) { g_pass++; }                                         \
        else { g_fail++; fprintf(stderr, "FAIL  %s:%d: %s = %d (expected > %d)\n", \
                                 __FILE__, __LINE__, #a, (int)_a, (int)_b); }  \
    } while (0)

// ---- helpers --------------------------------------------------------------

static int LayerCount(GDALDataset* ds, const char* name)
{
    OGRLayer* lyr = ds->GetLayerByName(name);
    return lyr ? static_cast<int>(lyr->GetFeatureCount()) : -1;
}

static bool LayerHasField(GDALDataset* ds, const char* layerName,
                           const char* fieldName)
{
    OGRLayer* lyr = ds->GetLayerByName(layerName);
    if (!lyr) return false;
    return lyr->GetLayerDefn()->GetFieldIndex(fieldName) >= 0;
}

// Count features in a layer where a string field matches a value
static int CountWhere(GDALDataset* ds, const char* layerName,
                       const char* field, const char* value)
{
    OGRLayer* lyr = ds->GetLayerByName(layerName);
    if (!lyr) return 0;
    int idx = lyr->GetLayerDefn()->GetFieldIndex(field);
    if (idx < 0) return 0;

    lyr->ResetReading();
    int count = 0;
    OGRFeature* f;
    while ((f = lyr->GetNextFeature()) != nullptr) {
        const char* v = f->GetFieldAsString(idx);
        if (v && std::string(v) == value) count++;
        OGRFeature::DestroyFeature(f);
    }
    return count;
}

// Count features where a string field is non-empty
static int CountNonEmpty(GDALDataset* ds, const char* layerName,
                          const char* field)
{
    OGRLayer* lyr = ds->GetLayerByName(layerName);
    if (!lyr) return 0;
    int idx = lyr->GetLayerDefn()->GetFieldIndex(field);
    if (idx < 0) return 0;

    lyr->ResetReading();
    int count = 0;
    OGRFeature* f;
    while ((f = lyr->GetNextFeature()) != nullptr) {
        const char* v = f->GetFieldAsString(idx);
        if (v && v[0]) count++;
        OGRFeature::DestroyFeature(f);
    }
    return count;
}

// ---- tests ----------------------------------------------------------------

int main(int argc, char* argv[])
{
    GDALAllRegister();

    // Locate test data relative to the binary or via argument
    std::string pbfPath = "test/data/oslo_central.osm.pbf";
    if (argc > 1)
        pbfPath = argv[1];

    std::string gpkgPath = "/tmp/test_osm_extract_output.gpkg";

    // Remove any previous output
    remove(gpkgPath.c_str());

    // --- Run the extraction pipeline ---
    OsmData::BBox bbox;
    bbox.lonMin = 10.72;
    bbox.latMin = 59.90;
    bbox.lonMax = 10.78;
    bbox.latMax = 59.92;

    printf("Running OSM extraction on %s ...\n", pbfPath.c_str());
    int total = OsmData::Extract(pbfPath, bbox, gpkgPath, nullptr);
    printf("Extract returned: %d features\n\n", total);

    CHECK_GT(total, 0);

    // --- Open the GPKG and verify ---
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(gpkgPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    CHECK(ds != nullptr);
    if (!ds) {
        fprintf(stderr, "Cannot open output GPKG — aborting.\n");
        return 1;
    }

    // -- Layer existence --
    printf("Checking layers...\n");
    CHECK(ds->GetLayerByName("buildings") != nullptr);
    CHECK(ds->GetLayerByName("railway_tracks") != nullptr);
    CHECK(ds->GetLayerByName("railway_points") != nullptr);
    CHECK(ds->GetLayerByName("railway_platforms") != nullptr);

    // -- Buildings --
    int buildings = LayerCount(ds, "buildings");
    printf("  buildings:        %d\n", buildings);
    CHECK_GE(buildings, 3000);   // Oslo center has thousands

    CHECK(LayerHasField(ds, "buildings", "building"));
    CHECK(LayerHasField(ds, "buildings", "name"));
    CHECK(LayerHasField(ds, "buildings", "levels"));
    CHECK(LayerHasField(ds, "buildings", "height"));

    // -- Railway tracks --
    int tracks = LayerCount(ds, "railway_tracks");
    printf("  railway_tracks:   %d\n", tracks);
    CHECK_GE(tracks, 400);      // Oslo S area has many tracks

    CHECK(LayerHasField(ds, "railway_tracks", "railway"));
    CHECK(LayerHasField(ds, "railway_tracks", "service"));
    CHECK(LayerHasField(ds, "railway_tracks", "electrified"));
    CHECK(LayerHasField(ds, "railway_tracks", "voltage"));
    CHECK(LayerHasField(ds, "railway_tracks", "frequency"));
    CHECK(LayerHasField(ds, "railway_tracks", "gauge"));
    CHECK(LayerHasField(ds, "railway_tracks", "maxspeed"));
    CHECK(LayerHasField(ds, "railway_tracks", "tunnel"));
    CHECK(LayerHasField(ds, "railway_tracks", "bridge"));

    // Track types present
    int railTracks = CountWhere(ds, "railway_tracks", "railway", "rail");
    int tramTracks = CountWhere(ds, "railway_tracks", "railway", "tram");
    int subwayTracks = CountWhere(ds, "railway_tracks", "railway", "subway");
    printf("    rail: %d, tram: %d, subway: %d\n",
           railTracks, tramTracks, subwayTracks);
    CHECK_GT(railTracks, 0);
    CHECK_GT(tramTracks, 0);     // Oslo has trams

    // Service types
    int sidings = CountWhere(ds, "railway_tracks", "service", "siding");
    int yards = CountWhere(ds, "railway_tracks", "service", "yard");
    printf("    sidings: %d, yards: %d\n", sidings, yards);
    CHECK_GT(sidings, 0);
    CHECK_GT(yards, 0);

    // Electrification data present
    int withElectrified = CountNonEmpty(ds, "railway_tracks", "electrified");
    printf("    with electrified: %d\n", withElectrified);
    CHECK_GT(withElectrified, 0);

    // -- Railway points --
    int points = LayerCount(ds, "railway_points");
    printf("  railway_points:   %d\n", points);
    CHECK_GE(points, 200);

    CHECK(LayerHasField(ds, "railway_points", "railway"));
    CHECK(LayerHasField(ds, "railway_points", "name"));

    // Point types
    int switches = CountWhere(ds, "railway_points", "railway", "switch");
    int signals = CountWhere(ds, "railway_points", "railway", "signal");
    int crossings = CountWhere(ds, "railway_points", "railway", "level_crossing");
    printf("    switches: %d, signals: %d, crossings: %d\n",
           switches, signals, crossings);
    CHECK_GT(switches, 0);
    CHECK_GT(crossings, 0);

    // -- Platforms --
    int platforms = LayerCount(ds, "railway_platforms");
    printf("  railway_platforms: %d\n", platforms);
    CHECK_GE(platforms, 30);     // Oslo S + nearby stations

    CHECK(LayerHasField(ds, "railway_platforms", "name"));
    CHECK(LayerHasField(ds, "railway_platforms", "ref"));

    // -- Total consistency --
    int summed = buildings + tracks + points + platforms;
    printf("\n  total (summed):   %d\n", summed);
    printf("  total (returned): %d\n", total);
    CHECK(summed == total);

    // -- Geometry checks --
    // Verify tracks have actual geometries, not empty
    {
        OGRLayer* lyr = ds->GetLayerByName("railway_tracks");
        lyr->ResetReading();
        OGRFeature* f = lyr->GetNextFeature();
        CHECK(f != nullptr);
        if (f) {
            OGRGeometry* g = f->GetGeometryRef();
            CHECK(g != nullptr);
            CHECK(!g->IsEmpty());
            OGRFeature::DestroyFeature(f);
        }
    }

    // Verify point features have point geometry
    {
        OGRLayer* lyr = ds->GetLayerByName("railway_points");
        lyr->ResetReading();
        OGRFeature* f = lyr->GetNextFeature();
        CHECK(f != nullptr);
        if (f) {
            OGRGeometry* g = f->GetGeometryRef();
            CHECK(g != nullptr);
            CHECK(wkbFlatten(g->getGeometryType()) == wkbPoint);
            OGRFeature::DestroyFeature(f);
        }
    }

    GDALClose(ds);
    remove(gpkgPath.c_str());

    // -- Summary --
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

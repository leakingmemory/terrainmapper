#include "OsmData.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

OsmData::BBox OsmData::ComputeTileBBox(const std::vector<TileBounds>& tileBounds)
{
    BBox bb{};
    if (tileBounds.empty())
        return bb;
    bb.lonMin = tileBounds[0].lonMin;
    bb.latMin = tileBounds[0].latMin;
    bb.lonMax = tileBounds[0].lonMax;
    bb.latMax = tileBounds[0].latMax;
    for (size_t i = 1; i < tileBounds.size(); i++) {
        bb.lonMin = std::min(bb.lonMin, tileBounds[i].lonMin);
        bb.latMin = std::min(bb.latMin, tileBounds[i].latMin);
        bb.lonMax = std::max(bb.lonMax, tileBounds[i].lonMax);
        bb.latMax = std::max(bb.latMax, tileBounds[i].latMax);
    }
    return bb;
}

std::string OsmData::GetOtherTag(const char* otherTags, const char* key)
{
    if (!otherTags || !key)
        return {};

    // Format: "key1"=>"value1","key2"=>"value2"
    std::string needle = std::string("\"") + key + "\"=>\"";
    const char* pos = strstr(otherTags, needle.c_str());
    if (!pos)
        return {};

    pos += needle.size();
    const char* end = strchr(pos, '"');
    if (!end)
        return {};

    return std::string(pos, end);
}

// Run osmium extract to clip a PBF to the bbox. Returns the temp PBF path,
// or the original path if osmium is unavailable.
static std::string ExtractBBox(const std::string& osmPbfPath,
                                const OsmData::BBox& bbox,
                                const std::string& basePath)
{
    std::string tempPbf = basePath + ".tmp.osm.pbf";

    char bboxStr[256];
    snprintf(bboxStr, sizeof(bboxStr), "%.6f,%.6f,%.6f,%.6f",
             bbox.lonMin, bbox.latMin, bbox.lonMax, bbox.latMax);

    std::string cmd = "osmium extract --bbox " + std::string(bboxStr)
        + " -o '" + tempPbf + "' --overwrite '" + osmPbfPath + "' 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return osmPbfPath;

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {}

    int exitCode = pclose(pipe);
    if (exitCode != 0)
        return osmPbfPath;  // fallback to direct read

    return tempPbf;
}

// ---- Output layer creation helpers ----------------------------------------

static OGRLayer* CreateBuildingsLayer(GDALDataset* dst)
{
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* lyr = dst->CreateLayer("buildings", &wgs84,
                                      wkbMultiPolygon, nullptr);
    if (!lyr) return nullptr;

    auto addStr = [&](const char* n, int w) {
        OGRFieldDefn f(n, OFTString); f.SetWidth(w);
        (void)lyr->CreateField(&f);
    };
    auto addInt = [&](const char* n) {
        OGRFieldDefn f(n, OFTInteger); (void)lyr->CreateField(&f);
    };
    auto addReal = [&](const char* n) {
        OGRFieldDefn f(n, OFTReal); (void)lyr->CreateField(&f);
    };

    addStr("osm_id", 20);
    addStr("building", 50);
    addStr("name", 200);
    addInt("levels");
    addReal("height");
    addStr("amenity", 50);
    addStr("shop", 50);
    addStr("tourism", 50);
    addStr("historic", 50);
    addStr("roof_shape", 30);
    addStr("roof_colour", 30);
    addStr("building_colour", 30);
    addStr("building_material", 30);
    return lyr;
}

static OGRLayer* CreateTracksLayer(GDALDataset* dst)
{
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* lyr = dst->CreateLayer("railway_tracks", &wgs84,
                                      wkbLineString, nullptr);
    if (!lyr) return nullptr;

    auto addStr = [&](const char* n, int w) {
        OGRFieldDefn f(n, OFTString); f.SetWidth(w);
        (void)lyr->CreateField(&f);
    };
    auto addInt = [&](const char* n) {
        OGRFieldDefn f(n, OFTInteger); (void)lyr->CreateField(&f);
    };

    addStr("osm_id", 20);
    addStr("railway", 30);
    addStr("name", 200);
    addStr("service", 30);
    addStr("usage", 30);
    addInt("gauge");
    addStr("electrified", 20);
    addInt("voltage");
    addStr("frequency", 10);
    addInt("maxspeed");
    addStr("track_ref", 20);
    addStr("tunnel", 10);
    addStr("bridge", 10);
    addInt("layer");
    addStr("operator", 100);
    return lyr;
}

static OGRLayer* CreatePointsLayer(GDALDataset* dst)
{
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* lyr = dst->CreateLayer("railway_points", &wgs84,
                                      wkbPoint, nullptr);
    if (!lyr) return nullptr;

    auto addStr = [&](const char* n, int w) {
        OGRFieldDefn f(n, OFTString); f.SetWidth(w);
        (void)lyr->CreateField(&f);
    };

    addStr("osm_id", 20);
    addStr("railway", 50);
    addStr("name", 200);
    addStr("ref", 50);
    addStr("operator", 100);
    addStr("uic_ref", 20);
    addStr("railway_position", 20);
    return lyr;
}

static OGRLayer* CreatePlatformsLayer(GDALDataset* dst)
{
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* lyr = dst->CreateLayer("railway_platforms", &wgs84,
                                      wkbGeometryCollection, nullptr);
    if (!lyr) return nullptr;

    auto addStr = [&](const char* n, int w) {
        OGRFieldDefn f(n, OFTString); f.SetWidth(w);
        (void)lyr->CreateField(&f);
    };

    addStr("osm_id", 20);
    addStr("name", 200);
    addStr("ref", 20);
    return lyr;
}

// ---- Per-feature processing (called from interleaved loop) ----------------

static bool ProcessBuilding(OGRFeature* feat, OGRLayer* dstLayer)
{
    const char* building = feat->GetFieldAsString("building");
    if (!building || building[0] == '\0')
        return false;

    OGRFeature* out = OGRFeature::CreateFeature(dstLayer->GetLayerDefn());
    out->SetGeometry(feat->GetGeometryRef());

    const char* osmId = feat->GetFieldAsString("osm_id");
    if (osmId) out->SetField("osm_id", osmId);
    out->SetField("building", building);

    auto copyIfSet = [&](const char* name) {
        const char* v = feat->GetFieldAsString(name);
        if (v && v[0]) out->SetField(name, v);
    };
    copyIfSet("name");
    copyIfSet("amenity");
    copyIfSet("shop");
    copyIfSet("tourism");
    copyIfSet("historic");

    const char* otherTags = feat->GetFieldAsString("other_tags");
    if (otherTags && otherTags[0]) {
        std::string levels = OsmData::GetOtherTag(otherTags, "building:levels");
        if (!levels.empty()) {
            int n = atoi(levels.c_str());
            if (n > 0) out->SetField("levels", n);
        }
        std::string height = OsmData::GetOtherTag(otherTags, "height");
        if (!height.empty()) {
            double h = atof(height.c_str());
            if (h > 0) out->SetField("height", h);
        }
        auto tagToField = [&](const char* tag, const char* field) {
            std::string v = OsmData::GetOtherTag(otherTags, tag);
            if (!v.empty()) out->SetField(field, v.c_str());
        };
        tagToField("roof:shape", "roof_shape");
        tagToField("roof:colour", "roof_colour");
        tagToField("building:colour", "building_colour");
        tagToField("building:material", "building_material");
    }

    (void)dstLayer->CreateFeature(out);
    OGRFeature::DestroyFeature(out);
    return true;
}

static bool ProcessTrack(OGRFeature* feat, OGRLayer* dstLayer)
{
    const char* railway = feat->GetFieldAsString("railway");
    if (!railway || railway[0] == '\0')
        return false;

    std::string rtype = railway;
    if (rtype != "rail" && rtype != "subway" && rtype != "tram" &&
        rtype != "light_rail" && rtype != "narrow_gauge" &&
        rtype != "construction" && rtype != "disused" &&
        rtype != "abandoned" && rtype != "preserved")
        return false;

    OGRFeature* out = OGRFeature::CreateFeature(dstLayer->GetLayerDefn());
    out->SetGeometry(feat->GetGeometryRef());

    const char* osmId = feat->GetFieldAsString("osm_id");
    if (osmId) out->SetField("osm_id", osmId);
    out->SetField("railway", railway);

    const char* name = feat->GetFieldAsString("name");
    if (name && name[0]) out->SetField("name", name);

    const char* otherTags = feat->GetFieldAsString("other_tags");
    if (otherTags && otherTags[0]) {
        auto tagToField = [&](const char* tag, const char* field) {
            std::string v = OsmData::GetOtherTag(otherTags, tag);
            if (!v.empty()) out->SetField(field, v.c_str());
        };
        auto tagToIntField = [&](const char* tag, const char* field) {
            std::string v = OsmData::GetOtherTag(otherTags, tag);
            if (!v.empty()) {
                int n = atoi(v.c_str());
                if (n != 0) out->SetField(field, n);
            }
        };
        tagToField("service", "service");
        tagToField("usage", "usage");
        tagToIntField("gauge", "gauge");
        tagToField("electrified", "electrified");
        tagToIntField("voltage", "voltage");
        tagToField("frequency", "frequency");
        tagToIntField("maxspeed", "maxspeed");
        tagToField("railway:track_ref", "track_ref");
        tagToField("tunnel", "tunnel");
        tagToField("bridge", "bridge");
        tagToIntField("layer", "layer");
        tagToField("operator", "operator");
    }

    (void)dstLayer->CreateFeature(out);
    OGRFeature::DestroyFeature(out);
    return true;
}

static bool ProcessLinePlatform(OGRFeature* feat, OGRLayer* dstLayer)
{
    const char* railway = feat->GetFieldAsString("railway");
    if (!railway)
        return false;

    std::string rt = railway;
    if (rt != "platform" && rt != "platform_edge")
        return false;

    OGRFeature* out = OGRFeature::CreateFeature(dstLayer->GetLayerDefn());
    out->SetGeometry(feat->GetGeometryRef());

    const char* osmId = feat->GetFieldAsString("osm_id");
    if (osmId) out->SetField("osm_id", osmId);

    const char* name = feat->GetFieldAsString("name");
    if (name && name[0]) out->SetField("name", name);

    const char* otherTags = feat->GetFieldAsString("other_tags");
    if (otherTags) {
        std::string ref = OsmData::GetOtherTag(otherTags, "ref");
        if (!ref.empty()) out->SetField("ref", ref.c_str());
    }

    (void)dstLayer->CreateFeature(out);
    OGRFeature::DestroyFeature(out);
    return true;
}

static bool ProcessPolyPlatform(OGRFeature* feat, OGRLayer* dstLayer)
{
    const char* otherTags = feat->GetFieldAsString("other_tags");
    if (!otherTags)
        return false;

    std::string rw = OsmData::GetOtherTag(otherTags, "railway");
    if (rw != "platform" && rw != "platform_edge")
        return false;

    OGRFeature* out = OGRFeature::CreateFeature(dstLayer->GetLayerDefn());
    out->SetGeometry(feat->GetGeometryRef());

    const char* osmId = feat->GetFieldAsString("osm_id");
    if (osmId) out->SetField("osm_id", osmId);

    const char* name = feat->GetFieldAsString("name");
    if (name && name[0]) out->SetField("name", name);

    std::string ref = OsmData::GetOtherTag(otherTags, "ref");
    if (!ref.empty()) out->SetField("ref", ref.c_str());

    (void)dstLayer->CreateFeature(out);
    OGRFeature::DestroyFeature(out);
    return true;
}

static bool ProcessRailwayPoint(OGRFeature* feat, OGRLayer* dstLayer)
{
    // The 'railway' tag is NOT a direct field on the OSM points layer —
    // it lives in other_tags. Check there.
    const char* otherTags = feat->GetFieldAsString("other_tags");
    if (!otherTags || !otherTags[0])
        return false;

    std::string railway = OsmData::GetOtherTag(otherTags, "railway");
    if (railway.empty())
        return false;

    OGRFeature* out = OGRFeature::CreateFeature(dstLayer->GetLayerDefn());
    out->SetGeometry(feat->GetGeometryRef());

    const char* osmId = feat->GetFieldAsString("osm_id");
    if (osmId) out->SetField("osm_id", osmId);
    out->SetField("railway", railway.c_str());

    const char* name = feat->GetFieldAsString("name");
    if (name && name[0]) out->SetField("name", name);

    auto tagToField = [&](const char* tag, const char* field) {
        std::string v = OsmData::GetOtherTag(otherTags, tag);
        if (!v.empty()) out->SetField(field, v.c_str());
    };
    tagToField("ref", "ref");
    tagToField("operator", "operator");
    tagToField("uic_ref", "uic_ref");
    tagToField("railway:position", "railway_position");

    (void)dstLayer->CreateFeature(out);
    OGRFeature::DestroyFeature(out);
    return true;
}

// ---- Main extraction (single interleaved pass) ----------------------------

int OsmData::Extract(const std::string& osmPbfPath,
                      const BBox& bbox,
                      const std::string& outputGpkg,
                      ProgressCb progress)
{
    if (progress && !progress(0, "Extracting area from OSM file..."))
        return -1;

    // Step 1: Use osmium to clip the PBF to our bbox
    std::string dataPbf = ExtractBBox(osmPbfPath, bbox, outputGpkg);

    if (progress && !progress(30, "Reading OSM data..."))
        return -1;

    // Step 2: Open with GDAL using interleaved reading.
    // The OSM driver REQUIRES interleaved mode for large files — with
    // non-interleaved mode, it buffers all features from earlier layers
    // in memory and overflows. With interleaved mode, we must use
    // dataset->GetNextFeature() instead of layer->GetNextFeature().
    CPLSetConfigOption("OGR_INTERLEAVED_READING", "YES");
    CPLSetConfigOption("OSM_COMPRESS_NODES", "YES");
    CPLSetConfigOption("OSM_MAX_TMPFILE_SIZE", "8192");

    GDALDataset* src = static_cast<GDALDataset*>(
        GDALOpenEx(dataPbf.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!src) {
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        if (progress) progress(0, "Failed to open OSM data");
        return -1;
    }

    // Step 3: Create output GPKG
    GDALDriver* gpkgDrv = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (!gpkgDrv) {
        GDALClose(src);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        return -1;
    }

    GDALDataset* dst = gpkgDrv->Create(outputGpkg.c_str(), 0, 0, 0,
                                        GDT_Unknown, nullptr);
    if (!dst) {
        GDALClose(src);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        return -1;
    }

    // Step 4: Create all output layers up front
    OGRLayer* dstBuildings = CreateBuildingsLayer(dst);
    OGRLayer* dstTracks    = CreateTracksLayer(dst);
    OGRLayer* dstPoints    = CreatePointsLayer(dst);
    OGRLayer* dstPlatforms = CreatePlatformsLayer(dst);

    // Set spatial filters on source layers to limit to our bbox
    const char* layerNames[] = { "points", "lines", "multilinestrings",
                                  "multipolygons", "other_relations" };
    for (const char* name : layerNames) {
        OGRLayer* lyr = src->GetLayerByName(name);
        if (lyr)
            lyr->SetSpatialFilterRect(bbox.lonMin, bbox.latMin,
                                       bbox.lonMax, bbox.latMax);
    }

    // Identify source layer pointers for dispatching
    OGRLayer* srcPoints = src->GetLayerByName("points");
    OGRLayer* srcLines  = src->GetLayerByName("lines");
    OGRLayer* srcMultipolygons = src->GetLayerByName("multipolygons");

    // GPKG driver manages transactions automatically via AutoTransaction

    // Step 5: Single interleaved pass through all features
    int buildingCount = 0, trackCount = 0, pointCount = 0, platformCount = 0;
    int totalProcessed = 0;
    bool cancelled = false;

    OGRLayer* belongingLayer = nullptr;
    OGRFeature* feat;

    while ((feat = src->GetNextFeature(&belongingLayer, nullptr,
                                        nullptr, nullptr)) != nullptr) {
        if (belongingLayer == srcMultipolygons) {
            // Could be a building or a polygon platform
            if (dstBuildings && ProcessBuilding(feat, dstBuildings))
                buildingCount++;
            else if (dstPlatforms && ProcessPolyPlatform(feat, dstPlatforms))
                platformCount++;
        }
        else if (belongingLayer == srcLines) {
            // Could be a railway track or a line platform
            if (dstTracks && ProcessTrack(feat, dstTracks))
                trackCount++;
            else if (dstPlatforms && ProcessLinePlatform(feat, dstPlatforms))
                platformCount++;
        }
        else if (belongingLayer == srcPoints) {
            if (dstPoints && ProcessRailwayPoint(feat, dstPoints))
                pointCount++;
        }

        OGRFeature::DestroyFeature(feat);
        totalProcessed++;

        if ((totalProcessed % 100000) == 0 && progress) {
            int total = buildingCount + trackCount + pointCount + platformCount;
            std::string msg = "Processing... "
                + std::to_string(buildingCount) + " buildings, "
                + std::to_string(trackCount) + " tracks, "
                + std::to_string(pointCount) + " rail points, "
                + std::to_string(platformCount) + " platforms";
            if (!progress(40 + std::min(totalProcessed / 100000, 50), msg)) {
                cancelled = true;
                break;
            }
        }
    }

    if (progress)
        progress(95, "Finalizing...");

    GDALClose(dst);
    GDALClose(src);

    if (dataPbf != osmPbfPath)
        remove(dataPbf.c_str());

    if (cancelled) {
        remove(outputGpkg.c_str());
        return -1;
    }

    int total = buildingCount + trackCount + pointCount + platformCount;

    if (progress)
        progress(100, "Done");

    return total;
}

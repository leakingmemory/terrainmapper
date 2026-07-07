#include "OsmData.h"
#include "MainFrame.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

static int ExtractBuildings(GDALDataset* src, GDALDataset* dst,
                             const OsmData::BBox& bbox,
                             OsmData::ProgressCb progress)
{
    OGRLayer* polyLayer = src->GetLayerByName("multipolygons");
    if (!polyLayer)
        return 0;

    polyLayer->SetSpatialFilterRect(bbox.lonMin, bbox.latMin,
                                    bbox.lonMax, bbox.latMax);
    polyLayer->SetAttributeFilter("building IS NOT NULL");

    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* dstLayer = dst->CreateLayer("buildings", &wgs84,
                                           wkbMultiPolygon, nullptr);
    if (!dstLayer)
        return 0;

    // Define fields
    auto addStr = [&](const char* name, int w) {
        OGRFieldDefn f(name, OFTString); f.SetWidth(w);
        (void)dstLayer->CreateField(&f);
    };
    auto addInt = [&](const char* name) {
        OGRFieldDefn f(name, OFTInteger);
        (void)dstLayer->CreateField(&f);
    };
    auto addReal = [&](const char* name) {
        OGRFieldDefn f(name, OFTReal);
        (void)dstLayer->CreateField(&f);
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

    (void)dstLayer->StartTransaction();
    polyLayer->ResetReading();

    int count = 0;
    OGRFeature* feat;
    while ((feat = polyLayer->GetNextFeature()) != nullptr) {
        const char* building = feat->GetFieldAsString("building");
        if (!building || building[0] == '\0') {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

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
        OGRFeature::DestroyFeature(feat);
        count++;

        if ((count % 10000) == 0 && progress) {
            if (!progress(40, "Buildings: " + std::to_string(count) + "..."))
            {
                (void)dstLayer->CommitTransaction();
                return -1;
            }
        }
    }

    (void)dstLayer->CommitTransaction();
    return count;
}

static int ExtractRailwayTracks(GDALDataset* src, GDALDataset* dst,
                                 const OsmData::BBox& bbox)
{
    OGRLayer* lineLayer = src->GetLayerByName("lines");
    if (!lineLayer)
        return 0;

    lineLayer->SetSpatialFilterRect(bbox.lonMin, bbox.latMin,
                                    bbox.lonMax, bbox.latMax);
    lineLayer->SetAttributeFilter("railway IS NOT NULL");

    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* dstLayer = dst->CreateLayer("railway_tracks", &wgs84,
                                           wkbLineString, nullptr);
    if (!dstLayer)
        return 0;

    auto addStr = [&](const char* name, int w) {
        OGRFieldDefn f(name, OFTString); f.SetWidth(w);
        (void)dstLayer->CreateField(&f);
    };
    auto addInt = [&](const char* name) {
        OGRFieldDefn f(name, OFTInteger);
        (void)dstLayer->CreateField(&f);
    };

    addStr("osm_id", 20);
    addStr("railway", 30);     // rail, subway, tram, narrow_gauge, etc.
    addStr("name", 200);
    addStr("service", 30);     // yard, siding, crossover, spur
    addStr("usage", 30);       // main, branch, industrial, military
    addInt("gauge");
    addStr("electrified", 20); // contact_line, rail, yes, no
    addInt("voltage");
    addStr("frequency", 10);    // e.g. 16.7 (Hz) for AC systems
    addInt("maxspeed");
    addStr("track_ref", 20);   // track number at stations
    addStr("tunnel", 10);
    addStr("bridge", 10);
    addInt("layer");
    addStr("operator", 100);

    (void)dstLayer->StartTransaction();
    lineLayer->ResetReading();

    int count = 0;
    OGRFeature* feat;
    while ((feat = lineLayer->GetNextFeature()) != nullptr) {
        const char* railway = feat->GetFieldAsString("railway");
        if (!railway || railway[0] == '\0') {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

        // Skip non-track types that end up in lines layer
        std::string rtype = railway;
        if (rtype != "rail" && rtype != "subway" && rtype != "tram" &&
            rtype != "light_rail" && rtype != "narrow_gauge" &&
            rtype != "construction" && rtype != "disused" &&
            rtype != "abandoned" && rtype != "preserved") {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

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
        OGRFeature::DestroyFeature(feat);
        count++;
    }

    (void)dstLayer->CommitTransaction();
    return count;
}

// Helper: ensure the railway_platforms layer exists in dst, creating it if needed
static OGRLayer* EnsurePlatformLayer(GDALDataset* dst)
{
    OGRLayer* existing = dst->GetLayerByName("railway_platforms");
    if (existing)
        return existing;

    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* dstLayer = dst->CreateLayer("railway_platforms", &wgs84,
                                           wkbGeometryCollection, nullptr);
    if (!dstLayer)
        return nullptr;

    auto addStr = [&](const char* name, int w) {
        OGRFieldDefn f(name, OFTString); f.SetWidth(w);
        (void)dstLayer->CreateField(&f);
    };

    addStr("osm_id", 20);
    addStr("name", 200);
    addStr("ref", 20);
    return dstLayer;
}

// Extract polygon platforms from multipolygons layer (separate dataset open)
static int ExtractRailwayPlatformPolygons(GDALDataset* src, GDALDataset* dst,
                                           const OsmData::BBox& bbox)
{
    OGRLayer* polyLayer = src->GetLayerByName("multipolygons");
    if (!polyLayer)
        return 0;

    OGRLayer* dstLayer = EnsurePlatformLayer(dst);
    if (!dstLayer)
        return 0;

    polyLayer->SetSpatialFilterRect(bbox.lonMin, bbox.latMin,
                                    bbox.lonMax, bbox.latMax);
    polyLayer->SetAttributeFilter(
        "other_tags LIKE '%\"railway\"=>\"platform\"%' OR "
        "other_tags LIKE '%\"railway\"=>\"platform_edge\"%'");
    polyLayer->ResetReading();

    (void)dstLayer->StartTransaction();
    int count = 0;

    OGRFeature* feat;
    while ((feat = polyLayer->GetNextFeature()) != nullptr) {
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
        OGRFeature::DestroyFeature(feat);
        count++;
    }

    (void)dstLayer->CommitTransaction();
    return count;
}

// Extract line platforms from lines layer (separate dataset open)
static int ExtractRailwayPlatformLines(GDALDataset* src, GDALDataset* dst,
                                        const OsmData::BBox& bbox)
{
    OGRLayer* lineLayer = src->GetLayerByName("lines");
    if (!lineLayer)
        return 0;

    OGRLayer* dstLayer = EnsurePlatformLayer(dst);
    if (!dstLayer)
        return 0;

    lineLayer->SetSpatialFilterRect(bbox.lonMin, bbox.latMin,
                                    bbox.lonMax, bbox.latMax);
    lineLayer->SetAttributeFilter(
        "railway = 'platform' OR railway = 'platform_edge'");
    lineLayer->ResetReading();

    (void)dstLayer->StartTransaction();
    int count = 0;

    OGRFeature* feat;
    while ((feat = lineLayer->GetNextFeature()) != nullptr) {
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
        OGRFeature::DestroyFeature(feat);
        count++;
    }

    (void)dstLayer->CommitTransaction();
    return count;
}

static int ExtractRailwayPoints(GDALDataset* src, GDALDataset* dst,
                                 const OsmData::BBox& bbox)
{
    OGRLayer* pointLayer = src->GetLayerByName("points");
    if (!pointLayer)
        return 0;

    pointLayer->SetSpatialFilterRect(bbox.lonMin, bbox.latMin,
                                     bbox.lonMax, bbox.latMax);
    pointLayer->SetAttributeFilter("railway IS NOT NULL");

    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* dstLayer = dst->CreateLayer("railway_points", &wgs84,
                                           wkbPoint, nullptr);
    if (!dstLayer)
        return 0;

    auto addStr = [&](const char* name, int w) {
        OGRFieldDefn f(name, OFTString); f.SetWidth(w);
        (void)dstLayer->CreateField(&f);
    };

    addStr("osm_id", 20);
    addStr("railway", 30);  // switch, signal, stop, halt, station, crossing...
    addStr("name", 200);
    addStr("ref", 50);
    addStr("operator", 100);
    addStr("uic_ref", 20);
    addStr("railway_position", 20);  // kilometre marker

    (void)dstLayer->StartTransaction();
    pointLayer->ResetReading();

    int count = 0;
    OGRFeature* feat;
    while ((feat = pointLayer->GetNextFeature()) != nullptr) {
        const char* railway = feat->GetFieldAsString("railway");
        if (!railway || railway[0] == '\0') {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

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
            tagToField("ref", "ref");
            tagToField("operator", "operator");
            tagToField("uic_ref", "uic_ref");
            tagToField("railway:position", "railway_position");
        }

        (void)dstLayer->CreateFeature(out);
        OGRFeature::DestroyFeature(out);
        OGRFeature::DestroyFeature(feat);
        count++;
    }

    (void)dstLayer->CommitTransaction();
    return count;
}

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

    // Step 2: Open with GDAL
    CPLSetConfigOption("OGR_INTERLEAVED_READING", "NO");
    CPLSetConfigOption("OSM_MAX_TMPFILE_SIZE", "1024");

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

    // Step 4: Extract each data type.
    // Note: GDAL's OSM driver is single-pass — each layer can only be
    // read once per dataset open. We reopen the dataset for each pass.

    if (progress && !progress(35, "Extracting buildings..."))
    {
        GDALClose(dst);
        GDALClose(src);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        return -1;
    }

    int buildingCount = ExtractBuildings(src, dst, bbox, progress);
    if (buildingCount < 0) {
        GDALClose(dst);
        GDALClose(src);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        remove(outputGpkg.c_str());
        return -1;
    }

    // Reopen for railway tracks (new pass through the PBF)
    GDALClose(src);

    if (progress && !progress(55, "Extracting railway tracks..."))
    {
        GDALClose(dst);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        return -1;
    }

    src = static_cast<GDALDataset*>(
        GDALOpenEx(dataPbf.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    int trackCount = 0;
    if (src) {
        trackCount = ExtractRailwayTracks(src, dst, bbox);
        GDALClose(src);
    }

    // Reopen for railway points (switches, stations, signals)
    if (progress && !progress(70, "Extracting railway points..."))
    {
        GDALClose(dst);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        return -1;
    }

    src = static_cast<GDALDataset*>(
        GDALOpenEx(dataPbf.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    int pointCount = 0;
    if (src) {
        pointCount = ExtractRailwayPoints(src, dst, bbox);
        GDALClose(src);
    }

    // Reopen for platform lines
    if (progress && !progress(80, "Extracting platform lines..."))
    {
        GDALClose(dst);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        return -1;
    }

    int platformCount = 0;
    src = static_cast<GDALDataset*>(
        GDALOpenEx(dataPbf.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (src) {
        platformCount += ExtractRailwayPlatformLines(src, dst, bbox);
        GDALClose(src);
    }

    // Reopen for platform polygons (multipolygons layer)
    if (progress && !progress(88, "Extracting platform polygons..."))
    {
        GDALClose(dst);
        if (dataPbf != osmPbfPath)
            remove(dataPbf.c_str());
        return -1;
    }

    src = static_cast<GDALDataset*>(
        GDALOpenEx(dataPbf.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (src) {
        platformCount += ExtractRailwayPlatformPolygons(src, dst, bbox);
        GDALClose(src);
    }

    if (progress)
        progress(95, "Finalizing...");

    GDALClose(dst);

    if (dataPbf != osmPbfPath)
        remove(dataPbf.c_str());

    int total = buildingCount + trackCount + pointCount + platformCount;

    if (progress)
        progress(100, "Done");

    return total;
}

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

int OsmData::ExtractBuildings(const std::string& osmPbfPath,
                               const BBox& bbox,
                               const std::string& outputGpkg,
                               ProgressCb progress)
{
    if (progress && !progress(0, "Extracting area from OSM file..."))
        return -1;

    // Step 1: Use osmium to extract the bounding box area.
    // This is much faster than having GDAL scan the entire PBF.
    std::string tempPbf = outputGpkg + ".tmp.osm.pbf";

    char bboxStr[256];
    snprintf(bboxStr, sizeof(bboxStr), "%.6f,%.6f,%.6f,%.6f",
             bbox.lonMin, bbox.latMin, bbox.lonMax, bbox.latMax);

    // Check if osmium is available
    std::string osmiumCmd = "osmium extract --bbox " + std::string(bboxStr)
        + " -o '" + tempPbf + "' --overwrite '" + osmPbfPath + "' 2>&1";

    FILE* pipe = popen(osmiumCmd.c_str(), "r");
    if (!pipe) {
        if (progress) progress(0, "Failed to run osmium extract");
        return -1;
    }

    // Read osmium output (for error messages)
    char buf[4096];
    std::string osmiumOutput;
    while (fgets(buf, sizeof(buf), pipe))
        osmiumOutput += buf;

    int exitCode = pclose(pipe);
    if (exitCode != 0) {
        // osmium not available or failed — try GDAL directly on the PBF
        // This works but may be slow for large files
        tempPbf = osmPbfPath;
    }

    if (progress && !progress(30, "Reading buildings from OSM data..."))
        return -1;

    // Step 2: Open with GDAL and extract buildings.
    // Configure OSM driver to only load building-related tags.
    CPLSetConfigOption("OGR_INTERLEAVED_READING", "YES");
    CPLSetConfigOption("OSM_MAX_TMPFILE_SIZE", "1024");

    GDALDataset* src = static_cast<GDALDataset*>(
        GDALOpenEx(tempPbf.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!src) {
        if (tempPbf != osmPbfPath)
            remove(tempPbf.c_str());
        if (progress) progress(0, "Failed to open OSM data");
        return -1;
    }

    // We need two layers: multipolygons (closed ways + relations) for building
    // outlines, and lines for ways that GDAL couldn't close.
    OGRLayer* polyLayer = src->GetLayerByName("multipolygons");
    if (!polyLayer) {
        GDALClose(src);
        if (tempPbf != osmPbfPath)
            remove(tempPbf.c_str());
        return -1;
    }

    // Apply spatial filter to the bbox
    polyLayer->SetSpatialFilterRect(bbox.lonMin, bbox.latMin,
                                    bbox.lonMax, bbox.latMax);
    polyLayer->SetAttributeFilter("building IS NOT NULL");

    // Step 3: Create output GPKG
    GDALDriver* gpkgDrv = GetGDALDriverManager()->GetDriverByName("GPKG");
    if (!gpkgDrv) {
        GDALClose(src);
        if (tempPbf != osmPbfPath)
            remove(tempPbf.c_str());
        return -1;
    }

    GDALDataset* dst = gpkgDrv->Create(outputGpkg.c_str(), 0, 0, 0,
                                        GDT_Unknown, nullptr);
    if (!dst) {
        GDALClose(src);
        if (tempPbf != osmPbfPath)
            remove(tempPbf.c_str());
        return -1;
    }

    // Create buildings layer in EPSG:4326 (same as OSM source)
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* dstLayer = dst->CreateLayer("buildings", &wgs84,
                                           wkbMultiPolygon, nullptr);
    if (!dstLayer) {
        GDALClose(dst);
        GDALClose(src);
        if (tempPbf != osmPbfPath)
            remove(tempPbf.c_str());
        return -1;
    }

    // Define output fields
    {
        OGRFieldDefn f("osm_id", OFTString); f.SetWidth(20);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("building", OFTString); f.SetWidth(50);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("name", OFTString); f.SetWidth(200);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("levels", OFTInteger);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("height", OFTReal);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("amenity", OFTString); f.SetWidth(50);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("shop", OFTString); f.SetWidth(50);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("tourism", OFTString); f.SetWidth(50);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("historic", OFTString); f.SetWidth(50);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("roof_shape", OFTString); f.SetWidth(30);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("roof_colour", OFTString); f.SetWidth(30);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("building_colour", OFTString); f.SetWidth(30);
        (void)dstLayer->CreateField(&f);
    }
    {
        OGRFieldDefn f("building_material", OFTString); f.SetWidth(30);
        (void)dstLayer->CreateField(&f);
    }

    if (progress && !progress(35, "Converting buildings..."))
    {
        GDALClose(dst);
        GDALClose(src);
        if (tempPbf != osmPbfPath)
            remove(tempPbf.c_str());
        return -1;
    }

    // Read and convert features
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

        OGRFeature* outFeat = OGRFeature::CreateFeature(dstLayer->GetLayerDefn());
        outFeat->SetGeometry(feat->GetGeometryRef());

        // Core fields from named columns
        const char* osmId = feat->GetFieldAsString("osm_id");
        if (osmId) outFeat->SetField("osm_id", osmId);
        outFeat->SetField("building", building);

        const char* name = feat->GetFieldAsString("name");
        if (name && name[0]) outFeat->SetField("name", name);

        const char* amenity = feat->GetFieldAsString("amenity");
        if (amenity && amenity[0]) outFeat->SetField("amenity", amenity);

        const char* shop = feat->GetFieldAsString("shop");
        if (shop && shop[0]) outFeat->SetField("shop", shop);

        const char* tourism = feat->GetFieldAsString("tourism");
        if (tourism && tourism[0]) outFeat->SetField("tourism", tourism);

        const char* historic = feat->GetFieldAsString("historic");
        if (historic && historic[0]) outFeat->SetField("historic", historic);

        // Fields from other_tags
        const char* otherTags = feat->GetFieldAsString("other_tags");
        if (otherTags && otherTags[0]) {
            std::string levels = GetOtherTag(otherTags, "building:levels");
            if (!levels.empty()) {
                int n = atoi(levels.c_str());
                if (n > 0) outFeat->SetField("levels", n);
            }

            std::string height = GetOtherTag(otherTags, "height");
            if (!height.empty()) {
                double h = atof(height.c_str());
                if (h > 0) outFeat->SetField("height", h);
            }

            std::string roofShape = GetOtherTag(otherTags, "roof:shape");
            if (!roofShape.empty())
                outFeat->SetField("roof_shape", roofShape.c_str());

            std::string roofColour = GetOtherTag(otherTags, "roof:colour");
            if (!roofColour.empty())
                outFeat->SetField("roof_colour", roofColour.c_str());

            std::string bldgColour = GetOtherTag(otherTags, "building:colour");
            if (!bldgColour.empty())
                outFeat->SetField("building_colour", bldgColour.c_str());

            std::string bldgMaterial = GetOtherTag(otherTags, "building:material");
            if (!bldgMaterial.empty())
                outFeat->SetField("building_material", bldgMaterial.c_str());
        }

        (void)dstLayer->CreateFeature(outFeat);
        OGRFeature::DestroyFeature(outFeat);
        OGRFeature::DestroyFeature(feat);
        count++;

        if ((count % 10000) == 0 && progress) {
            // We don't know the total, so just show count
            if (!progress(50, "Converting buildings... " + std::to_string(count)))
            {
                (void)dstLayer->CommitTransaction();
                GDALClose(dst);
                GDALClose(src);
                if (tempPbf != osmPbfPath)
                    remove(tempPbf.c_str());
                remove(outputGpkg.c_str());
                return -1;
            }
        }
    }

    (void)dstLayer->CommitTransaction();

    if (progress)
        progress(90, "Creating spatial index...");

    // GPKG spatial index is created automatically by GDAL

    if (progress)
        progress(95, "Finalizing...");

    GDALClose(dst);
    GDALClose(src);

    // Clean up temp PBF if we created one
    if (tempPbf != osmPbfPath)
        remove(tempPbf.c_str());

    if (progress)
        progress(100, "Done");

    return count;
}

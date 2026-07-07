#include "MapView.h"

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogrsf_frmts.h>

#include <wx/dcbuffer.h>
#include <wx/progdlg.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

MapView::MapView(wxWindow* parent, const wxString& title,
                 const std::string& vsiPath,
                 const std::string& ar50Path,
                 const std::string& railwayPath,
                 const std::string& roadsPath,
                 const std::string& osmBuildingsPath)
    : wxFrame(parent, wxID_ANY, title, wxDefaultPosition, wxSize(900, 700))
{
    m_canvas = new wxScrolledCanvas(this, wxID_ANY);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_canvas->Bind(wxEVT_PAINT,  &MapView::OnPaint,     this);
    m_canvas->Bind(wxEVT_MOTION, &MapView::OnMouseMove,  this);

    CreateStatusBar(4);

    wxProgressDialog progress(
        "Loading Tile", "Reading elevation data...",
        100, this,
        wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_SMOOTH |
        wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME);

    if (LoadTile(vsiPath)) {
        progress.Update(10, "Loading land cover...");
        if (!ar50Path.empty())
            LoadLandCover(ar50Path, &progress);
        progress.Update(85, "Loading road data...");
        if (!roadsPath.empty())
            LoadRoads(roadsPath);
        progress.Update(88, "Loading railway data...");
        if (!railwayPath.empty())
            LoadRailway(railwayPath);
        progress.Update(90, "Loading buildings...");
        if (!osmBuildingsPath.empty())
            LoadBuildings(osmBuildingsPath);
        progress.Update(93, "Rendering...");
        m_bitmap = wxBitmap(RenderElevation());
        m_canvas->SetScrollbars(1, 1, m_rasterW, m_rasterH, 0, 0);
        progress.Update(100);
        SetStatusText("Ready", 0);
    } else {
        SetStatusText("Failed to load tile", 0);
    }
}

MapView::~MapView()
{
    if (m_toWGS84)
        OCTDestroyCoordinateTransformation(m_toWGS84);
}

bool MapView::LoadTile(const std::string& vsiPath)
{
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpen(vsiPath.c_str(), GA_ReadOnly));
    if (!ds)
        return false;

    m_rasterW = ds->GetRasterXSize();
    m_rasterH = ds->GetRasterYSize();

    if (ds->GetGeoTransform(m_gt) != CE_None) {
        GDALClose(ds);
        return false;
    }

    m_tileProjectionWkt = ds->GetProjectionRef();

    GDALRasterBand* band = ds->GetRasterBand(1);

    int hasND = 0;
    m_nodata = band->GetNoDataValue(&hasND);
    m_hasNodata = (hasND != 0);

    // Read the full band
    m_elevation.resize(static_cast<size_t>(m_rasterW) * m_rasterH);
    if (band->RasterIO(GF_Read, 0, 0, m_rasterW, m_rasterH,
                        m_elevation.data(), m_rasterW, m_rasterH,
                        GDT_Float32, 0, 0) != CE_None) {
        GDALClose(ds);
        return false;
    }

    // Set up coordinate transform
    OGRSpatialReference src, dst;
    src.importFromWkt(m_tileProjectionWkt.c_str());
    src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dst.SetWellKnownGeogCS("WGS84");
    dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_toWGS84 = OGRCreateCoordinateTransformation(&src, &dst);

    GDALClose(ds);

    // Compute inverse geotransform for coordinate → pixel conversion
    if (!GDALInvGeoTransform(m_gt, m_invGt))
        return false;

    // Compute min/max elevation (excluding nodata)
    m_minElev = FLT_MAX;
    m_maxElev = -FLT_MAX;
    for (float v : m_elevation) {
        if (m_hasNodata && v == static_cast<float>(m_nodata))
            continue;
        m_minElev = std::min(m_minElev, v);
        m_maxElev = std::max(m_maxElev, v);
    }
    if (m_minElev > m_maxElev) {
        m_minElev = 0;
        m_maxElev = 1;
    }

    return true;
}

// GDAL progress callback that forwards to a wxProgressDialog.
// Maps dfComplete (0..1) to the 10%..90% range of the dialog.
static int GDALProgressToWx(double dfComplete, const char*, void* pData)
{
    auto* dlg = static_cast<wxProgressDialog*>(pData);
    if (!dlg)
        return TRUE;
    int pct = 10 + static_cast<int>(dfComplete * 80.0);
    dlg->Update(pct, wxString::Format("Rasterizing land cover... %d%%",
                                      static_cast<int>(dfComplete * 100)));
    return TRUE;
}

bool MapView::LoadLandCover(const std::string& ar50Path, wxProgressDialog* progress)
{
    // Open AR50 as vector
    GDALDataset* ar50 = static_cast<GDALDataset*>(
        GDALOpenEx(ar50Path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ar50)
        return false;

    OGRLayer* layer = ar50->GetLayerByName("ar50");
    if (!layer) {
        GDALClose(ar50);
        return false;
    }

    // Compute tile extent in WGS84 for spatial filter
    double ulx = m_gt[0], uly = m_gt[3];
    double lrx = m_gt[0] + m_rasterW * m_gt[1];
    double lry = m_gt[3] + m_rasterH * m_gt[5];

    double lonUL = ulx, latUL = uly;
    double lonLR = lrx, latLR = lry;
    if (m_toWGS84) {
        m_toWGS84->Transform(1, &lonUL, &latUL);
        m_toWGS84->Transform(1, &lonLR, &latLR);
    }
    layer->SetSpatialFilterRect(
        std::min(lonUL, lonLR), std::min(latUL, latLR),
        std::max(lonUL, lonLR), std::max(latUL, latLR));

    // Create in-memory raster matching the tile
    GDALDriver* memDrv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset* memDs = memDrv->Create("", m_rasterW, m_rasterH,
                                         1, GDT_Int32, nullptr);
    memDs->SetGeoTransform(m_gt);
    memDs->SetProjection(m_tileProjectionWkt.c_str());
    memDs->GetRasterBand(1)->Fill(0);

    // Create transformer: raster pixel/line (UTM) <-> AR50 coords (EPSG:4258)
    const char* transOpts[] = {"DST_SRS=EPSG:4258", nullptr};
    void* hTransform = GDALCreateGenImgProjTransformer2(
        static_cast<GDALDatasetH>(memDs), nullptr, transOpts);

    if (hTransform) {
        int bands[] = {1};
        OGRLayerH layers[] = {static_cast<OGRLayerH>(layer)};
        char attrOpt[] = "ATTRIBUTE=artype";
        char* rasterizeOpts[] = {attrOpt, nullptr};

        GDALRasterizeLayers(
            static_cast<GDALDatasetH>(memDs),
            1, bands,
            1, layers,
            GDALGenImgProjTransform, hTransform,
            nullptr, rasterizeOpts,
            progress ? GDALProgressToWx : nullptr,
            progress);

        GDALDestroyGenImgProjTransformer(hTransform);

        m_landcover.resize(static_cast<size_t>(m_rasterW) * m_rasterH);
        if (memDs->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, m_rasterW, m_rasterH,
                m_landcover.data(), m_rasterW, m_rasterH,
                GDT_Int32, 0, 0) == CE_None)
            m_hasLandcover = true;
    }

    GDALClose(memDs);
    GDALClose(ar50);
    return m_hasLandcover;
}

bool MapView::LoadRailway(const std::string& railwayPath)
{
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds)
        return false;

    // Set up coordinate transform from railway CRS to tile CRS
    OGRSpatialReference tileSrs;
    tileSrs.importFromWkt(m_tileProjectionWkt.c_str());
    tileSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // Tile extent in tile CRS for filtering
    double tileMinX = m_gt[0];
    double tileMaxX = m_gt[0] + m_rasterW * m_gt[1];
    double tileMaxY = m_gt[3];
    double tileMinY = m_gt[3] + m_rasterH * m_gt[5];

    auto clamp = [](double v) -> int {
        constexpr double kMax = 100000.0;
        return static_cast<int>(std::clamp(v, -kMax, kMax));
    };

    // Load track segments (Banelenke)
    // GML has 8172 individual track segments including sidings, passing loops,
    // and parallel tracks at stations — much more detail than the FGDB version.
    OGRLayer* tracks = ds->GetLayerByName("Banelenke");
    if (tracks) {
        const OGRSpatialReference* trackSrs = tracks->GetSpatialRef();
        OGRCoordinateTransformation* toTile = nullptr;
        if (trackSrs) {
            OGRSpatialReference srcSrs(*trackSrs);
            srcSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            toTile = OGRCreateCoordinateTransformation(&srcSrs, &tileSrs);
        }

        tracks->ResetReading();
        OGRFeature* feat;
        while ((feat = tracks->GetNextFeature()) != nullptr) {
            OGRGeometry* geom = feat->GetGeometryRef();
            if (!geom) { OGRFeature::DestroyFeature(feat); continue; }

            // Clone and transform to tile CRS
            OGRGeometry* clone = geom->clone();
            if (toTile)
                clone->transform(toTile);

            // Check if geometry intersects the tile extent
            OGREnvelope env;
            clone->getEnvelope(&env);
            if (env.MaxX < tileMinX || env.MinX > tileMaxX ||
                env.MaxY < tileMinY || env.MinY > tileMaxY) {
                delete clone;
                OGRFeature::DestroyFeature(feat);
                continue;
            }

            // Read attributes (GML field names are lowercase)
            const char* nameField = feat->GetFieldAsString("banenavn");
            const char* statusField = feat->GetFieldAsString("banestatus");
            const char* mediumField = feat->GetFieldAsString("medium");
            std::string lineName = nameField ? nameField : "";
            char status = (statusField && statusField[0]) ? statusField[0] : 'I';
            char medium = (mediumField && mediumField[0]) ? mediumField[0] : ' ';

            auto processLine = [&](OGRLineString* line) {
                RailwaySegment seg;
                seg.name = lineName;
                seg.status = status;
                seg.medium = medium;
                int nPts = line->getNumPoints();
                for (int p = 0; p < nPts; p++) {
                    double x = line->getX(p);
                    double y = line->getY(p);
                    int px = clamp(m_invGt[0] + x * m_invGt[1] + y * m_invGt[2]);
                    int py = clamp(m_invGt[3] + x * m_invGt[4] + y * m_invGt[5]);
                    seg.points.push_back({px, py});
                }
                if (!seg.points.empty())
                    m_railSegments.push_back(std::move(seg));
            };

            OGRwkbGeometryType gtype = wkbFlatten(clone->getGeometryType());
            if (gtype == wkbLineString) {
                processLine(static_cast<OGRLineString*>(clone));
            } else if (gtype == wkbMultiLineString) {
                auto* multi = static_cast<OGRMultiLineString*>(clone);
                for (int g = 0; g < multi->getNumGeometries(); g++)
                    processLine(static_cast<OGRLineString*>(multi->getGeometryRef(g)));
            }

            delete clone;
            OGRFeature::DestroyFeature(feat);
        }

        if (toTile)
            OCTDestroyCoordinateTransformation(toTile);
    }

    // Load stations (Stasjonsnode)
    OGRLayer* stations = ds->GetLayerByName("Stasjonsnode");
    if (stations) {
        const OGRSpatialReference* stationSrs = stations->GetSpatialRef();
        OGRCoordinateTransformation* toTile = nullptr;
        if (stationSrs) {
            OGRSpatialReference srcSrs(*stationSrs);
            srcSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            toTile = OGRCreateCoordinateTransformation(&srcSrs, &tileSrs);
        }

        stations->ResetReading();
        OGRFeature* feat;
        while ((feat = stations->GetNextFeature()) != nullptr) {
            OGRGeometry* geom = feat->GetGeometryRef();
            if (!geom || wkbFlatten(geom->getGeometryType()) != wkbPoint) {
                OGRFeature::DestroyFeature(feat);
                continue;
            }

            OGRPoint* pt = static_cast<OGRPoint*>(geom->clone());
            if (toTile)
                pt->transform(toTile);

            double x = pt->getX(), y = pt->getY();
            delete pt;

            // Check if within tile bounds
            if (x < tileMinX || x > tileMaxX || y < tileMinY || y > tileMaxY) {
                OGRFeature::DestroyFeature(feat);
                continue;
            }

            int px = clamp(m_invGt[0] + x * m_invGt[1] + y * m_invGt[2]);
            int py = clamp(m_invGt[3] + x * m_invGt[4] + y * m_invGt[5]);

            // GML uses 'stasjonsnavn' and 'stasjonstype'
            const char* name = feat->GetFieldAsString("stasjonsnavn");
            const char* typeField = feat->GetFieldAsString("stasjonstype");
            char type = (typeField && typeField[0]) ? typeField[0] : 'S';

            m_railStations.push_back({{px, py}, name ? name : "", type});
            OGRFeature::DestroyFeature(feat);
        }

        if (toTile)
            OCTDestroyCoordinateTransformation(toTile);
    }

    GDALClose(ds);
    m_hasRailway = !m_railSegments.empty() || !m_railStations.empty();
    return m_hasRailway;
}

bool MapView::LoadRoads(const std::string& roadsPath)
{
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(roadsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds)
        return false;

    OGRLayer* layer = ds->GetLayerByName("roads");
    if (!layer) {
        GDALClose(ds);
        return false;
    }

    // Set up coordinate transform from roads CRS to tile CRS
    OGRSpatialReference tileSrs;
    tileSrs.importFromWkt(m_tileProjectionWkt.c_str());
    tileSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    const OGRSpatialReference* roadSrs = layer->GetSpatialRef();
    OGRCoordinateTransformation* toTile = nullptr;
    if (roadSrs) {
        OGRSpatialReference srcSrs(*roadSrs);
        srcSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        toTile = OGRCreateCoordinateTransformation(&srcSrs, &tileSrs);
    }

    // Tile extent in tile CRS for filtering
    double tileMinX = m_gt[0];
    double tileMaxX = m_gt[0] + m_rasterW * m_gt[1];
    double tileMaxY = m_gt[3];
    double tileMinY = m_gt[3] + m_rasterH * m_gt[5];

    // Apply spatial filter in road CRS (transform tile extent to road CRS)
    if (toTile && roadSrs) {
        OGRSpatialReference srcSrs(*roadSrs);
        srcSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRCoordinateTransformation* fromTile =
            OGRCreateCoordinateTransformation(&tileSrs, &srcSrs);
        if (fromTile) {
            double x1 = tileMinX, y1 = tileMinY;
            double x2 = tileMaxX, y2 = tileMaxY;
            fromTile->Transform(1, &x1, &y1);
            fromTile->Transform(1, &x2, &y2);
            layer->SetSpatialFilterRect(
                std::min(x1, x2), std::min(y1, y2),
                std::max(x1, x2), std::max(y1, y2));
            OCTDestroyCoordinateTransformation(fromTile);
        }
    }

    auto clamp = [](double v) -> int {
        constexpr double kMax = 100000.0;
        return static_cast<int>(std::clamp(v, -kMax, kMax));
    };

    layer->ResetReading();
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom || wkbFlatten(geom->getGeometryType()) != wkbLineString) {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

        OGRLineString* line = static_cast<OGRLineString*>(geom->clone());
        if (toTile)
            line->transform(toTile);

        RoadSegment seg;
        const char* cat = feat->GetFieldAsString("vegkategori");
        seg.category = (cat && cat[0]) ? cat[0] : 'F';
        seg.roadNumber = feat->GetFieldAsInteger("vegnummer");

        int nPts = line->getNumPoints();
        for (int p = 0; p < nPts; p++) {
            double x = line->getX(p);
            double y = line->getY(p);
            int px = clamp(m_invGt[0] + x * m_invGt[1] + y * m_invGt[2]);
            int py = clamp(m_invGt[3] + x * m_invGt[4] + y * m_invGt[5]);
            seg.points.push_back({px, py});
        }
        delete line;

        if (!seg.points.empty())
            m_roadSegments.push_back(std::move(seg));

        OGRFeature::DestroyFeature(feat);
    }

    if (toTile)
        OCTDestroyCoordinateTransformation(toTile);

    GDALClose(ds);
    m_hasRoads = !m_roadSegments.empty();
    return m_hasRoads;
}

bool MapView::LoadBuildings(const std::string& osmBuildingsPath)
{
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(osmBuildingsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds)
        return false;

    OGRLayer* layer = ds->GetLayerByName("buildings");
    if (!layer) {
        GDALClose(ds);
        return false;
    }

    // Set up coordinate transform from WGS84 to tile CRS
    OGRSpatialReference tileSrs;
    tileSrs.importFromWkt(m_tileProjectionWkt.c_str());
    tileSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    const OGRSpatialReference* srcSrs = layer->GetSpatialRef();
    OGRCoordinateTransformation* toTile = nullptr;
    if (srcSrs) {
        OGRSpatialReference src(*srcSrs);
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        toTile = OGRCreateCoordinateTransformation(&src, &tileSrs);
    }

    // Spatial filter: transform tile extent to building CRS
    if (toTile && srcSrs) {
        OGRSpatialReference src(*srcSrs);
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRCoordinateTransformation* fromTile =
            OGRCreateCoordinateTransformation(&tileSrs, &src);
        if (fromTile) {
            double x1 = m_gt[0], y1 = m_gt[3] + m_rasterH * m_gt[5];
            double x2 = m_gt[0] + m_rasterW * m_gt[1], y2 = m_gt[3];
            fromTile->Transform(1, &x1, &y1);
            fromTile->Transform(1, &x2, &y2);
            layer->SetSpatialFilterRect(
                std::min(x1, x2), std::min(y1, y2),
                std::max(x1, x2), std::max(y1, y2));
            OCTDestroyCoordinateTransformation(fromTile);
        }
    }

    auto clamp = [](double v) -> int {
        constexpr double kMax = 100000.0;
        return static_cast<int>(std::clamp(v, -kMax, kMax));
    };

    auto ringToPixels = [&](OGRLinearRing* ring) -> std::vector<wxPoint> {
        std::vector<wxPoint> pts;
        if (!ring) return pts;
        int n = ring->getNumPoints();
        pts.reserve(n);
        for (int i = 0; i < n; i++) {
            double x = ring->getX(i);
            double y = ring->getY(i);
            int px = clamp(m_invGt[0] + x * m_invGt[1] + y * m_invGt[2]);
            int py = clamp(m_invGt[3] + x * m_invGt[4] + y * m_invGt[5]);
            pts.push_back({px, py});
        }
        return pts;
    };

    layer->ResetReading();
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom) {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

        // Transform entire geometry to tile CRS at once
        OGRGeometry* transformed = geom->clone();
        if (toTile)
            transformed->transform(toTile);

        auto processPolygon = [&](OGRPolygon* poly) {
            BuildingOutline bldg;
            bldg.rings.push_back(ringToPixels(poly->getExteriorRing()));
            for (int h = 0; h < poly->getNumInteriorRings(); h++)
                bldg.rings.push_back(ringToPixels(poly->getInteriorRing(h)));

            const char* type = feat->GetFieldAsString("building");
            if (type && type[0]) bldg.type = type;

            const char* name = feat->GetFieldAsString("name");
            if (name && name[0]) bldg.name = name;

            int levelsIdx = feat->GetFieldIndex("levels");
            if (levelsIdx >= 0 && feat->IsFieldSetAndNotNull(levelsIdx))
                bldg.levels = feat->GetFieldAsInteger(levelsIdx);

            if (!bldg.rings.empty() && bldg.rings[0].size() >= 3)
                m_buildings.push_back(std::move(bldg));
        };

        OGRwkbGeometryType gtype = wkbFlatten(transformed->getGeometryType());
        if (gtype == wkbPolygon) {
            processPolygon(static_cast<OGRPolygon*>(transformed));
        } else if (gtype == wkbMultiPolygon) {
            OGRMultiPolygon* mp = static_cast<OGRMultiPolygon*>(transformed);
            for (int g = 0; g < mp->getNumGeometries(); g++)
                processPolygon(static_cast<OGRPolygon*>(mp->getGeometryRef(g)));
        }

        delete transformed;
        OGRFeature::DestroyFeature(feat);
    }

    if (toTile)
        OCTDestroyCoordinateTransformation(toTile);

    GDALClose(ds);
    m_hasBuildings = !m_buildings.empty();
    return m_hasBuildings;
}

wxImage MapView::RenderElevation() const
{
    wxImage img(m_rasterW, m_rasterH);
    unsigned char* rgb = img.GetData();

    for (int i = 0; i < m_rasterW * m_rasterH; i++) {
        int32_t artype = m_hasLandcover ? m_landcover[i] : 0;
        auto c = ElevToColor(m_elevation[i], artype);
        rgb[3 * i]     = c.r;
        rgb[3 * i + 1] = c.g;
        rgb[3 * i + 2] = c.b;
    }
    return img;
}

MapView::RGB MapView::ElevToColor(float elev, int32_t artype) const
{
    // NoData → steel blue (water / ocean)
    if (m_hasNodata && elev == static_cast<float>(m_nodata))
        return {70, 130, 180};

    // Water and glacier override elevation entirely
    if (artype == 80) return {65, 105, 225};   // freshwater — royal blue
    if (artype == 81) return {30, 60, 150};    // sea — dark blue
    if (artype == 70) return {220, 240, 255};  // glacier — ice white

    // Below sea level → dark teal
    if (elev <= 0.0f)
        return {0, 80, 80};

    // Normalize to [0, 1]
    float t = (elev - m_minElev) / (m_maxElev - m_minElev);
    t = std::clamp(t, 0.0f, 1.0f);

    // Terrain color stops
    struct Stop { float pos; unsigned char r, g, b; };
    static constexpr Stop stops[] = {
        {0.00f,   1,  97,  69},   // dark green — lowland
        {0.15f,  46, 153,  79},   // green
        {0.30f, 121, 200,  87},   // light green
        {0.45f, 233, 230, 110},   // yellow
        {0.55f, 205, 163,  69},   // tan
        {0.70f, 157, 110,  68},   // brown
        {0.85f, 185, 176, 172},   // grey — alpine
        {1.00f, 255, 255, 255},   // white — peaks
    };
    constexpr int nStops = sizeof(stops) / sizeof(stops[0]);

    int hi = 1;
    while (hi < nStops - 1 && stops[hi].pos < t)
        ++hi;
    int lo = hi - 1;

    float frac = (t - stops[lo].pos) / (stops[hi].pos - stops[lo].pos);
    frac = std::clamp(frac, 0.0f, 1.0f);

    auto lerp = [](unsigned char a, unsigned char b, float f) -> unsigned char {
        return static_cast<unsigned char>(a + f * (b - a));
    };

    RGB elevColor = {
        lerp(stops[lo].r, stops[hi].r, frac),
        lerp(stops[lo].g, stops[hi].g, frac),
        lerp(stops[lo].b, stops[hi].b, frac),
    };

    // If no land cover data, return pure elevation color
    if (artype == 0)
        return elevColor;

    // Blend elevation color with land cover tint (60% elev, 40% land cover)
    RGB lcColor = LandCoverColor(artype);
    return {
        lerp(elevColor.r, lcColor.r, 0.4f),
        lerp(elevColor.g, lcColor.g, 0.4f),
        lerp(elevColor.b, lcColor.b, 0.4f),
    };
}

MapView::RGB MapView::LandCoverColor(int32_t artype)
{
    switch (artype) {
        case 10: return {160, 160, 160};  // built-up — grey
        case 20: return {240, 220, 130};  // agriculture — warm yellow
        case 30: return { 34, 139,  34};  // forest — green
        case 50: return {194, 178, 128};  // open land — khaki
        case 60: return {107, 142,  35};  // bog — olive
        case 70: return {220, 240, 255};  // glacier — ice
        case 80: return { 65, 105, 225};  // freshwater
        case 81: return { 30,  60, 150};  // sea
        default: return {128, 128, 128};
    }
}

const char* MapView::LandCoverName(int32_t artype)
{
    switch (artype) {
        case 10: return "Built-up";
        case 20: return "Agriculture";
        case 30: return "Forest";
        case 50: return "Open land";
        case 60: return "Bog";
        case 70: return "Glacier";
        case 80: return "Freshwater";
        case 81: return "Sea";
        default: return nullptr;
    }
}

void MapView::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_canvas);
    m_canvas->DoPrepareDC(dc);
    if (m_bitmap.IsOk())
        dc.DrawBitmap(m_bitmap, 0, 0, false);

    // Road overlay (drawn first so railway appears on top)
    if (m_hasRoads) {
        for (const auto& seg : m_roadSegments) {
            if (seg.points.size() < 2)
                continue;
            // Color and width by road category
            switch (seg.category) {
                case 'E':
                    dc.SetPen(wxPen(wxColour(50, 120, 50), 3));
                    break;
                case 'R':
                    dc.SetPen(wxPen(wxColour(180, 80, 0), 2));
                    break;
                case 'F':
                    dc.SetPen(wxPen(wxColour(100, 100, 100), 1));
                    break;
                case 'K':
                    dc.SetPen(wxPen(wxColour(160, 160, 160), 1));
                    break;
                default:  // P (private)
                    dc.SetPen(wxPen(wxColour(180, 180, 180), 1, wxPENSTYLE_SHORT_DASH));
                    break;
            }
            for (size_t i = 1; i < seg.points.size(); i++)
                dc.DrawLine(seg.points[i - 1], seg.points[i]);
        }
    }

    // Building overlay
    if (m_hasBuildings) {
        dc.SetPen(wxPen(wxColour(180, 100, 60), 1));
        dc.SetBrush(wxBrush(wxColour(200, 160, 130, 128)));
        for (const auto& bldg : m_buildings) {
            if (bldg.rings.empty() || bldg.rings[0].size() < 3)
                continue;
            dc.DrawPolygon(static_cast<int>(bldg.rings[0].size()),
                           bldg.rings[0].data());
        }
    }

    // Railway overlay
    if (m_hasRailway) {
        // Draw track lines — style varies by status and medium
        for (const auto& seg : m_railSegments) {
            if (seg.points.size() < 2)
                continue;

            switch (seg.status) {
                case 'I':  // operational
                    if (seg.medium == 'U')
                        dc.SetPen(wxPen(wxColour(160, 20, 20), 2, wxPENSTYLE_SHORT_DASH));
                    else
                        dc.SetPen(wxPen(wxColour(160, 20, 20), 2));
                    break;
                case 'N':  // closed
                    dc.SetPen(wxPen(wxColour(140, 120, 120), 1, wxPENSTYLE_DOT));
                    break;
                case 'M':  // museum
                    dc.SetPen(wxPen(wxColour(140, 100, 40), 1));
                    break;
                case 'P':  // planned
                case 'F':  // under construction
                    dc.SetPen(wxPen(wxColour(160, 20, 20), 1, wxPENSTYLE_LONG_DASH));
                    break;
                default:
                    dc.SetPen(wxPen(wxColour(160, 20, 20), 2));
                    break;
            }

            for (size_t i = 1; i < seg.points.size(); i++)
                dc.DrawLine(seg.points[i - 1], seg.points[i]);
        }

        // Draw stations
        dc.SetFont(wxFont(7, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                          wxFONTWEIGHT_BOLD));
        for (const auto& st : m_railStations) {
            if (st.type == 'I') {
                // Interchange — larger marker
                dc.SetPen(wxPen(*wxBLACK, 1));
                dc.SetBrush(wxBrush(wxColour(255, 180, 50)));
                dc.DrawCircle(st.pos, 5);
            } else {
                dc.SetPen(wxPen(*wxBLACK, 1));
                dc.SetBrush(wxBrush(wxColour(255, 220, 50)));
                dc.DrawCircle(st.pos, 4);
            }

            if (!st.name.empty()) {
                dc.SetTextForeground(*wxBLACK);
                dc.DrawText(st.name, st.pos.x + 6, st.pos.y - 6);
            }
        }
    }
}

void MapView::OnMouseMove(wxMouseEvent& event)
{
    if (m_elevation.empty())
        return;

    int viewX, viewY;
    m_canvas->CalcUnscrolledPosition(event.GetX(), event.GetY(),
                                     &viewX, &viewY);

    if (viewX < 0 || viewX >= m_rasterW || viewY < 0 || viewY >= m_rasterH)
        return;

    const size_t idx = static_cast<size_t>(viewY) * m_rasterW + viewX;

    // Pixel coordinates
    SetStatusText(wxString::Format("Pixel (%d, %d)", viewX, viewY), 0);

    // Elevation
    float elev = m_elevation[idx];
    if (m_hasNodata && elev == static_cast<float>(m_nodata))
        SetStatusText("Water / NoData", 1);
    else
        SetStatusText(wxString::Format("Elev: %.1f m", elev), 1);

    // Geographic coordinates
    if (m_toWGS84) {
        double geoX = m_gt[0] + viewX * m_gt[1] + viewY * m_gt[2];
        double geoY = m_gt[3] + viewX * m_gt[4] + viewY * m_gt[5];
        double lon = geoX, lat = geoY;
        if (m_toWGS84->Transform(1, &lon, &lat))
            SetStatusText(wxString::Format("%.4f\u00b0N  %.4f\u00b0E", lat, lon), 2);
    }

    // Land cover type
    if (m_hasLandcover) {
        int32_t artype = m_landcover[idx];
        const char* name = LandCoverName(artype);
        SetStatusText(name ? wxString(name) : wxString(""), 3);
    }
}

#include "LocalMapView.h"
#include "ProfileData.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <wx/dcbuffer.h>

#include <algorithm>
#include <cmath>

LocalMapView::LocalMapView(wxWindow* parent,
                           const wxString& title,
                           double centerX, double centerY,
                           const ProfileData& profileData,
                           const std::string& railwayPath,
                           const std::string& roadsPath,
                           const std::string& osmDataPath)
    : wxFrame(parent, wxID_ANY, title, wxDefaultPosition, wxSize(900, 900))
    , m_cx(centerX), m_cy(centerY)
{
    m_canvas = new wxScrolledCanvas(this, wxID_ANY);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_canvas->Bind(wxEVT_PAINT,  &LocalMapView::OnPaint,     this);
    m_canvas->Bind(wxEVT_MOTION, &LocalMapView::OnMouseMove,  this);

    CreateStatusBar(3);

    BuildElevationBitmap(profileData);

    if (!railwayPath.empty())
        LoadGmlRailway(railwayPath);
    if (!roadsPath.empty())
        LoadRoads(roadsPath);
    if (!osmDataPath.empty())
        LoadOsmData(osmDataPath);

    m_canvas->SetScrollbars(1, 1, kPixels, kPixels, kPixels / 2, kPixels / 2);

    SetStatusText(wxString::Format("Center: %.0f, %.0f (EPSG:25833)", m_cx, m_cy), 0);
}

wxPoint LocalMapView::GeoToPixel(double x, double y) const
{
    int px = static_cast<int>((x - m_cx + kHalfSize) / kScale);
    int py = static_cast<int>((m_cy + kHalfSize - y) / kScale);
    return {px, py};
}

// ─── Elevation background ──────────────────────────────────────────

void LocalMapView::BuildElevationBitmap(const ProfileData& profileData)
{
    // Sample elevation at 10m spacing (400×400), then scale up
    constexpr int kSampleRes = 10;
    constexpr int kSamples = 4000 / kSampleRes;  // 400

    std::vector<float> elev(kSamples * kSamples);
    float minE = 1e9f, maxE = -1e9f;

    double x0 = m_cx - kHalfSize;
    double y0 = m_cy + kHalfSize;  // top-left in geo coords (y points north)

    for (int row = 0; row < kSamples; row++) {
        for (int col = 0; col < kSamples; col++) {
            double gx = x0 + (col + 0.5) * kSampleRes;
            double gy = y0 - (row + 0.5) * kSampleRes;
            float e;
            if (profileData.SampleElevation(gx, gy, e)) {
                elev[row * kSamples + col] = e;
                minE = std::min(minE, e);
                maxE = std::max(maxE, e);
            } else {
                elev[row * kSamples + col] = -9999;
            }
        }
    }

    if (minE > maxE) { minE = 0; maxE = 1; }
    float range = maxE - minE;
    if (range < 1) range = 1;

    // Render small image
    wxImage small(kSamples, kSamples);
    unsigned char* rgb = small.GetData();

    struct Stop { float pos; unsigned char r, g, b; };
    static constexpr Stop stops[] = {
        {0.00f,   1,  97,  69},
        {0.15f,  46, 153,  79},
        {0.30f, 121, 200,  87},
        {0.45f, 233, 230, 110},
        {0.55f, 205, 163,  69},
        {0.70f, 157, 110,  68},
        {0.85f, 185, 176, 172},
        {1.00f, 255, 255, 255},
    };
    constexpr int nStops = sizeof(stops) / sizeof(stops[0]);

    for (int i = 0; i < kSamples * kSamples; i++) {
        float e = elev[i];
        unsigned char r, g, b;
        if (e < -9000) {
            r = 70; g = 130; b = 180;  // water
        } else {
            float t = std::clamp((e - minE) / range, 0.0f, 1.0f);
            int hi = 1;
            while (hi < nStops - 1 && stops[hi].pos < t) ++hi;
            int lo = hi - 1;
            float f = (t - stops[lo].pos) / (stops[hi].pos - stops[lo].pos);
            f = std::clamp(f, 0.0f, 1.0f);
            r = static_cast<unsigned char>(stops[lo].r + f * (stops[hi].r - stops[lo].r));
            g = static_cast<unsigned char>(stops[lo].g + f * (stops[hi].g - stops[lo].g));
            b = static_cast<unsigned char>(stops[lo].b + f * (stops[hi].b - stops[lo].b));
        }
        rgb[3 * i]     = r;
        rgb[3 * i + 1] = g;
        rgb[3 * i + 2] = b;
    }

    // Scale up to display size
    wxImage scaled = small.Scale(kPixels, kPixels, wxIMAGE_QUALITY_BILINEAR);
    m_bitmap = wxBitmap(scaled);
}

// ─── GML railway ───────────────────────────────────────────────────

void LocalMapView::LoadGmlRailway(const std::string& railwayPath)
{
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return;

    // We need to transform from railway CRS to EPSG:25833
    OGRSpatialReference epsg25833;
    epsg25833.importFromEPSG(25833);
    epsg25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    double geoMinX = m_cx - kHalfSize;
    double geoMaxX = m_cx + kHalfSize;
    double geoMinY = m_cy - kHalfSize;
    double geoMaxY = m_cy + kHalfSize;

    // Load tracks (Banelenke)
    OGRLayer* tracks = ds->GetLayerByName("Banelenke");
    if (tracks) {
        const OGRSpatialReference* srcSrs = tracks->GetSpatialRef();
        OGRCoordinateTransformation* ct = nullptr;
        if (srcSrs) {
            OGRSpatialReference src(*srcSrs);
            src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            ct = OGRCreateCoordinateTransformation(&src, &epsg25833);
        }

        // Set spatial filter in source CRS
        if (srcSrs) {
            OGRSpatialReference src(*srcSrs);
            src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            OGRCoordinateTransformation* inv =
                OGRCreateCoordinateTransformation(&epsg25833, &src);
            if (inv) {
                double fx1 = geoMinX, fy1 = geoMinY;
                double fx2 = geoMaxX, fy2 = geoMaxY;
                inv->Transform(1, &fx1, &fy1);
                inv->Transform(1, &fx2, &fy2);
                tracks->SetSpatialFilterRect(
                    std::min(fx1, fx2), std::min(fy1, fy2),
                    std::max(fx1, fx2), std::max(fy1, fy2));
                OCTDestroyCoordinateTransformation(inv);
            }
        }

        tracks->ResetReading();
        OGRFeature* feat;
        while ((feat = tracks->GetNextFeature()) != nullptr) {
            OGRGeometry* geom = feat->GetGeometryRef();
            if (!geom) { OGRFeature::DestroyFeature(feat); continue; }

            OGRGeometry* clone = geom->clone();
            if (ct) clone->transform(ct);

            const char* mediumField = feat->GetFieldAsString("medium");
            char medium = (mediumField && mediumField[0]) ? mediumField[0] : ' ';
            const char* nameField = feat->GetFieldAsString("banenavn");

            auto processLine = [&](OGRLineString* line) {
                TrackSeg seg;
                seg.railway = "rail";
                seg.medium = medium;
                seg.isOsm = false;
                if (nameField && nameField[0]) seg.railway = nameField;
                int nPts = line->getNumPoints();
                for (int p = 0; p < nPts; p++)
                    seg.points.push_back(GeoToPixel(line->getX(p), line->getY(p)));
                if (!seg.points.empty())
                    m_tracks.push_back(std::move(seg));
            };

            OGRwkbGeometryType gtype = wkbFlatten(clone->getGeometryType());
            if (gtype == wkbLineString)
                processLine(static_cast<OGRLineString*>(clone));
            else if (gtype == wkbMultiLineString) {
                auto* multi = static_cast<OGRMultiLineString*>(clone);
                for (int g = 0; g < multi->getNumGeometries(); g++)
                    processLine(static_cast<OGRLineString*>(multi->getGeometryRef(g)));
            }

            delete clone;
            OGRFeature::DestroyFeature(feat);
        }

        if (ct) OCTDestroyCoordinateTransformation(ct);
    }

    // Load stations (Stasjonsnode)
    OGRLayer* stations = ds->GetLayerByName("Stasjonsnode");
    if (stations) {
        const OGRSpatialReference* srcSrs = stations->GetSpatialRef();
        OGRCoordinateTransformation* ct = nullptr;
        if (srcSrs) {
            OGRSpatialReference src(*srcSrs);
            src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            ct = OGRCreateCoordinateTransformation(&src, &epsg25833);
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
            if (ct) pt->transform(ct);
            double x = pt->getX(), y = pt->getY();
            delete pt;

            if (x < geoMinX || x > geoMaxX || y < geoMinY || y > geoMaxY) {
                OGRFeature::DestroyFeature(feat);
                continue;
            }

            Station st;
            st.pos = GeoToPixel(x, y);
            const char* name = feat->GetFieldAsString("stasjonsnavn");
            if (name && name[0]) st.name = name;
            m_stations.push_back(std::move(st));
            OGRFeature::DestroyFeature(feat);
        }

        if (ct) OCTDestroyCoordinateTransformation(ct);
    }

    GDALClose(ds);
}

// ─── Roads ─────────────────────────────────────────────────────────

void LocalMapView::LoadRoads(const std::string& roadsPath)
{
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(roadsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return;

    OGRLayer* layer = ds->GetLayerByName("roads");
    if (!layer) { GDALClose(ds); return; }

    OGRSpatialReference epsg25833;
    epsg25833.importFromEPSG(25833);
    epsg25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    const OGRSpatialReference* srcSrs = layer->GetSpatialRef();
    OGRCoordinateTransformation* ct = nullptr;
    if (srcSrs) {
        OGRSpatialReference src(*srcSrs);
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        ct = OGRCreateCoordinateTransformation(&src, &epsg25833);
    }

    // Spatial filter in source CRS
    if (srcSrs) {
        OGRSpatialReference src(*srcSrs);
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRCoordinateTransformation* inv =
            OGRCreateCoordinateTransformation(&epsg25833, &src);
        if (inv) {
            double fx1 = m_cx - kHalfSize, fy1 = m_cy - kHalfSize;
            double fx2 = m_cx + kHalfSize, fy2 = m_cy + kHalfSize;
            inv->Transform(1, &fx1, &fy1);
            inv->Transform(1, &fx2, &fy2);
            layer->SetSpatialFilterRect(
                std::min(fx1, fx2), std::min(fy1, fy2),
                std::max(fx1, fx2), std::max(fy1, fy2));
            OCTDestroyCoordinateTransformation(inv);
        }
    }

    layer->ResetReading();
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom || wkbFlatten(geom->getGeometryType()) != wkbLineString) {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

        OGRLineString* line = static_cast<OGRLineString*>(geom->clone());
        if (ct) line->transform(ct);

        RoadSeg seg;
        const char* cat = feat->GetFieldAsString("vegkategori");
        seg.category = (cat && cat[0]) ? cat[0] : 'F';
        seg.roadNumber = feat->GetFieldAsInteger("vegnummer");

        int nPts = line->getNumPoints();
        for (int p = 0; p < nPts; p++)
            seg.points.push_back(GeoToPixel(line->getX(p), line->getY(p)));
        delete line;

        if (!seg.points.empty())
            m_roads.push_back(std::move(seg));

        OGRFeature::DestroyFeature(feat);
    }

    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
}

// ─── OSM data (tracks, platforms, switches) ────────────────────────

void LocalMapView::LoadOsmData(const std::string& osmDataPath)
{
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(osmDataPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return;

    OGRSpatialReference epsg25833;
    epsg25833.importFromEPSG(25833);
    epsg25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRCoordinateTransformation* ct =
        OGRCreateCoordinateTransformation(&wgs84, &epsg25833);
    OGRCoordinateTransformation* inv =
        OGRCreateCoordinateTransformation(&epsg25833, &wgs84);

    // Compute WGS84 filter bounds
    double fx1 = m_cx - kHalfSize, fy1 = m_cy - kHalfSize;
    double fx2 = m_cx + kHalfSize, fy2 = m_cy + kHalfSize;
    if (inv) {
        inv->Transform(1, &fx1, &fy1);
        inv->Transform(1, &fx2, &fy2);
    }
    double fMinX = std::min(fx1, fx2), fMinY = std::min(fy1, fy2);
    double fMaxX = std::max(fx1, fx2), fMaxY = std::max(fy1, fy2);

    // OSM railway tracks
    OGRLayer* trackLayer = ds->GetLayerByName("railway_tracks");
    if (trackLayer) {
        trackLayer->SetSpatialFilterRect(fMinX, fMinY, fMaxX, fMaxY);
        trackLayer->ResetReading();

        OGRFeature* feat;
        while ((feat = trackLayer->GetNextFeature()) != nullptr) {
            OGRGeometry* geom = feat->GetGeometryRef();
            if (!geom) { OGRFeature::DestroyFeature(feat); continue; }

            OGRGeometry* clone = geom->clone();
            if (ct) clone->transform(ct);

            auto processLine = [&](OGRLineString* line) {
                TrackSeg seg;
                seg.isOsm = true;
                const char* rw = feat->GetFieldAsString("railway");
                if (rw && rw[0]) seg.railway = rw;
                const char* svc = feat->GetFieldAsString("service");
                if (svc && svc[0]) seg.service = svc;
                const char* tn = feat->GetFieldAsString("tunnel");
                if (tn && (tn[0] == 'y' || tn[0] == 'Y')) seg.medium = 'U';
                const char* br = feat->GetFieldAsString("bridge");
                if (br && (br[0] == 'y' || br[0] == 'Y')) seg.medium = 'B';

                int nPts = line->getNumPoints();
                for (int p = 0; p < nPts; p++)
                    seg.points.push_back(GeoToPixel(line->getX(p), line->getY(p)));
                if (!seg.points.empty())
                    m_tracks.push_back(std::move(seg));
            };

            OGRwkbGeometryType gtype = wkbFlatten(clone->getGeometryType());
            if (gtype == wkbLineString)
                processLine(static_cast<OGRLineString*>(clone));
            else if (gtype == wkbMultiLineString) {
                auto* multi = static_cast<OGRMultiLineString*>(clone);
                for (int g = 0; g < multi->getNumGeometries(); g++)
                    processLine(static_cast<OGRLineString*>(multi->getGeometryRef(g)));
            }

            delete clone;
            OGRFeature::DestroyFeature(feat);
        }
    }

    // OSM railway points
    OGRLayer* pointLayer = ds->GetLayerByName("railway_points");
    if (pointLayer) {
        pointLayer->SetSpatialFilterRect(fMinX, fMinY, fMaxX, fMaxY);
        pointLayer->ResetReading();

        OGRFeature* feat;
        while ((feat = pointLayer->GetNextFeature()) != nullptr) {
            OGRGeometry* geom = feat->GetGeometryRef();
            if (!geom || wkbFlatten(geom->getGeometryType()) != wkbPoint) {
                OGRFeature::DestroyFeature(feat);
                continue;
            }

            OGRPoint* pt = static_cast<OGRPoint*>(geom->clone());
            if (ct) pt->transform(ct);

            RailPoint rp;
            rp.pos = GeoToPixel(pt->getX(), pt->getY());
            delete pt;

            const char* rw = feat->GetFieldAsString("railway");
            if (rw && rw[0]) rp.type = rw;
            const char* nm = feat->GetFieldAsString("name");
            if (nm && nm[0]) rp.name = nm;

            m_railPoints.push_back(std::move(rp));
            OGRFeature::DestroyFeature(feat);
        }
    }

    // OSM platforms
    OGRLayer* platLayer = ds->GetLayerByName("railway_platforms");
    if (platLayer) {
        platLayer->SetSpatialFilterRect(fMinX, fMinY, fMaxX, fMaxY);
        platLayer->ResetReading();

        OGRFeature* feat;
        while ((feat = platLayer->GetNextFeature()) != nullptr) {
            OGRGeometry* geom = feat->GetGeometryRef();
            if (!geom) { OGRFeature::DestroyFeature(feat); continue; }

            OGRGeometry* transformed = geom->clone();
            if (ct) transformed->transform(ct);

            Platform plat;
            const char* nm = feat->GetFieldAsString("name");
            if (nm && nm[0]) plat.name = nm;
            const char* ref = feat->GetFieldAsString("ref");
            if (ref && ref[0]) plat.ref = ref;

            OGRwkbGeometryType gtype = wkbFlatten(transformed->getGeometryType());
            if (gtype == wkbLineString) {
                plat.isLine = true;
                OGRLineString* line = static_cast<OGRLineString*>(transformed);
                int nPts = line->getNumPoints();
                for (int p = 0; p < nPts; p++)
                    plat.linePoints.push_back(
                        GeoToPixel(line->getX(p), line->getY(p)));
                if (!plat.linePoints.empty())
                    m_platforms.push_back(std::move(plat));
            } else if (gtype == wkbPolygon) {
                OGRPolygon* poly = static_cast<OGRPolygon*>(transformed);
                OGRLinearRing* ring = poly->getExteriorRing();
                if (ring) {
                    int n = ring->getNumPoints();
                    for (int i = 0; i < n; i++)
                        plat.ring.push_back(
                            GeoToPixel(ring->getX(i), ring->getY(i)));
                    if (plat.ring.size() >= 3)
                        m_platforms.push_back(std::move(plat));
                }
            } else if (gtype == wkbMultiPolygon) {
                OGRMultiPolygon* mp = static_cast<OGRMultiPolygon*>(transformed);
                for (int g = 0; g < mp->getNumGeometries(); g++) {
                    Platform sub;
                    sub.name = plat.name;
                    sub.ref = plat.ref;
                    OGRPolygon* poly =
                        static_cast<OGRPolygon*>(mp->getGeometryRef(g));
                    OGRLinearRing* ring = poly->getExteriorRing();
                    if (ring) {
                        int n = ring->getNumPoints();
                        for (int i = 0; i < n; i++)
                            sub.ring.push_back(
                                GeoToPixel(ring->getX(i), ring->getY(i)));
                        if (sub.ring.size() >= 3)
                            m_platforms.push_back(std::move(sub));
                    }
                }
            }

            delete transformed;
            OGRFeature::DestroyFeature(feat);
        }
    }

    if (inv) OCTDestroyCoordinateTransformation(inv);
    if (ct) OCTDestroyCoordinateTransformation(ct);
    GDALClose(ds);
}

// ─── Rendering ─────────────────────────────────────────────────────

void LocalMapView::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_canvas);
    m_canvas->DoPrepareDC(dc);

    // Elevation background
    if (m_bitmap.IsOk())
        dc.DrawBitmap(m_bitmap, 0, 0, false);

    // Roads (under railway)
    for (const auto& seg : m_roads) {
        if (seg.points.size() < 2) continue;
        switch (seg.category) {
            case 'E': dc.SetPen(wxPen(wxColour(50, 120, 50), 3)); break;
            case 'R': dc.SetPen(wxPen(wxColour(180, 80, 0), 2)); break;
            case 'F': dc.SetPen(wxPen(wxColour(100, 100, 100), 1)); break;
            case 'K': dc.SetPen(wxPen(wxColour(160, 160, 160), 1)); break;
            default:  dc.SetPen(wxPen(wxColour(180, 180, 180), 1,
                                       wxPENSTYLE_SHORT_DASH)); break;
        }
        for (size_t i = 1; i < seg.points.size(); i++)
            dc.DrawLine(seg.points[i - 1], seg.points[i]);
    }

    // Platforms (under tracks)
    dc.SetPen(wxPen(wxColour(100, 100, 130), 1));
    dc.SetBrush(wxBrush(wxColour(180, 180, 210)));
    for (const auto& plat : m_platforms) {
        if (plat.isLine) {
            if (plat.linePoints.size() >= 2) {
                dc.SetPen(wxPen(wxColour(100, 100, 130), 4));
                for (size_t i = 1; i < plat.linePoints.size(); i++)
                    dc.DrawLine(plat.linePoints[i - 1], plat.linePoints[i]);
                dc.SetPen(wxPen(wxColour(100, 100, 130), 1));
            }
        } else if (plat.ring.size() >= 3) {
            dc.DrawPolygon(static_cast<int>(plat.ring.size()),
                           plat.ring.data());
        }

        // Label platforms with ref number
        if (!plat.ref.empty()) {
            wxPoint labelPos;
            if (plat.isLine && !plat.linePoints.empty())
                labelPos = plat.linePoints[plat.linePoints.size() / 2];
            else if (!plat.ring.empty())
                labelPos = plat.ring[0];
            else
                continue;
            dc.SetFont(wxFont(7, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                               wxFONTWEIGHT_BOLD));
            dc.SetTextForeground(wxColour(50, 50, 80));
            dc.DrawText(wxString::FromUTF8(plat.ref),
                        labelPos.x + 2, labelPos.y - 4);
        }
    }

    // Track segments
    for (const auto& seg : m_tracks) {
        if (seg.points.size() < 2) continue;

        if (seg.isOsm) {
            // OSM tracks — styled by service type
            if (seg.service == "yard")
                dc.SetPen(wxPen(wxColour(180, 120, 60), 1, wxPENSTYLE_SHORT_DASH));
            else if (seg.service == "siding")
                dc.SetPen(wxPen(wxColour(180, 80, 80), 1));
            else if (seg.service == "crossover")
                dc.SetPen(wxPen(wxColour(160, 100, 100), 1, wxPENSTYLE_DOT));
            else if (seg.service == "spur")
                dc.SetPen(wxPen(wxColour(160, 60, 60), 1));
            else if (seg.railway == "subway")
                dc.SetPen(wxPen(wxColour(0, 80, 200), 2, wxPENSTYLE_SHORT_DASH));
            else if (seg.railway == "tram" || seg.railway == "light_rail")
                dc.SetPen(wxPen(wxColour(200, 60, 60), 1));
            else if (seg.medium == 'U')
                dc.SetPen(wxPen(wxColour(120, 40, 40), 1, wxPENSTYLE_SHORT_DASH));
            else if (seg.medium == 'B')
                dc.SetPen(wxPen(wxColour(70, 130, 180), 2));
            else if (seg.railway == "disused" || seg.railway == "abandoned")
                dc.SetPen(wxPen(wxColour(150, 130, 130), 1, wxPENSTYLE_DOT));
            else
                dc.SetPen(wxPen(wxColour(160, 20, 20), 2));
        } else {
            // GML tracks — thicker, on top
            if (seg.medium == 'U')
                dc.SetPen(wxPen(wxColour(160, 20, 20), 3, wxPENSTYLE_SHORT_DASH));
            else if (seg.medium == 'L' || seg.medium == 'B')
                dc.SetPen(wxPen(wxColour(70, 130, 180), 3));
            else
                dc.SetPen(wxPen(wxColour(160, 20, 20), 3));
        }

        for (size_t i = 1; i < seg.points.size(); i++)
            dc.DrawLine(seg.points[i - 1], seg.points[i]);
    }

    // Rail points (switches, signals, crossings)
    for (const auto& rp : m_railPoints) {
        if (rp.type == "switch") {
            dc.SetPen(wxPen(wxColour(40, 40, 40), 1));
            dc.SetBrush(wxBrush(wxColour(255, 200, 0)));
            wxPoint diamond[4] = {
                {rp.pos.x, rp.pos.y - 4},
                {rp.pos.x + 4, rp.pos.y},
                {rp.pos.x, rp.pos.y + 4},
                {rp.pos.x - 4, rp.pos.y}
            };
            dc.DrawPolygon(4, diamond);
        } else if (rp.type == "signal") {
            dc.SetPen(wxPen(wxColour(20, 20, 20), 1));
            dc.SetBrush(wxBrush(wxColour(60, 200, 60)));
            dc.DrawCircle(rp.pos, 3);
        } else if (rp.type == "crossing" || rp.type == "level_crossing") {
            dc.SetPen(wxPen(wxColour(200, 20, 20), 2));
            dc.DrawLine(rp.pos.x - 4, rp.pos.y - 4,
                        rp.pos.x + 4, rp.pos.y + 4);
            dc.DrawLine(rp.pos.x + 4, rp.pos.y - 4,
                        rp.pos.x - 4, rp.pos.y + 4);
        } else if (rp.type == "station" || rp.type == "halt") {
            dc.SetPen(wxPen(*wxBLACK, 1));
            dc.SetBrush(wxBrush(wxColour(255, 220, 50)));
            dc.DrawCircle(rp.pos, 5);
            if (!rp.name.empty()) {
                dc.SetFont(wxFont(8, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                                   wxFONTWEIGHT_BOLD));
                dc.SetTextForeground(*wxBLACK);
                dc.DrawText(wxString::FromUTF8(rp.name),
                            rp.pos.x + 7, rp.pos.y - 5);
            }
        }
    }

    // GML stations (on top of everything)
    dc.SetFont(wxFont(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                       wxFONTWEIGHT_BOLD));
    for (const auto& st : m_stations) {
        dc.SetPen(wxPen(*wxBLACK, 1));
        dc.SetBrush(wxBrush(wxColour(255, 180, 50)));
        dc.DrawCircle(st.pos, 6);
        if (!st.name.empty()) {
            dc.SetTextForeground(*wxBLACK);
            dc.DrawText(wxString::FromUTF8(st.name),
                        st.pos.x + 8, st.pos.y - 6);
        }
    }

    // Center crosshair
    dc.SetPen(wxPen(wxColour(255, 0, 0, 128), 1, wxPENSTYLE_DOT));
    int cx = kPixels / 2, cy = kPixels / 2;
    dc.DrawLine(cx - 20, cy, cx + 20, cy);
    dc.DrawLine(cx, cy - 20, cx, cy + 20);
}

void LocalMapView::OnMouseMove(wxMouseEvent& event)
{
    int viewX, viewY;
    m_canvas->CalcUnscrolledPosition(event.GetX(), event.GetY(),
                                     &viewX, &viewY);

    // Convert pixel back to EPSG:25833
    double geoX = m_cx - kHalfSize + viewX * kScale;
    double geoY = m_cy + kHalfSize - viewY * kScale;

    SetStatusText(wxString::Format("%.0f, %.0f (EPSG:25833)", geoX, geoY), 1);

    // Show distance from center
    double dx = geoX - m_cx, dy = geoY - m_cy;
    double dist = std::sqrt(dx * dx + dy * dy);
    SetStatusText(wxString::Format("%.0f m from center", dist), 2);
}

#include "MapView.h"

#include <gdal_priv.h>

#include <wx/dcbuffer.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

MapView::MapView(wxWindow* parent, const wxString& title,
                 const std::string& vsiPath)
    : wxFrame(parent, wxID_ANY, title, wxDefaultPosition, wxSize(900, 700))
{
    m_canvas = new wxScrolledCanvas(this, wxID_ANY);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_canvas->Bind(wxEVT_PAINT,  &MapView::OnPaint,     this);
    m_canvas->Bind(wxEVT_MOTION, &MapView::OnMouseMove,  this);

    CreateStatusBar(3);
    SetStatusText("Loading...", 0);

    if (LoadTile(vsiPath)) {
        m_canvas->SetScrollbars(1, 1, m_rasterW, m_rasterH, 0, 0);
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
    src.importFromWkt(ds->GetProjectionRef());
    src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dst.SetWellKnownGeogCS("WGS84");
    dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_toWGS84 = OGRCreateCoordinateTransformation(&src, &dst);

    GDALClose(ds);

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

    // Render to bitmap
    wxImage img = RenderElevation();
    m_bitmap = wxBitmap(img);
    return true;
}

wxImage MapView::RenderElevation() const
{
    wxImage img(m_rasterW, m_rasterH);
    unsigned char* rgb = img.GetData();

    for (int i = 0; i < m_rasterW * m_rasterH; i++) {
        auto c = ElevToColor(m_elevation[i]);
        rgb[3 * i]     = c.r;
        rgb[3 * i + 1] = c.g;
        rgb[3 * i + 2] = c.b;
    }
    return img;
}

MapView::RGB MapView::ElevToColor(float elev) const
{
    // NoData → steel blue (water / ocean)
    if (m_hasNodata && elev == static_cast<float>(m_nodata))
        return {70, 130, 180};

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

    // Find the pair of stops surrounding t
    int hi = 1;
    while (hi < nStops - 1 && stops[hi].pos < t)
        ++hi;
    int lo = hi - 1;

    float frac = (t - stops[lo].pos) / (stops[hi].pos - stops[lo].pos);
    frac = std::clamp(frac, 0.0f, 1.0f);

    auto lerp = [](unsigned char a, unsigned char b, float f) -> unsigned char {
        return static_cast<unsigned char>(a + f * (b - a));
    };

    return {
        lerp(stops[lo].r, stops[hi].r, frac),
        lerp(stops[lo].g, stops[hi].g, frac),
        lerp(stops[lo].b, stops[hi].b, frac),
    };
}

void MapView::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_canvas);
    m_canvas->DoPrepareDC(dc);
    if (m_bitmap.IsOk())
        dc.DrawBitmap(m_bitmap, 0, 0, false);
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

    // Pixel coordinates
    SetStatusText(wxString::Format("Pixel (%d, %d)", viewX, viewY), 0);

    // Elevation
    float elev = m_elevation[static_cast<size_t>(viewY) * m_rasterW + viewX];
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
}

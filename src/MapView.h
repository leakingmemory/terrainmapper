#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>

#include <ogr_spatialref.h>

#include <string>
#include <vector>

class MapView : public wxFrame {
public:
    MapView(wxWindow* parent, const wxString& title,
            const std::string& vsiPath);
    ~MapView();

private:
    // Raster data
    std::vector<float> m_elevation;
    int m_rasterW = 0, m_rasterH = 0;
    double m_nodata = -32767;
    bool m_hasNodata = false;
    double m_gt[6] = {};
    float m_minElev = 0, m_maxElev = 0;

    // Display
    wxScrolledCanvas* m_canvas = nullptr;
    wxBitmap m_bitmap;

    // Coordinate transform (tile SRS → WGS84), owned
    OGRCoordinateTransformation* m_toWGS84 = nullptr;

    bool LoadTile(const std::string& vsiPath);
    wxImage RenderElevation() const;

    void OnPaint(wxPaintEvent& event);
    void OnMouseMove(wxMouseEvent& event);

    struct RGB { unsigned char r, g, b; };
    RGB ElevToColor(float elev) const;
};

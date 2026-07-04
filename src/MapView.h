#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <wx/progdlg.h>

#include <ogr_spatialref.h>

#include <cstdint>
#include <string>
#include <vector>

class MapView : public wxFrame {
public:
    MapView(wxWindow* parent, const wxString& title,
            const std::string& vsiPath,
            const std::string& ar50Path = "",
            const std::string& railwayPath = "",
            const std::string& roadsPath = "");
    ~MapView();

private:
    // Raster data
    std::vector<float> m_elevation;
    int m_rasterW = 0, m_rasterH = 0;
    double m_nodata = -32767;
    bool m_hasNodata = false;
    double m_gt[6] = {};
    float m_minElev = 0, m_maxElev = 0;
    std::string m_tileProjectionWkt;

    // Land cover (artype per pixel, 0 = unknown)
    std::vector<int32_t> m_landcover;
    bool m_hasLandcover = false;

    // Railway overlay
    struct RailwaySegment {
        std::vector<wxPoint> points;
        std::string name;
    };
    struct RailwayStation {
        wxPoint pos;
        std::string name;
        bool active = true;  // based on Jernbanestatus
    };
    std::vector<RailwaySegment> m_railSegments;
    std::vector<RailwayStation> m_railStations;
    bool m_hasRailway = false;

    // Road overlay
    struct RoadSegment {
        std::vector<wxPoint> points;
        char category = 'F';  // E, R, or F
        int roadNumber = 0;
    };
    std::vector<RoadSegment> m_roadSegments;
    bool m_hasRoads = false;

    // Display
    wxScrolledCanvas* m_canvas = nullptr;
    wxBitmap m_bitmap;

    // Coordinate transform (tile SRS → WGS84), owned
    OGRCoordinateTransformation* m_toWGS84 = nullptr;

    // Inverse geotransform for coordinate → pixel conversion
    double m_invGt[6] = {};

    bool LoadTile(const std::string& vsiPath);
    bool LoadLandCover(const std::string& ar50Path,
                       wxProgressDialog* progress = nullptr);
    bool LoadRailway(const std::string& railwayPath);
    bool LoadRoads(const std::string& roadsPath);
    wxImage RenderElevation() const;

    void OnPaint(wxPaintEvent& event);
    void OnMouseMove(wxMouseEvent& event);

    struct RGB { unsigned char r, g, b; };
    RGB ElevToColor(float elev, int32_t artype) const;
    static RGB LandCoverColor(int32_t artype);
    static const char* LandCoverName(int32_t artype);
};

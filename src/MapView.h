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
            const std::string& roadsPath = "",
            const std::string& osmDataPath = "");
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
        char status = 'I';   // I=operational, N=closed, P=planned, M=museum, F=construction
        char medium = ' ';   // ' '=surface, U=underground, L/B=bridge, T=metro
    };
    struct RailwayStation {
        wxPoint pos;
        std::string name;
        char type = 'S';     // S=station, I=interchange
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

    // Building overlay (from OSM)
    struct BuildingOutline {
        std::vector<std::vector<wxPoint>> rings;  // outer ring + holes
        std::string type;     // building type (house, apartments, etc.)
        std::string name;
        int levels = 0;
    };
    std::vector<BuildingOutline> m_buildings;
    bool m_hasBuildings = false;

    // OSM railway track overlay (yard tracks, sidings, platforms, switches)
    struct OsmTrackSegment {
        std::vector<wxPoint> points;
        std::string railway;   // rail, subway, tram, etc.
        std::string service;   // yard, siding, crossover, spur, or empty=main
        std::string name;
        bool tunnel = false;
        bool bridge = false;
    };
    struct OsmRailPoint {
        wxPoint pos;
        std::string railway;   // switch, signal, station, halt, crossing, etc.
        std::string name;
    };
    struct OsmPlatform {
        std::vector<std::vector<wxPoint>> rings;  // polygon rings (or single-ring from line)
        std::string name;
        std::string ref;
        bool isLine = false;   // true if sourced from a line (draw as line, not polygon)
        std::vector<wxPoint> linePoints;  // if isLine
    };
    std::vector<OsmTrackSegment> m_osmTracks;
    std::vector<OsmRailPoint> m_osmRailPoints;
    std::vector<OsmPlatform> m_osmPlatforms;
    bool m_hasOsmRailway = false;

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
    bool LoadBuildings(const std::string& osmDataPath);
    bool LoadOsmRailway(const std::string& osmDataPath);
    wxImage RenderElevation() const;

    void OnPaint(wxPaintEvent& event);
    void OnMouseMove(wxMouseEvent& event);

    struct RGB { unsigned char r, g, b; };
    RGB ElevToColor(float elev, int32_t artype) const;
    static RGB LandCoverColor(int32_t artype);
    static const char* LandCoverName(int32_t artype);
};

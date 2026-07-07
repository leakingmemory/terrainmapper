#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>

#include <cstdint>
#include <string>
#include <vector>

class ProfileData;

class LocalMapView : public wxFrame {
public:
    // centerX/Y in EPSG:25833
    LocalMapView(wxWindow* parent,
                 const wxString& title,
                 double centerX, double centerY,
                 const ProfileData& profileData,
                 const std::string& railwayPath,
                 const std::string& roadsPath,
                 const std::string& osmDataPath);

private:
    // Area geometry
    double m_cx, m_cy;           // center in EPSG:25833
    static constexpr double kHalfSize = 2000.0;  // 4km × 4km
    static constexpr double kScale = 2.0;         // metres per pixel
    static constexpr int kPixels = 2000;           // 4000m / 2m

    // Elevation background
    wxBitmap m_bitmap;

    // Vector overlays
    struct TrackSeg {
        std::vector<wxPoint> points;
        std::string railway;   // Banelenke line type or OSM railway value
        std::string service;   // OSM: yard, siding, spur, crossover
        char medium = ' ';     // GML: U=tunnel, L/B=bridge
        bool isOsm = false;
    };
    struct Station {
        wxPoint pos;
        std::string name;
    };
    struct Platform {
        std::vector<wxPoint> ring;      // polygon outline
        std::vector<wxPoint> linePoints; // line geometry
        bool isLine = false;
        std::string name;
        std::string ref;
    };
    struct RailPoint {
        wxPoint pos;
        std::string type;  // switch, signal, crossing, etc.
        std::string name;
    };
    struct RoadSeg {
        std::vector<wxPoint> points;
        char category = 'F';
        int roadNumber = 0;
    };

    std::vector<TrackSeg> m_tracks;
    std::vector<Station> m_stations;
    std::vector<Platform> m_platforms;
    std::vector<RailPoint> m_railPoints;
    std::vector<RoadSeg> m_roads;

    wxScrolledCanvas* m_canvas = nullptr;

    // Coordinate conversion: EPSG:25833 → pixel
    wxPoint GeoToPixel(double x, double y) const;

    void BuildElevationBitmap(const ProfileData& profileData);
    void LoadGmlRailway(const std::string& railwayPath);
    void LoadRoads(const std::string& roadsPath);
    void LoadOsmData(const std::string& osmDataPath);

    void OnPaint(wxPaintEvent& event);
    void OnMouseMove(wxMouseEvent& event);
};

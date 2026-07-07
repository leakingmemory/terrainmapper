#pragma once

#include "ProfileData.h"
#include "RoadProfileData.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>

#include <string>
#include <vector>

class ProfileView : public wxFrame {
public:
    ProfileView(wxWindow* parent,
                const std::string& railwayPath,
                const std::string& roadsPath,
                const std::vector<std::string>& zipPaths,
                const std::string& osmDataPath = "");

private:
    enum class Mode { Railway, Road };
    Mode m_mode = Mode::Railway;

    ProfileData m_profileData;
    RoadProfileData m_roadProfileData;
    std::string m_railwayPath;
    std::string m_roadsPath;
    std::string m_osmDataPath;

    // Railway data
    std::vector<std::string> m_lineNames;
    ProfileResult m_railResult;

    // Road data
    std::vector<RoadId> m_roadList;
    RoadProfileResult m_roadResult;

    bool m_hasProfile = false;
    bool m_indexReady = false;

    // UI elements
    wxChoice* m_modeChoice = nullptr;
    wxChoice* m_roadCatChoice = nullptr;   // road category filter (E/R/F/K/P)
    wxTextCtrl* m_searchCtrl = nullptr;    // search box for F/K/P categories
    wxChoice* m_lineChoice = nullptr;
    wxButton* m_showBtn = nullptr;
    wxSplitterWindow* m_splitter = nullptr;
    wxScrolledCanvas* m_canvas = nullptr;
    wxListCtrl* m_junctionList = nullptr;
    wxPanel* m_statsPanel = nullptr;

    // Stats labels
    wxStaticText* m_lblLength = nullptr;
    wxStaticText* m_lblElevRange = nullptr;
    wxStaticText* m_lblClimb = nullptr;
    wxStaticText* m_lblMaxGrade = nullptr;
    wxStaticText* m_lblExtra1 = nullptr;       // tunnel/rail crossings
    wxStaticText* m_lblExtra2 = nullptr;       // bridge/road crossings
    wxStaticText* m_lblStations = nullptr;
    wxStaticText* m_lblSegments = nullptr;

    // Extra stat labels (the label text itself, not the value)
    wxStaticText* m_lblExtra1Label = nullptr;
    wxStaticText* m_lblExtra2Label = nullptr;
    wxStaticText* m_lblStationsLabel = nullptr;

    // Diagram layout constants
    static constexpr int kMarginLeft = 70;
    static constexpr int kMarginRight = 30;
    static constexpr int kMarginTop = 20;
    static constexpr int kMarginBottom = 60;
    static constexpr int kDiagramHeight = 450;
    static constexpr int kPixelsPerKm = 8;

    // Current mouse position in km (for crosshair)
    double m_mouseKm = -1;
    double m_mouseElev = -1;

    void OnModeChanged(wxCommandEvent& event);
    void OnRoadCatChanged(wxCommandEvent& event);
    void OnSearchText(wxCommandEvent& event);
    void OnShowProfile(wxCommandEvent& event);
    void OnJunctionActivated(wxListEvent& event);
    void OnPaint(wxPaintEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void UpdateStats();
    void PopulateLineChoice();
    void PopulateJunctionList();

    // Current profile points (shared between modes)
    const std::vector<ProfilePoint>& CurrentPoints() const;

    // Coordinate conversion helpers for diagram
    int KmToX(double km) const;
    int ElevToY(double elev) const;
    double XToKm(int x) const;
    double YToElev(int y) const;

    double m_kmMin = 0, m_kmMax = 0;
    double m_elevMin = 0, m_elevMax = 0;
    int m_diagramW = 800;
};

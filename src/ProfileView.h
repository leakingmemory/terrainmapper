#pragma once

#include "ProfileData.h"

#include <wx/wx.h>
#include <wx/scrolwin.h>

#include <string>
#include <vector>

class ProfileView : public wxFrame {
public:
    ProfileView(wxWindow* parent,
                const std::string& railwayPath,
                const std::vector<std::string>& zipPaths);

private:
    ProfileData m_profileData;
    std::string m_railwayPath;
    std::vector<std::string> m_lineNames;
    ProfileResult m_result;
    bool m_hasProfile = false;
    bool m_indexReady = false;

    // UI elements
    wxChoice* m_lineChoice = nullptr;
    wxButton* m_showBtn = nullptr;
    wxScrolledCanvas* m_canvas = nullptr;
    wxPanel* m_statsPanel = nullptr;

    // Stats labels
    wxStaticText* m_lblLength = nullptr;
    wxStaticText* m_lblElevRange = nullptr;
    wxStaticText* m_lblClimb = nullptr;
    wxStaticText* m_lblMaxGrade = nullptr;
    wxStaticText* m_lblTunnel = nullptr;
    wxStaticText* m_lblBridge = nullptr;
    wxStaticText* m_lblStations = nullptr;
    wxStaticText* m_lblSegments = nullptr;

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

    void OnShowProfile(wxCommandEvent& event);
    void OnPaint(wxPaintEvent& event);
    void OnMouseMove(wxMouseEvent& event);
    void UpdateStats();

    // Coordinate conversion helpers for diagram
    int KmToX(double km) const;
    int ElevToY(double elev) const;
    double XToKm(int x) const;
    double YToElev(int y) const;

    double m_kmMin = 0, m_kmMax = 0;
    double m_elevMin = 0, m_elevMax = 0;
    int m_diagramW = 800;
};

#include "ProfileView.h"

#include <wx/dcbuffer.h>
#include <wx/progdlg.h>

#include <algorithm>
#include <cmath>

ProfileView::ProfileView(wxWindow* parent,
                         const std::string& railwayPath,
                         const std::vector<std::string>& zipPaths)
    : wxFrame(parent, wxID_ANY, "Railway Elevation Profiles",
              wxDefaultPosition, wxSize(1100, 700))
    , m_railwayPath(railwayPath)
{
    // --- Build UI ---
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Top bar: line selector + button
    auto* topSizer = new wxBoxSizer(wxHORIZONTAL);
    topSizer->Add(new wxStaticText(this, wxID_ANY, "Railway line:"),
                  0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
    m_lineChoice = new wxChoice(this, wxID_ANY);
    topSizer->Add(m_lineChoice, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    m_showBtn = new wxButton(this, wxID_ANY, "Show Profile");
    m_showBtn->Enable(false);
    topSizer->Add(m_showBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    mainSizer->Add(topSizer, 0, wxEXPAND | wxALL, 5);

    // Center: split between diagram and stats
    auto* centerSizer = new wxBoxSizer(wxHORIZONTAL);

    // Diagram canvas (scrollable)
    m_canvas = new wxScrolledCanvas(this, wxID_ANY);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_canvas->SetBackgroundColour(*wxWHITE);
    centerSizer->Add(m_canvas, 1, wxEXPAND);

    // Stats panel on the right
    m_statsPanel = new wxPanel(this, wxID_ANY);
    auto* statsSizer = new wxStaticBoxSizer(wxVERTICAL, m_statsPanel, "Key Data");

    auto addStat = [&](const wxString& label) -> wxStaticText* {
        auto* lbl = new wxStaticText(m_statsPanel, wxID_ANY, label);
        statsSizer->Add(lbl, 0, wxLEFT | wxRIGHT | wxTOP, 5);
        auto* val = new wxStaticText(m_statsPanel, wxID_ANY, "--");
        statsSizer->Add(val, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);
        return val;
    };

    m_lblLength = addStat("Total length:");
    m_lblElevRange = addStat("Elevation range:");
    m_lblClimb = addStat("Total climb / descent:");
    m_lblMaxGrade = addStat("Max gradient:");
    m_lblTunnel = addStat("Tunnel length:");
    m_lblBridge = addStat("Bridge length:");
    m_lblStations = addStat("Stations:");
    m_lblSegments = addStat("Track segments:");

    m_statsPanel->SetSizer(statsSizer);
    centerSizer->Add(m_statsPanel, 0, wxEXPAND | wxLEFT, 5);
    mainSizer->Add(centerSizer, 1, wxEXPAND | wxALL, 5);

    SetSizer(mainSizer);

    // Status bar with 4 fields
    CreateStatusBar(4);
    SetStatusText("Building tile index...", 0);

    // Event bindings
    m_showBtn->Bind(wxEVT_BUTTON, &ProfileView::OnShowProfile, this);
    m_canvas->Bind(wxEVT_PAINT, &ProfileView::OnPaint, this);
    m_canvas->Bind(wxEVT_MOTION, &ProfileView::OnMouseMove, this);

    // Build tile index (with progress)
    wxProgressDialog progress(
        "Building Tile Index",
        "Scanning DTM tiles...",
        100, this,
        wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_SMOOTH |
        wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME);

    int tilesIndexed = m_profileData.BuildTileIndex(zipPaths,
        [&](int current, int total) -> bool {
            if (total > 0) {
                int pct = current * 100 / total;
                progress.Update(pct, wxString::Format(
                    "Scanning tile %d / %d...", current + 1, total));
            }
            return true;
        });
    progress.Update(100);
    m_indexReady = true;

    // Get line names
    m_lineNames = m_profileData.GetLineNames(railwayPath);
    for (const auto& name : m_lineNames)
        m_lineChoice->Append(wxString::FromUTF8(name));

    if (!m_lineNames.empty()) {
        m_lineChoice->SetSelection(0);
        m_showBtn->Enable(true);
    }

    SetStatusText(wxString::Format("Ready — %zu railway lines, %d tiles indexed",
                                   m_lineNames.size(), tilesIndexed), 0);
}

void ProfileView::OnShowProfile(wxCommandEvent&)
{
    int sel = m_lineChoice->GetSelection();
    if (sel == wxNOT_FOUND || sel >= static_cast<int>(m_lineNames.size()))
        return;

    const std::string& lineName = m_lineNames[sel];
    SetStatusText(wxString::Format("Building profile for %s...", lineName), 0);
    wxBeginBusyCursor();

    m_result = m_profileData.BuildProfile(m_railwayPath, lineName);

    wxEndBusyCursor();

    if (m_result.points.empty()) {
        SetStatusText("No elevation data found for this line.", 0);
        m_hasProfile = false;
        m_canvas->Refresh();
        return;
    }

    m_hasProfile = true;

    // Compute diagram extents
    m_kmMin = m_result.points.front().km;
    m_kmMax = m_result.points.back().km;
    m_elevMin = 1e9;
    m_elevMax = -1e9;
    for (const auto& pt : m_result.points) {
        if (pt.elevation > -9000) {
            m_elevMin = std::min(m_elevMin, pt.elevation);
            m_elevMax = std::max(m_elevMax, pt.elevation);
        }
    }

    // Add padding to elevation range
    double elevPad = (m_elevMax - m_elevMin) * 0.1;
    if (elevPad < 20) elevPad = 20;
    m_elevMin = std::max(0.0, m_elevMin - elevPad);
    m_elevMax += elevPad;

    // Round to nice values
    m_elevMin = std::floor(m_elevMin / 50) * 50;
    m_elevMax = std::ceil(m_elevMax / 50) * 50;

    // Set diagram width based on line length
    double lengthKm = m_kmMax - m_kmMin;
    m_diagramW = kMarginLeft + kMarginRight +
                 std::max(800, static_cast<int>(lengthKm * kPixelsPerKm));

    m_canvas->SetScrollbars(1, 1, m_diagramW,
                            kMarginTop + kDiagramHeight + kMarginBottom,
                            0, 0);

    UpdateStats();
    m_canvas->Refresh();

    SetStatusText(wxString::Format("%s — %.1f km, %d points",
                                   lineName,
                                   m_result.stats.totalLengthKm,
                                   static_cast<int>(m_result.points.size())), 0);
}

// ─── Coordinate conversion ──────────────────────────────────────────

int ProfileView::KmToX(double km) const
{
    double range = m_kmMax - m_kmMin;
    if (range <= 0) range = 1;
    int plotW = m_diagramW - kMarginLeft - kMarginRight;
    return kMarginLeft + static_cast<int>((km - m_kmMin) / range * plotW);
}

int ProfileView::ElevToY(double elev) const
{
    double range = m_elevMax - m_elevMin;
    if (range <= 0) range = 1;
    // Y increases downward, higher elevation → lower Y
    return kMarginTop + static_cast<int>(
        (1.0 - (elev - m_elevMin) / range) * kDiagramHeight);
}

double ProfileView::XToKm(int x) const
{
    int plotW = m_diagramW - kMarginLeft - kMarginRight;
    if (plotW <= 0) return m_kmMin;
    return m_kmMin + static_cast<double>(x - kMarginLeft) / plotW * (m_kmMax - m_kmMin);
}

double ProfileView::YToElev(int y) const
{
    if (kDiagramHeight <= 0) return m_elevMin;
    double f = 1.0 - static_cast<double>(y - kMarginTop) / kDiagramHeight;
    return m_elevMin + f * (m_elevMax - m_elevMin);
}

// ─── Diagram painting ───────────────────────────────────────────────

void ProfileView::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(m_canvas);
    m_canvas->DoPrepareDC(dc);

    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();

    if (!m_hasProfile || m_result.points.empty())
        return;

    int plotW = m_diagramW - kMarginLeft - kMarginRight;
    int totalH = kMarginTop + kDiagramHeight + kMarginBottom;

    // --- Grid lines ---
    dc.SetPen(wxPen(wxColour(220, 220, 220), 1));
    dc.SetFont(wxFont(8, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                       wxFONTWEIGHT_NORMAL));

    // Horizontal gridlines (elevation)
    double elevStep = 100;
    double elevRange = m_elevMax - m_elevMin;
    if (elevRange > 2000) elevStep = 500;
    else if (elevRange > 1000) elevStep = 200;
    else if (elevRange > 500) elevStep = 100;
    else elevStep = 50;

    dc.SetTextForeground(wxColour(100, 100, 100));
    for (double e = std::ceil(m_elevMin / elevStep) * elevStep;
         e <= m_elevMax; e += elevStep) {
        int y = ElevToY(e);
        dc.DrawLine(kMarginLeft, y, kMarginLeft + plotW, y);
        dc.DrawText(wxString::Format("%.0f m", e),
                    5, y - 7);
    }

    // Vertical gridlines (km)
    double kmRange = m_kmMax - m_kmMin;
    double kmStep = 10;
    if (kmRange > 500) kmStep = 50;
    else if (kmRange > 200) kmStep = 20;
    else if (kmRange > 100) kmStep = 10;
    else if (kmRange > 50) kmStep = 5;
    else kmStep = 1;

    for (double k = std::ceil(m_kmMin / kmStep) * kmStep;
         k <= m_kmMax; k += kmStep) {
        int x = KmToX(k);
        dc.DrawLine(x, kMarginTop, x, kMarginTop + kDiagramHeight);
        dc.DrawText(wxString::Format("%.0f", k),
                    x - 10, kMarginTop + kDiagramHeight + 5);
    }

    // X-axis label
    dc.DrawText("km", kMarginLeft + plotW + 5,
                kMarginTop + kDiagramHeight + 5);

    // --- Axes ---
    dc.SetPen(wxPen(*wxBLACK, 1));
    dc.DrawLine(kMarginLeft, kMarginTop,
                kMarginLeft, kMarginTop + kDiagramHeight);
    dc.DrawLine(kMarginLeft, kMarginTop + kDiagramHeight,
                kMarginLeft + plotW, kMarginTop + kDiagramHeight);

    // --- Profile fill ---
    // Build polygon points for surface and tunnel sections
    int baseY = ElevToY(m_elevMin);

    // Separate surface and tunnel points for different fill colors
    // First pass: draw filled polygon for the entire profile
    {
        // Collect all points for the full profile polygon
        std::vector<wxPoint> polyPts;
        polyPts.push_back({KmToX(m_result.points.front().km), baseY});

        for (const auto& pt : m_result.points) {
            if (pt.elevation > -9000)
                polyPts.push_back({KmToX(pt.km), ElevToY(pt.elevation)});
        }
        polyPts.push_back({KmToX(m_result.points.back().km), baseY});

        // Light green fill for surface
        dc.SetBrush(wxBrush(wxColour(200, 230, 200)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawPolygon(static_cast<int>(polyPts.size()), polyPts.data());
    }

    // Overlay tunnel sections with grey-brown fill
    {
        size_t i = 0;
        while (i < m_result.points.size()) {
            if (m_result.points[i].medium != 'U' ||
                m_result.points[i].elevation < -9000) {
                i++;
                continue;
            }

            std::vector<wxPoint> tunnelPoly;
            tunnelPoly.push_back({KmToX(m_result.points[i].km), baseY});

            while (i < m_result.points.size() &&
                   m_result.points[i].medium == 'U' &&
                   m_result.points[i].elevation > -9000) {
                tunnelPoly.push_back({KmToX(m_result.points[i].km),
                                      ElevToY(m_result.points[i].elevation)});
                i++;
            }

            tunnelPoly.push_back({KmToX(m_result.points[i > 0 ? i - 1 : 0].km),
                                  baseY});

            dc.SetBrush(wxBrush(wxColour(180, 170, 150)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawPolygon(static_cast<int>(tunnelPoly.size()),
                           tunnelPoly.data());
        }
    }

    // --- DTM surface line over tunnels (faint dotted) ---
    // For tunnel sections, show what the mountain surface looks like
    dc.SetPen(wxPen(wxColour(180, 180, 180), 1, wxPENSTYLE_DOT));
    for (size_t i = 1; i < m_result.points.size(); i++) {
        if (m_result.points[i].interpolated &&
            m_result.points[i - 1].interpolated) {
            // For interpolated tunnel points, we don't have the raw DTM value
            // stored separately, so we skip this
        }
    }

    // --- Profile line ---
    for (size_t i = 1; i < m_result.points.size(); i++) {
        const auto& prev = m_result.points[i - 1];
        const auto& cur = m_result.points[i];
        if (prev.elevation < -9000 || cur.elevation < -9000) continue;

        // Skip large gaps
        if (cur.km - prev.km > 1.0) continue;

        // Color by type
        if (cur.medium == 'U') {
            dc.SetPen(wxPen(wxColour(120, 100, 80), 2));
        } else if (cur.medium == 'L' || cur.medium == 'B') {
            dc.SetPen(wxPen(wxColour(70, 130, 180), 2));  // steel blue for bridges
        } else {
            dc.SetPen(wxPen(wxColour(40, 120, 40), 2));   // green for surface
        }

        dc.DrawLine(KmToX(prev.km), ElevToY(prev.elevation),
                     KmToX(cur.km), ElevToY(cur.elevation));
    }

    // --- Station markers ---
    dc.SetFont(wxFont(7, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                       wxFONTWEIGHT_NORMAL));

    for (const auto& st : m_result.stations) {
        int x = KmToX(st.km);

        // Find elevation at this km from profile
        double elev = st.elevation;
        if (elev < -9000) continue;

        int y = ElevToY(elev);

        // Vertical tick
        dc.SetPen(wxPen(wxColour(100, 100, 100), 1, wxPENSTYLE_DOT));
        dc.DrawLine(x, y, x, kMarginTop + kDiagramHeight);

        // Station dot
        if (st.type == 'I') {
            dc.SetPen(wxPen(*wxBLACK, 1));
            dc.SetBrush(wxBrush(wxColour(255, 180, 50)));
            dc.DrawCircle(x, y, 4);
        } else {
            dc.SetPen(wxPen(*wxBLACK, 1));
            dc.SetBrush(wxBrush(wxColour(255, 50, 50)));
            dc.DrawCircle(x, y, 3);
        }

        // Rotated station name
        if (!st.name.empty()) {
            dc.SetTextForeground(wxColour(60, 60, 60));
            dc.DrawRotatedText(wxString::FromUTF8(st.name),
                               x + 3, kMarginTop + kDiagramHeight + 3,
                               45.0);
        }
    }

    // --- Crosshair ---
    if (m_mouseKm >= m_kmMin && m_mouseKm <= m_kmMax) {
        int cx = KmToX(m_mouseKm);
        dc.SetPen(wxPen(wxColour(255, 0, 0, 128), 1, wxPENSTYLE_LONG_DASH));
        dc.DrawLine(cx, kMarginTop, cx, kMarginTop + kDiagramHeight);
    }
}

// ─── Mouse tracking ─────────────────────────────────────────────────

void ProfileView::OnMouseMove(wxMouseEvent& event)
{
    if (!m_hasProfile) return;

    int viewX, viewY;
    m_canvas->CalcUnscrolledPosition(event.GetX(), event.GetY(),
                                      &viewX, &viewY);

    double km = XToKm(viewX);
    double elev = YToElev(viewY);

    m_mouseKm = km;
    m_mouseElev = elev;
    m_canvas->Refresh();

    // Status bar: km, elevation at cursor, medium, find closest point
    SetStatusText(wxString::Format("km: %.1f", km), 0);

    // Find closest profile point for actual elevation
    double bestDist = 1e9;
    const ProfilePoint* closest = nullptr;
    for (const auto& pt : m_result.points) {
        double d = std::abs(pt.km - km);
        if (d < bestDist) {
            bestDist = d;
            closest = &pt;
        }
    }

    if (closest && closest->elevation > -9000) {
        SetStatusText(wxString::Format("Elevation: %.0f m", closest->elevation), 1);

        const char* medName = "Surface";
        if (closest->medium == 'U') medName = "Tunnel";
        else if (closest->medium == 'L' || closest->medium == 'B') medName = "Bridge";
        SetStatusText(medName, 2);

        // Find nearest station
        double bestStDist = 1e9;
        const ProfileStation* nearestSt = nullptr;
        for (const auto& st : m_result.stations) {
            double d = std::abs(st.km - km);
            if (d < bestStDist) {
                bestStDist = d;
                nearestSt = &st;
            }
        }
        if (nearestSt && bestStDist < 5.0) {
            SetStatusText(wxString::Format("Near: %s (%.1f km)",
                                           nearestSt->name, nearestSt->km), 3);
        } else {
            SetStatusText("", 3);
        }
    }
}

// ─── Stats panel update ─────────────────────────────────────────────

void ProfileView::UpdateStats()
{
    if (!m_hasProfile) return;

    const auto& s = m_result.stats;

    m_lblLength->SetLabel(wxString::Format("%.1f km", s.totalLengthKm));
    m_lblElevRange->SetLabel(wxString::Format("%.0f – %.0f m (%.0f m)",
                                               s.minElev, s.maxElev,
                                               s.maxElev - s.minElev));
    m_lblClimb->SetLabel(wxString::Format("%.0f m / %.0f m",
                                           s.totalClimb, s.totalDescent));
    m_lblMaxGrade->SetLabel(wxString::Format("%.1f %%", s.maxGradePct));
    m_lblTunnel->SetLabel(wxString::Format("%.1f km (%.1f%%)",
                                            s.tunnelLengthKm,
                                            s.totalLengthKm > 0 ?
                                            s.tunnelLengthKm / s.totalLengthKm * 100 : 0));
    m_lblBridge->SetLabel(wxString::Format("%.1f km", s.bridgeLengthKm));
    m_lblStations->SetLabel(wxString::Format("%d", s.stationCount));
    m_lblSegments->SetLabel(wxString::Format("%d", s.segmentCount));

    m_statsPanel->Layout();
}

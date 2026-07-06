#include "ProfileView.h"

#include <wx/dcbuffer.h>
#include <wx/progdlg.h>

#include <algorithm>
#include <cmath>

ProfileView::ProfileView(wxWindow* parent,
                         const std::string& railwayPath,
                         const std::string& roadsPath,
                         const std::vector<std::string>& zipPaths)
    : wxFrame(parent, wxID_ANY, "Elevation Profiles",
              wxDefaultPosition, wxSize(1100, 700))
    , m_railwayPath(railwayPath)
    , m_roadsPath(roadsPath)
{
    // --- Build UI ---
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Top bar: mode selector + line/road selector + button
    auto* topSizer = new wxBoxSizer(wxHORIZONTAL);

    topSizer->Add(new wxStaticText(this, wxID_ANY, "Mode:"),
                  0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 5);
    m_modeChoice = new wxChoice(this, wxID_ANY);
    if (!railwayPath.empty())
        m_modeChoice->Append("Railway");
    if (!roadsPath.empty())
        m_modeChoice->Append("Road");
    if (m_modeChoice->GetCount() > 0)
        m_modeChoice->SetSelection(0);
    topSizer->Add(m_modeChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    topSizer->Add(new wxStaticText(this, wxID_ANY, "Select:"),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    m_lineChoice = new wxChoice(this, wxID_ANY);
    topSizer->Add(m_lineChoice, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

    m_showBtn = new wxButton(this, wxID_ANY, "Show Profile");
    m_showBtn->Enable(false);
    topSizer->Add(m_showBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    mainSizer->Add(topSizer, 0, wxEXPAND | wxALL, 5);

    // Center: split between diagram and stats
    auto* centerSizer = new wxBoxSizer(wxHORIZONTAL);

    m_canvas = new wxScrolledCanvas(this, wxID_ANY);
    m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_canvas->SetBackgroundColour(*wxWHITE);
    centerSizer->Add(m_canvas, 1, wxEXPAND);

    // Stats panel
    m_statsPanel = new wxPanel(this, wxID_ANY);
    auto* statsSizer = new wxStaticBoxSizer(wxVERTICAL, m_statsPanel, "Key Data");

    auto addStat = [&](const wxString& label, wxStaticText** labelOut = nullptr)
        -> wxStaticText* {
        auto* lbl = new wxStaticText(m_statsPanel, wxID_ANY, label);
        statsSizer->Add(lbl, 0, wxLEFT | wxRIGHT | wxTOP, 5);
        if (labelOut) *labelOut = lbl;
        auto* val = new wxStaticText(m_statsPanel, wxID_ANY, "--");
        statsSizer->Add(val, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);
        return val;
    };

    m_lblLength = addStat("Total length:");
    m_lblElevRange = addStat("Elevation range:");
    m_lblClimb = addStat("Total climb / descent:");
    m_lblMaxGrade = addStat("Max gradient:");
    m_lblExtra1 = addStat("Tunnel length:", &m_lblExtra1Label);
    m_lblExtra2 = addStat("Bridge length:", &m_lblExtra2Label);
    m_lblStations = addStat("Stations:", &m_lblStationsLabel);
    m_lblSegments = addStat("Segments:");

    m_statsPanel->SetSizer(statsSizer);
    centerSizer->Add(m_statsPanel, 0, wxEXPAND | wxLEFT, 5);
    mainSizer->Add(centerSizer, 1, wxEXPAND | wxALL, 5);

    SetSizer(mainSizer);

    CreateStatusBar(4);
    SetStatusText("Building tile index...", 0);

    // Event bindings
    m_modeChoice->Bind(wxEVT_CHOICE, &ProfileView::OnModeChanged, this);
    m_showBtn->Bind(wxEVT_BUTTON, &ProfileView::OnShowProfile, this);
    m_canvas->Bind(wxEVT_PAINT, &ProfileView::OnPaint, this);
    m_canvas->Bind(wxEVT_MOTION, &ProfileView::OnMouseMove, this);

    // Build tile index
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

    // Load line names and road list
    if (!railwayPath.empty())
        m_lineNames = m_profileData.GetLineNames(railwayPath);
    if (!roadsPath.empty())
        m_roadList = m_roadProfileData.GetRoadList(roadsPath);

    // Determine initial mode
    if (!railwayPath.empty())
        m_mode = Mode::Railway;
    else
        m_mode = Mode::Road;

    PopulateLineChoice();

    SetStatusText(wxString::Format("Ready — %zu railways, %zu roads, %d tiles",
                                   m_lineNames.size(), m_roadList.size(),
                                   tilesIndexed), 0);
}

void ProfileView::OnModeChanged(wxCommandEvent&)
{
    wxString sel = m_modeChoice->GetStringSelection();
    if (sel == "Railway")
        m_mode = Mode::Railway;
    else
        m_mode = Mode::Road;

    m_hasProfile = false;
    PopulateLineChoice();
    m_canvas->Refresh();
}

void ProfileView::PopulateLineChoice()
{
    m_lineChoice->Clear();

    if (m_mode == Mode::Railway) {
        for (const auto& name : m_lineNames)
            m_lineChoice->Append(wxString::FromUTF8(name));
        // Update stat labels for railway
        if (m_lblExtra1Label) m_lblExtra1Label->SetLabel("Tunnel length:");
        if (m_lblExtra2Label) m_lblExtra2Label->SetLabel("Bridge length:");
        if (m_lblStationsLabel) m_lblStationsLabel->SetLabel("Stations:");
    } else {
        for (const auto& road : m_roadList)
            m_lineChoice->Append(wxString::FromUTF8(road.Label()));
        // Update stat labels for road
        if (m_lblExtra1Label) m_lblExtra1Label->SetLabel("Rail crossings:");
        if (m_lblExtra2Label) m_lblExtra2Label->SetLabel("Road crossings:");
        if (m_lblStationsLabel) m_lblStationsLabel->SetLabel("Total crossings:");
    }

    if (m_lineChoice->GetCount() > 0) {
        m_lineChoice->SetSelection(0);
        m_showBtn->Enable(true);
    } else {
        m_showBtn->Enable(false);
    }
}

const std::vector<ProfilePoint>& ProfileView::CurrentPoints() const
{
    if (m_mode == Mode::Railway)
        return m_railResult.points;
    else
        return m_roadResult.points;
}

void ProfileView::OnShowProfile(wxCommandEvent&)
{
    int sel = m_lineChoice->GetSelection();
    if (sel == wxNOT_FOUND) return;

    wxBeginBusyCursor();

    if (m_mode == Mode::Railway) {
        if (sel >= static_cast<int>(m_lineNames.size())) {
            wxEndBusyCursor();
            return;
        }
        const std::string& lineName = m_lineNames[sel];
        SetStatusText(wxString::Format("Building profile for %s...", lineName), 0);

        m_railResult = m_profileData.BuildProfile(m_railwayPath, lineName);

        if (m_railResult.points.empty()) {
            wxEndBusyCursor();
            SetStatusText("No elevation data found.", 0);
            m_hasProfile = false;
            m_canvas->Refresh();
            return;
        }
    } else {
        if (sel >= static_cast<int>(m_roadList.size())) {
            wxEndBusyCursor();
            return;
        }
        const auto& road = m_roadList[sel];
        SetStatusText(wxString::Format("Building profile for %s...", road.Label()), 0);

        m_roadResult = m_roadProfileData.BuildRoadProfile(
            m_roadsPath, m_railwayPath, road, m_profileData);

        if (m_roadResult.points.empty()) {
            wxEndBusyCursor();
            SetStatusText("No elevation data found.", 0);
            m_hasProfile = false;
            m_canvas->Refresh();
            return;
        }
    }

    wxEndBusyCursor();

    m_hasProfile = true;
    const auto& pts = CurrentPoints();

    // Compute diagram extents
    m_kmMin = pts.front().km;
    m_kmMax = pts.back().km;
    m_elevMin = 1e9;
    m_elevMax = -1e9;
    for (const auto& pt : pts) {
        if (pt.elevation > -9000) {
            m_elevMin = std::min(m_elevMin, pt.elevation);
            m_elevMax = std::max(m_elevMax, pt.elevation);
        }
    }

    double elevPad = (m_elevMax - m_elevMin) * 0.1;
    if (elevPad < 20) elevPad = 20;
    m_elevMin = std::max(0.0, m_elevMin - elevPad);
    m_elevMax += elevPad;
    m_elevMin = std::floor(m_elevMin / 50) * 50;
    m_elevMax = std::ceil(m_elevMax / 50) * 50;

    double lengthKm = m_kmMax - m_kmMin;
    m_diagramW = kMarginLeft + kMarginRight +
                 std::max(800, static_cast<int>(lengthKm * kPixelsPerKm));

    m_canvas->SetScrollbars(1, 1, m_diagramW,
                            kMarginTop + kDiagramHeight + kMarginBottom,
                            0, 0);

    UpdateStats();
    m_canvas->Refresh();

    const auto& stats = (m_mode == Mode::Railway) ?
        m_railResult.stats : m_roadResult.stats;
    SetStatusText(wxString::Format("%s — %.1f km, %d points",
                                   stats.lineName,
                                   stats.totalLengthKm,
                                   static_cast<int>(pts.size())), 0);
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
    return kMarginTop + static_cast<int>(
        (1.0 - (elev - m_elevMin) / range) * kDiagramHeight);
}

double ProfileView::XToKm(int x) const
{
    int plotW = m_diagramW - kMarginLeft - kMarginRight;
    if (plotW <= 0) return m_kmMin;
    return m_kmMin + static_cast<double>(x - kMarginLeft) / plotW *
        (m_kmMax - m_kmMin);
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

    const auto& pts = CurrentPoints();
    if (!m_hasProfile || pts.empty())
        return;

    int plotW = m_diagramW - kMarginLeft - kMarginRight;

    // --- Grid lines ---
    dc.SetPen(wxPen(wxColour(220, 220, 220), 1));
    dc.SetFont(wxFont(8, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                       wxFONTWEIGHT_NORMAL));

    double elevRange = m_elevMax - m_elevMin;
    double elevStep = 100;
    if (elevRange > 2000) elevStep = 500;
    else if (elevRange > 1000) elevStep = 200;
    else if (elevRange > 500) elevStep = 100;
    else elevStep = 50;

    dc.SetTextForeground(wxColour(100, 100, 100));
    for (double e = std::ceil(m_elevMin / elevStep) * elevStep;
         e <= m_elevMax; e += elevStep) {
        int y = ElevToY(e);
        dc.DrawLine(kMarginLeft, y, kMarginLeft + plotW, y);
        dc.DrawText(wxString::Format("%.0f m", e), 5, y - 7);
    }

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
    dc.DrawText("km", kMarginLeft + plotW + 5,
                kMarginTop + kDiagramHeight + 5);

    // --- Axes ---
    dc.SetPen(wxPen(*wxBLACK, 1));
    dc.DrawLine(kMarginLeft, kMarginTop,
                kMarginLeft, kMarginTop + kDiagramHeight);
    dc.DrawLine(kMarginLeft, kMarginTop + kDiagramHeight,
                kMarginLeft + plotW, kMarginTop + kDiagramHeight);

    // --- Profile fill ---
    int baseY = ElevToY(m_elevMin);

    {
        std::vector<wxPoint> polyPts;
        polyPts.push_back({KmToX(pts.front().km), baseY});
        for (const auto& pt : pts) {
            if (pt.elevation > -9000)
                polyPts.push_back({KmToX(pt.km), ElevToY(pt.elevation)});
        }
        polyPts.push_back({KmToX(pts.back().km), baseY});

        if (m_mode == Mode::Railway)
            dc.SetBrush(wxBrush(wxColour(200, 230, 200)));
        else
            dc.SetBrush(wxBrush(wxColour(220, 220, 200)));  // light tan for roads
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawPolygon(static_cast<int>(polyPts.size()), polyPts.data());
    }

    // Railway-specific: tunnel section overlay
    if (m_mode == Mode::Railway) {
        size_t i = 0;
        while (i < pts.size()) {
            if (pts[i].medium != 'U' || pts[i].elevation < -9000) {
                i++;
                continue;
            }

            std::vector<wxPoint> tunnelPoly;
            tunnelPoly.push_back({KmToX(pts[i].km), baseY});

            while (i < pts.size() && pts[i].medium == 'U' &&
                   pts[i].elevation > -9000) {
                tunnelPoly.push_back({KmToX(pts[i].km),
                                      ElevToY(pts[i].elevation)});
                i++;
            }
            tunnelPoly.push_back(
                {KmToX(pts[i > 0 ? i - 1 : 0].km), baseY});

            dc.SetBrush(wxBrush(wxColour(180, 170, 150)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawPolygon(static_cast<int>(tunnelPoly.size()),
                           tunnelPoly.data());
        }
    }

    // --- Profile line ---
    for (size_t i = 1; i < pts.size(); i++) {
        const auto& prev = pts[i - 1];
        const auto& cur = pts[i];
        if (prev.elevation < -9000 || cur.elevation < -9000) continue;
        if (cur.km - prev.km > 1.0) continue;

        if (m_mode == Mode::Railway) {
            if (cur.medium == 'U')
                dc.SetPen(wxPen(wxColour(120, 100, 80), 2));
            else if (cur.medium == 'L' || cur.medium == 'B')
                dc.SetPen(wxPen(wxColour(70, 130, 180), 2));
            else
                dc.SetPen(wxPen(wxColour(40, 120, 40), 2));
        } else {
            // Road color by category
            switch (m_roadResult.roadId.kategori) {
                case 'E': dc.SetPen(wxPen(wxColour(50, 120, 50), 2)); break;
                case 'R': dc.SetPen(wxPen(wxColour(180, 80, 0), 2)); break;
                default:  dc.SetPen(wxPen(wxColour(100, 100, 100), 2)); break;
            }
        }

        dc.DrawLine(KmToX(prev.km), ElevToY(prev.elevation),
                     KmToX(cur.km), ElevToY(cur.elevation));
    }

    // --- Markers ---
    dc.SetFont(wxFont(7, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                       wxFONTWEIGHT_NORMAL));

    if (m_mode == Mode::Railway) {
        // Station markers
        for (const auto& st : m_railResult.stations) {
            int x = KmToX(st.km);
            double elev = st.elevation;
            if (elev < -9000) continue;
            int y = ElevToY(elev);

            dc.SetPen(wxPen(wxColour(100, 100, 100), 1, wxPENSTYLE_DOT));
            dc.DrawLine(x, y, x, kMarginTop + kDiagramHeight);

            if (st.type == 'I') {
                dc.SetPen(wxPen(*wxBLACK, 1));
                dc.SetBrush(wxBrush(wxColour(255, 180, 50)));
                dc.DrawCircle(x, y, 4);
            } else {
                dc.SetPen(wxPen(*wxBLACK, 1));
                dc.SetBrush(wxBrush(wxColour(255, 50, 50)));
                dc.DrawCircle(x, y, 3);
            }

            if (!st.name.empty()) {
                dc.SetTextForeground(wxColour(60, 60, 60));
                dc.DrawRotatedText(wxString::FromUTF8(st.name),
                                   x + 3, kMarginTop + kDiagramHeight + 3,
                                   45.0);
            }
        }
    } else {
        // Road intersection markers
        for (const auto& ix : m_roadResult.intersections) {
            int x = KmToX(ix.km);
            double elev = ix.elevation;
            if (elev <= 0) continue;
            int y = ElevToY(elev);

            // Vertical tick
            dc.SetPen(wxPen(wxColour(100, 100, 100), 1, wxPENSTYLE_DOT));
            dc.DrawLine(x, y, x, kMarginTop + kDiagramHeight);

            if (ix.isRailway) {
                // Railway crossing: X mark in dark red
                dc.SetPen(wxPen(wxColour(160, 20, 20), 2));
                dc.DrawLine(x - 4, y - 4, x + 4, y + 4);
                dc.DrawLine(x - 4, y + 4, x + 4, y - 4);
            } else {
                // Road crossing: diamond colored by category
                wxColour col;
                switch (ix.crossingRoad.kategori) {
                    case 'E': col = wxColour(50, 120, 50); break;
                    case 'R': col = wxColour(180, 80, 0); break;
                    case 'F': col = wxColour(100, 100, 100); break;
                    default:  col = wxColour(160, 160, 160); break;
                }
                dc.SetPen(wxPen(*wxBLACK, 1));
                dc.SetBrush(wxBrush(col));
                wxPoint diamond[4] = {{x, y - 4}, {x + 4, y},
                                      {x, y + 4}, {x - 4, y}};
                dc.DrawPolygon(4, diamond);
            }

            // Label
            std::string label = ix.isRailway ? ix.railLine
                                             : ix.crossingRoad.Label();
            if (!label.empty()) {
                dc.SetTextForeground(wxColour(60, 60, 60));
                dc.DrawRotatedText(wxString::FromUTF8(label),
                                   x + 3, kMarginTop + kDiagramHeight + 3,
                                   45.0);
            }
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
    m_mouseKm = km;
    m_mouseElev = YToElev(viewY);
    m_canvas->Refresh();

    SetStatusText(wxString::Format("km: %.1f", km), 0);

    const auto& pts = CurrentPoints();
    double bestDist = 1e9;
    const ProfilePoint* closest = nullptr;
    for (const auto& pt : pts) {
        double d = std::abs(pt.km - km);
        if (d < bestDist) {
            bestDist = d;
            closest = &pt;
        }
    }

    if (closest && closest->elevation > -9000) {
        SetStatusText(wxString::Format("Elevation: %.0f m",
                                        closest->elevation), 1);

        if (m_mode == Mode::Railway) {
            const char* medName = "Surface";
            if (closest->medium == 'U') medName = "Tunnel";
            else if (closest->medium == 'L' || closest->medium == 'B')
                medName = "Bridge";
            SetStatusText(medName, 2);

            double bestStDist = 1e9;
            const ProfileStation* nearestSt = nullptr;
            for (const auto& st : m_railResult.stations) {
                double d = std::abs(st.km - km);
                if (d < bestStDist) {
                    bestStDist = d;
                    nearestSt = &st;
                }
            }
            if (nearestSt && bestStDist < 5.0)
                SetStatusText(wxString::Format("Near: %s (%.1f km)",
                    nearestSt->name, nearestSt->km), 3);
            else
                SetStatusText("", 3);
        } else {
            SetStatusText("", 2);

            double bestIxDist = 1e9;
            const RoadIntersection* nearestIx = nullptr;
            for (const auto& ix : m_roadResult.intersections) {
                double d = std::abs(ix.km - km);
                if (d < bestIxDist) {
                    bestIxDist = d;
                    nearestIx = &ix;
                }
            }
            if (nearestIx && bestIxDist < 5.0) {
                std::string name = nearestIx->isRailway ?
                    nearestIx->railLine : nearestIx->crossingRoad.Label();
                SetStatusText(wxString::Format("Near: %s (%.1f km)",
                    name, nearestIx->km), 3);
            } else {
                SetStatusText("", 3);
            }
        }
    }
}

// ─── Stats panel update ─────────────────────────────────────────────

void ProfileView::UpdateStats()
{
    if (!m_hasProfile) return;

    const auto& s = (m_mode == Mode::Railway) ?
        m_railResult.stats : m_roadResult.stats;

    m_lblLength->SetLabel(wxString::Format("%.1f km", s.totalLengthKm));
    m_lblElevRange->SetLabel(wxString::Format(
        "%.0f – %.0f m (%.0f m)",
        s.minElev, s.maxElev, s.maxElev - s.minElev));
    m_lblClimb->SetLabel(wxString::Format("%.0f m / %.0f m",
                                           s.totalClimb, s.totalDescent));
    m_lblMaxGrade->SetLabel(wxString::Format("%.1f %%", s.maxGradePct));

    if (m_mode == Mode::Railway) {
        m_lblExtra1->SetLabel(wxString::Format(
            "%.1f km (%.1f%%)", s.tunnelLengthKm,
            s.totalLengthKm > 0 ?
            s.tunnelLengthKm / s.totalLengthKm * 100 : 0));
        m_lblExtra2->SetLabel(wxString::Format("%.1f km", s.bridgeLengthKm));
        m_lblStations->SetLabel(wxString::Format("%d", s.stationCount));
    } else {
        // Count rail vs road crossings
        int railCrossings = 0, roadCrossings = 0;
        for (const auto& ix : m_roadResult.intersections) {
            if (ix.isRailway) railCrossings++;
            else roadCrossings++;
        }
        m_lblExtra1->SetLabel(wxString::Format("%d", railCrossings));
        m_lblExtra2->SetLabel(wxString::Format("%d", roadCrossings));
        m_lblStations->SetLabel(wxString::Format(
            "%d", static_cast<int>(m_roadResult.intersections.size())));
    }

    m_lblSegments->SetLabel(wxString::Format("%d", s.segmentCount));
    m_statsPanel->Layout();
}

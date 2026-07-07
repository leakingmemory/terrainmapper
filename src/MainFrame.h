#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <optional>
#include <string>
#include <vector>

struct TileBounds {
    double latMin, latMax;
    double lonMin, lonMax;
};

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame();

    // Build a GDAL nested-vsizip path from an outer zip and an entry name.
    // Returns empty string if the entry name doesn't match the DTM naming pattern.
    static std::string BuildTileVsiPath(const std::string& outerZipPath,
                                        const std::string& entryName);

private:
    wxListCtrl* m_list;
    std::vector<std::string> m_zipPaths;
    std::vector<TileBounds> m_tileBounds;  // WGS84 bounds per loaded tile
    std::string m_ar50Path;       // path to the AR50 .gdb (on disk)
    std::string m_ar50TempDir;    // temp directory to clean up
    std::string m_railwayPath;    // vsizip path to railway .gdb
    std::string m_roadsPath;      // path to roads GPKG (on disk)
    std::string m_roadsTempDir;   // temp directory for road data
    std::string m_osmDataPath;  // path to OSM enrichment GPKG

    void CleanupAr50Temp();
    void CleanupRoadsTemp();

    void OnOpenZip(wxCommandEvent& event);
    void OnLoadLandCover(wxCommandEvent& event);
    void OnLoadTransport(wxCommandEvent& event);
    void OnEnrichOsm(wxCommandEvent& event);
    void OnCloseAll(wxCommandEvent& event);
    void OnRailwayProfile(wxCommandEvent& event);
    void OnExit(wxCommandEvent& event);
    void OnAbout(wxCommandEvent& event);
    void OnTileActivated(wxListEvent& event);

    void LoadZip(const wxString& path);
    void UpdateTitleAndStatus();

    std::optional<TileBounds> GetTileBounds(const std::string& outerZipPath,
                                            const std::string& entryName);

    wxDECLARE_EVENT_TABLE();
};

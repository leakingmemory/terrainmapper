#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>

#include "OsmData.h"

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class wxProgressDialog;

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
    void OnGameExport(wxCommandEvent& event);
    void OnExit(wxCommandEvent& event);
    void OnAbout(wxCommandEvent& event);
    void OnTileActivated(wxListEvent& event);
    void OnClose(wxCloseEvent& event);

    void LoadZip(const wxString& path);
    void UpdateTitleAndStatus();

    std::optional<TileBounds> GetTileBounds(const std::string& outerZipPath,
                                            const std::string& entryName);

    // Background Game Export (runs off the UI thread; updates marshaled back
    // via CallAfter). Inputs are copied so the worker never reads members.
    void RunExportWorker(std::string outputDir, std::string railwayPath,
                         std::string roadsPath, std::string osmDataPath,
                         std::vector<std::string> zipPaths, std::string ar50Path);
    void OnExportProgress(int pct, const wxString& msg);
    void OnExportDone(bool ok, bool cancelled, const wxString& outputDir);

    std::thread m_exportThread;
    std::atomic<bool> m_exportCancel{false};
    bool m_exportRunning = false;
    wxProgressDialog* m_exportProgress = nullptr;

    wxDECLARE_EVENT_TABLE();
};

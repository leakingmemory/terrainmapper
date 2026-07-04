#pragma once

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <optional>
#include <string>

struct TileBounds {
    double latMin, latMax;
    double lonMin, lonMax;
};

class MainFrame : public wxFrame {
public:
    MainFrame();

    // Build a GDAL nested-vsizip path from an outer zip and an entry name.
    // Returns empty string if the entry name doesn't match the DTM naming pattern.
    static std::string BuildTileVsiPath(const std::string& outerZipPath,
                                        const std::string& entryName);

private:
    wxListCtrl* m_list;
    std::string m_currentZipPath;

    void OnOpenZip(wxCommandEvent& event);
    void OnExit(wxCommandEvent& event);
    void OnAbout(wxCommandEvent& event);
    void OnTileActivated(wxListEvent& event);

    void LoadZip(const wxString& path);

    std::optional<TileBounds> GetTileBounds(const std::string& outerZipPath,
                                            const std::string& entryName);

    wxDECLARE_EVENT_TABLE();
};

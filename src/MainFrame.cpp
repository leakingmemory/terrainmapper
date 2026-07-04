#include "MainFrame.h"
#include "MapView.h"

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/progdlg.h>

#include <zip.h>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <ctime>
#include <regex>

enum {
    ID_OpenZip = wxID_HIGHEST + 1
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_MENU(ID_OpenZip, MainFrame::OnOpenZip)
    EVT_MENU(wxID_EXIT,  MainFrame::OnExit)
    EVT_MENU(wxID_ABOUT, MainFrame::OnAbout)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY, MainFrame::OnTileActivated)
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "TerrainMapper", wxDefaultPosition, wxSize(1050, 520))
{
    GDALAllRegister();

    // Menu bar
    auto* fileMenu = new wxMenu;
    fileMenu->Append(ID_OpenZip, "&Open ZIP...\tCtrl+O", "Open a ZIP archive");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "E&xit\tAlt+F4");

    auto* helpMenu = new wxMenu;
    helpMenu->Append(wxID_ABOUT);

    auto* menuBar = new wxMenuBar;
    menuBar->Append(fileMenu, "&File");
    menuBar->Append(helpMenu, "&Help");
    SetMenuBar(menuBar);

    // List control
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_HRULES | wxLC_VRULES);
    m_list->AppendColumn("Name",        wxLIST_FORMAT_LEFT,  360);
    m_list->AppendColumn("Size",        wxLIST_FORMAT_RIGHT,  90);
    m_list->AppendColumn("Compressed",  wxLIST_FORMAT_RIGHT,  90);
    m_list->AppendColumn("Date",        wxLIST_FORMAT_LEFT,  120);
    m_list->AppendColumn("Sheet",       wxLIST_FORMAT_LEFT,   70);
    m_list->AppendColumn("Latitude",    wxLIST_FORMAT_LEFT,  150);
    m_list->AppendColumn("Longitude",   wxLIST_FORMAT_LEFT,  150);

    // Status bar
    CreateStatusBar();
    SetStatusText("Ready \u2014 use File > Open ZIP to load an archive");
}

void MainFrame::OnOpenZip(wxCommandEvent&)
{
    wxFileDialog dlg(this, "Open ZIP file", wxEmptyString, wxEmptyString,
                     "ZIP files (*.zip)|*.zip|All files (*)|*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_CANCEL)
        return;
    LoadZip(dlg.GetPath());
}

void MainFrame::LoadZip(const wxString& path)
{
    int err = 0;
    zip_t* z = zip_open(path.ToStdString().c_str(), ZIP_RDONLY, &err);
    if (!z) {
        zip_error_t zerr;
        zip_error_init_with_code(&zerr, err);
        wxMessageBox(wxString::Format("Could not open ZIP: %s",
                                      zip_error_strerror(&zerr)),
                     "Error", wxICON_ERROR | wxOK, this);
        zip_error_fini(&zerr);
        return;
    }

    m_currentZipPath = path.ToStdString();
    m_list->DeleteAllItems();

    zip_int64_t n = zip_get_num_entries(z, 0);

    wxProgressDialog progress(
        "Loading ZIP",
        "Reading archive contents...",
        static_cast<int>(n),
        this,
        wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_SMOOTH |
        wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME);

    bool cancelled = false;
    for (zip_int64_t i = 0; i < n; i++) {
        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(z, i, 0, &st) != 0)
            continue;

        // Extract a short display name for the progress message
        wxString entryName = wxString::FromUTF8(st.name);
        wxString shortName = wxFileName(entryName).GetFullName();
        if (!progress.Update(
                static_cast<int>(i),
                wxString::Format("Reading tile %lld/%lld: %s",
                                 (long long)(i + 1), (long long)n,
                                 shortName))) {
            cancelled = true;
            break;
        }

        long row = m_list->InsertItem(i, entryName);

        if (st.valid & ZIP_STAT_SIZE)
            m_list->SetItem(row, 1,
                wxString::Format("%llu", (unsigned long long)st.size));
        if (st.valid & ZIP_STAT_COMP_SIZE)
            m_list->SetItem(row, 2,
                wxString::Format("%llu", (unsigned long long)st.comp_size));
        if (st.valid & ZIP_STAT_MTIME) {
            char buf[32];
            struct tm* tm_info = localtime(&st.mtime);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
            m_list->SetItem(row, 3, buf);
        }

        auto bounds = GetTileBounds(m_currentZipPath, st.name);
        if (bounds) {
            static const std::regex sheetRe(R"(Basisdata_(\d+-\d+)_)");
            std::cmatch m;
            if (std::regex_search(st.name, m, sheetRe))
                m_list->SetItem(row, 4, wxString::FromUTF8(m[1].str()));

            m_list->SetItem(row, 5,
                wxString::Format("%.2f \u2013 %.2f\u00b0N",
                                 bounds->latMin, bounds->latMax));
            m_list->SetItem(row, 6,
                wxString::Format("%.2f \u2013 %.2f\u00b0E",
                                 bounds->lonMin, bounds->lonMax));
        }
    }

    zip_close(z);

    if (cancelled) {
        m_list->DeleteAllItems();
        m_currentZipPath.clear();
        SetStatusText("Loading cancelled");
        SetTitle("TerrainMapper");
        return;
    }

    SetTitle(wxString::Format("TerrainMapper \u2014 %s",
                              wxFileName(path).GetFullName()));
    SetStatusText(wxString::Format("%s  (%lld entries)", path, (long long)n));
}

// Static helper: build a GDAL nested-vsizip path from the entry name.
// Entry names follow:  .../Basisdata_XXYY-Q_Celle_EPSG_DTM10UTMZZ_TIFF.zip
// Inner tif:           XXYY_Q_10m_zZZ.tif
std::string MainFrame::BuildTileVsiPath(const std::string& outerZipPath,
                                         const std::string& entryName)
{
    static const std::regex nameRe(
        R"(Basisdata_(\d+)-(\d+)_Celle_\d+_DTM10UTM(\d+)_TIFF\.zip$)");
    std::smatch m;
    if (!std::regex_search(entryName, m, nameRe))
        return {};

    const std::string sheet    = m[1].str();
    const std::string quadrant = m[2].str();
    const std::string utmZone  = m[3].str();

    const std::string tifName =
        sheet + "_" + quadrant + "_10m_z" + utmZone + ".tif";

    return "/vsizip//vsizip/" + outerZipPath + "/" + entryName + "/" + tifName;
}

std::optional<TileBounds> MainFrame::GetTileBounds(const std::string& outerZipPath,
                                                   const std::string& entryName)
{
    const std::string vsiPath = BuildTileVsiPath(outerZipPath, entryName);
    if (vsiPath.empty())
        return std::nullopt;

    GDALDataset* ds = static_cast<GDALDataset*>(GDALOpen(vsiPath.c_str(), GA_ReadOnly));
    if (!ds)
        return std::nullopt;

    double gt[6];
    if (ds->GetGeoTransform(gt) != CE_None) {
        GDALClose(ds);
        return std::nullopt;
    }

    const double ulx = gt[0];
    const double uly = gt[3];
    const double lrx = gt[0] + ds->GetRasterXSize() * gt[1];
    const double lry = gt[3] + ds->GetRasterYSize() * gt[5];

    OGRSpatialReference src, dst;
    src.importFromWkt(ds->GetProjectionRef());
    dst.SetWellKnownGeogCS("WGS84");
    src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    GDALClose(ds);

    OGRCoordinateTransformation* ct =
        OGRCreateCoordinateTransformation(&src, &dst);
    if (!ct)
        return std::nullopt;

    double x1 = ulx, y1 = uly;
    double x2 = lrx, y2 = lry;
    ct->Transform(1, &x1, &y1);
    ct->Transform(1, &x2, &y2);
    OCTDestroyCoordinateTransformation(ct);

    TileBounds b;
    b.lonMin = std::min(x1, x2);
    b.lonMax = std::max(x1, x2);
    b.latMin = std::min(y1, y2);
    b.latMax = std::max(y1, y2);
    return b;
}

void MainFrame::OnTileActivated(wxListEvent& event)
{
    if (m_currentZipPath.empty())
        return;

    wxListItem item;
    item.SetId(event.GetIndex());
    item.SetColumn(0);
    item.SetMask(wxLIST_MASK_TEXT);
    m_list->GetItem(item);
    const std::string entryName = item.GetText().ToStdString();

    const std::string vsiPath = BuildTileVsiPath(m_currentZipPath, entryName);
    if (vsiPath.empty()) {
        SetStatusText("Not a recognized DTM tile");
        return;
    }

    // Extract sheet for the window title
    static const std::regex sheetRe(R"(Basisdata_(\d+-\d+)_)");
    std::smatch m;
    wxString title = "Tile";
    if (std::regex_search(entryName, m, sheetRe))
        title = wxString::Format("Tile %s", m[1].str());

    auto* view = new MapView(nullptr, title, vsiPath);
    view->Show();
}

void MainFrame::OnExit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::OnAbout(wxCommandEvent&)
{
    wxMessageBox("TerrainMapper\nNorwegian DTM10 elevation data viewer",
                 "About TerrainMapper", wxOK | wxICON_INFORMATION, this);
}

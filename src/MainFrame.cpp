#include "MainFrame.h"
#include "MapView.h"

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/progdlg.h>

#include <zip.h>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_vsi.h>

#include <ctime>
#include <functional>
#include <regex>

enum {
    ID_OpenZip = wxID_HIGHEST + 1,
    ID_LoadLandCover,
    ID_LoadTransport,
    ID_CloseAll
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_MENU(ID_OpenZip,       MainFrame::OnOpenZip)
    EVT_MENU(ID_LoadLandCover, MainFrame::OnLoadLandCover)
    EVT_MENU(ID_LoadTransport, MainFrame::OnLoadTransport)
    EVT_MENU(ID_CloseAll,      MainFrame::OnCloseAll)
    EVT_MENU(wxID_EXIT,        MainFrame::OnExit)
    EVT_MENU(wxID_ABOUT,       MainFrame::OnAbout)
    EVT_LIST_ITEM_ACTIVATED(wxID_ANY, MainFrame::OnTileActivated)
wxEND_EVENT_TABLE()

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "TerrainMapper", wxDefaultPosition, wxSize(1050, 520))
{
    GDALAllRegister();

    // Menu bar
    auto* fileMenu = new wxMenu;
    fileMenu->Append(ID_OpenZip, "&Open ZIP...\tCtrl+O", "Open one or more ZIP archives");
    fileMenu->Append(ID_LoadLandCover, "Load &Land Cover...", "Load an AR50 land cover dataset (.gdb in .zip)");
    fileMenu->Append(ID_LoadTransport, "Load &Transport Data...", "Load railway network (.gdb in .zip)");
    fileMenu->Append(ID_CloseAll, "&Close All\tCtrl+W", "Remove all loaded tiles");
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
    wxFileDialog dlg(this, "Open ZIP files", wxEmptyString, wxEmptyString,
                     "ZIP files (*.zip)|*.zip|All files (*)|*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() == wxID_CANCEL)
        return;

    wxArrayString paths;
    dlg.GetPaths(paths);
    for (const auto& p : paths)
        LoadZip(p);
}

void MainFrame::OnLoadLandCover(wxCommandEvent&)
{
    wxFileDialog dlg(this, "Load AR50 Land Cover", wxEmptyString, wxEmptyString,
                     "ZIP files (*.zip)|*.zip|All files (*)|*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_CANCEL)
        return;

    const std::string zipPath = dlg.GetPath().ToStdString();

    // Open the zip with libzip to find and extract the .gdb
    int err = 0;
    zip_t* z = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
    if (!z) {
        wxMessageBox("Could not open ZIP file.", "Error",
                     wxICON_ERROR | wxOK, this);
        return;
    }

    // Find the .gdb directory prefix and collect its entries
    std::string gdbPrefix;
    struct GdbEntry { zip_uint64_t index; std::string name; zip_uint64_t size; };
    std::vector<GdbEntry> gdbEntries;
    zip_uint64_t totalBytes = 0;

    zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(z, i, 0, &st) != 0)
            continue;
        std::string name = st.name;

        // Find the .gdb/ prefix
        if (gdbPrefix.empty()) {
            auto pos = name.find(".gdb/");
            if (pos != std::string::npos)
                gdbPrefix = name.substr(0, pos + 5);
        }

        if (!gdbPrefix.empty() && name.starts_with(gdbPrefix) && name != gdbPrefix) {
            gdbEntries.push_back({static_cast<zip_uint64_t>(i), name,
                                  (st.valid & ZIP_STAT_SIZE) ? st.size : 0});
            totalBytes += (st.valid & ZIP_STAT_SIZE) ? st.size : 0;
        }
    }

    if (gdbPrefix.empty() || gdbEntries.empty()) {
        wxMessageBox("No .gdb directory found inside the ZIP.", "Error",
                     wxICON_ERROR | wxOK, this);
        zip_close(z);
        return;
    }

    // Create temp directory for extraction
    CleanupAr50Temp();

    wxString tempBase = wxFileName::GetTempDir() + wxFileName::GetPathSeparator()
                        + "terrainmapper_ar50_XXXXXX";
    std::string tempTemplate = tempBase.ToStdString();
    char* tempDir = mkdtemp(tempTemplate.data());
    if (!tempDir) {
        wxMessageBox("Could not create temporary directory.", "Error",
                     wxICON_ERROR | wxOK, this);
        zip_close(z);
        return;
    }
    m_ar50TempDir = tempDir;

    // The .gdb directory name (strip trailing slash from prefix)
    std::string gdbDirName = gdbPrefix.substr(0, gdbPrefix.size() - 1);
    // Remove any leading path components (e.g. if prefix is "subdir/foo.gdb/")
    auto slashPos = gdbDirName.rfind('/');
    if (slashPos != std::string::npos)
        gdbDirName = gdbDirName.substr(slashPos + 1);

    const std::string gdbDiskPath = m_ar50TempDir + "/" + gdbDirName;
    wxFileName::Mkdir(gdbDiskPath);

    // Extract files with progress
    wxProgressDialog progress(
        "Extracting Land Cover",
        "Extracting geodatabase...",
        100, this,
        wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_SMOOTH |
        wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME);

    zip_uint64_t bytesExtracted = 0;
    bool extractOk = true;
    std::vector<char> buf(256 * 1024);

    for (const auto& entry : gdbEntries) {
        // Get just the filename within the .gdb directory
        std::string relName = entry.name.substr(gdbPrefix.size());
        std::string outPath = gdbDiskPath + "/" + relName;

        progress.Update(
            totalBytes ? static_cast<int>(bytesExtracted * 95 / totalBytes) : 0,
            wxString::Format("Extracting %s...", relName));

        zip_file_t* zf = zip_fopen_index(z, entry.index, 0);
        if (!zf) { extractOk = false; break; }

        FILE* fp = fopen(outPath.c_str(), "wb");
        if (!fp) { zip_fclose(zf); extractOk = false; break; }

        zip_int64_t nread;
        while ((nread = zip_fread(zf, buf.data(), buf.size())) > 0) {
            fwrite(buf.data(), 1, static_cast<size_t>(nread), fp);
            bytesExtracted += nread;
            if (totalBytes > 0)
                progress.Update(static_cast<int>(bytesExtracted * 95 / totalBytes));
        }
        fclose(fp);
        zip_fclose(zf);
    }

    zip_close(z);

    if (!extractOk) {
        wxMessageBox("Failed to extract geodatabase files.", "Error",
                     wxICON_ERROR | wxOK, this);
        CleanupAr50Temp();
        return;
    }

    // Verify the extracted .gdb opens correctly
    progress.Update(96, "Verifying dataset...");
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(gdbDiskPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) {
        wxMessageBox("Could not open the extracted geodatabase.", "Error",
                     wxICON_ERROR | wxOK, this);
        CleanupAr50Temp();
        return;
    }
    OGRLayer* layer = ds->GetLayerByName("ar50");
    if (!layer) {
        wxMessageBox("No 'ar50' layer found in the geodatabase.", "Error",
                     wxICON_ERROR | wxOK, this);
        GDALClose(ds);
        CleanupAr50Temp();
        return;
    }
    GDALClose(ds);

    progress.Update(100);
    m_ar50Path = gdbDiskPath;
    SetStatusText(wxString::Format("Land cover ready: %s (%zu files extracted)",
                                   gdbDirName, gdbEntries.size()));
}

void MainFrame::CleanupAr50Temp()
{
    if (m_ar50TempDir.empty())
        return;
    wxFileName::Rmdir(m_ar50TempDir, wxPATH_RMDIR_RECURSIVE);
    m_ar50TempDir.clear();
    m_ar50Path.clear();
}

void MainFrame::OnLoadTransport(wxCommandEvent&)
{
    wxFileDialog dlg(this, "Load Transport Data", wxEmptyString, wxEmptyString,
                     "ZIP files (*.zip)|*.zip|All files (*)|*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_CANCEL)
        return;

    const std::string zipPath = dlg.GetPath().ToStdString();
    const std::string outerVsi = "/vsizip/" + zipPath;

    // Recursively scan the outer zip for transport data
    std::string railwayVsiPath;   // GML railway dataset (preferred, more detail)
    std::string roadZipVsiPath;   // path to the inner VegnettPluss zip

    std::function<void(const std::string&, int)> scanDir =
        [&](const std::string& dir, int depth) {
        if (depth > 4)
            return;
        char** entries = VSIReadDir(dir.c_str());
        if (!entries)
            return;
        for (int i = 0; entries[i]; i++) {
            std::string name = entries[i];
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".zip") {
                if (name.find("Banenettverk") != std::string::npos &&
                    name.find("GML") != std::string::npos &&
                    railwayVsiPath.empty()) {
                    // Found railway GML zip — use it directly via /vsizip/
                    railwayVsiPath = "/vsizip/" + dir + "/" + name;
                } else if (name.find("NVDB-VegnettPluss") != std::string::npos &&
                           roadZipVsiPath.empty()) {
                    roadZipVsiPath = dir + "/" + name;
                }
            } else {
                scanDir(dir + "/" + name, depth + 1);
            }
        }
        CSLDestroy(entries);
    };
    scanDir(outerVsi, 0);

    if (railwayVsiPath.empty() && roadZipVsiPath.empty()) {
        wxMessageBox("No transport data found in the ZIP.\n"
                     "Expected '*Banenettverk*GML.zip' or '*NVDB-VegnettPluss*.zip'.",
                     "Error", wxICON_ERROR | wxOK, this);
        return;
    }

    // --- Railway ---
    int trackCount = 0, stationCount = 0;
    if (!railwayVsiPath.empty()) {
        GDALDataset* ds = static_cast<GDALDataset*>(
            GDALOpenEx(railwayVsiPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (ds) {
            OGRLayer* tracks = ds->GetLayerByName("Banelenke");
            OGRLayer* stations = ds->GetLayerByName("Stasjonsnode");
            trackCount = tracks ? static_cast<int>(tracks->GetFeatureCount()) : 0;
            stationCount = stations ? static_cast<int>(stations->GetFeatureCount()) : 0;
            GDALClose(ds);
            m_railwayPath = railwayVsiPath;
        }
    }

    // --- Roads ---
    int roadCount = 0;
    if (!roadZipVsiPath.empty()) {
        // Save the converted GPKG next to the source zip for reuse.
        // Derive the cache path from the source zip filename.
        wxFileName zipFn(zipPath);
        const std::string cachedGpkg =
            (zipFn.GetPath() + wxFileName::GetPathSeparator()
             + zipFn.GetName() + "_roads_v3.gpkg").ToStdString();

        // Check if a cached GPKG already exists
        GDALDataset* cached = static_cast<GDALDataset*>(
            GDALOpenEx(cachedGpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (cached) {
            OGRLayer* layer = cached->GetLayerByName("roads");
            roadCount = layer ? static_cast<int>(layer->GetFeatureCount()) : 0;
            GDALClose(cached);
            m_roadsPath = cachedGpkg;
        } else {
            // Need to convert — extract inner zip then build GPKG
            CleanupRoadsTemp();

            wxString tempBase = wxFileName::GetTempDir() + wxFileName::GetPathSeparator()
                                + "terrainmapper_roads_XXXXXX";
            std::string tempTemplate = tempBase.ToStdString();
            char* tempDir = mkdtemp(tempTemplate.data());
            if (!tempDir) {
                wxMessageBox("Could not create temporary directory.", "Error",
                             wxICON_ERROR | wxOK, this);
            } else {
                m_roadsTempDir = tempDir;
                const std::string innerZipPath = m_roadsTempDir + "/road.zip";

                int err = 0;
                zip_t* z = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
                if (!z) {
                    wxMessageBox("Could not open ZIP file.", "Error",
                                 wxICON_ERROR | wxOK, this);
                    CleanupRoadsTemp();
                } else {
                    zip_int64_t n = zip_get_num_entries(z, 0);
                    zip_int64_t roadIdx = -1;
                    zip_uint64_t roadSize = 0;
                    for (zip_int64_t i = 0; i < n; i++) {
                        zip_stat_t st;
                        zip_stat_init(&st);
                        if (zip_stat_index(z, i, 0, &st) != 0) continue;
                        std::string name = st.name;
                        if (name.find("NVDB-VegnettPluss") != std::string::npos &&
                            name.size() >= 4 && name.substr(name.size() - 4) == ".zip") {
                            roadIdx = i;
                            roadSize = (st.valid & ZIP_STAT_SIZE) ? st.size : 0;
                            break;
                        }
                    }

                    if (roadIdx < 0) {
                        zip_close(z);
                        CleanupRoadsTemp();
                    } else {
                        wxProgressDialog progress(
                            "Loading Road Data",
                            "Extracting road archive...",
                            100, this,
                            wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_SMOOTH |
                            wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME);

                        zip_file_t* zf = zip_fopen_index(z, roadIdx, 0);
                        FILE* fp = fopen(innerZipPath.c_str(), "wb");
                        std::vector<char> buf(1024 * 1024);
                        zip_uint64_t bytesWritten = 0;
                        zip_int64_t nread;
                        while ((nread = zip_fread(zf, buf.data(), buf.size())) > 0) {
                            fwrite(buf.data(), 1, static_cast<size_t>(nread), fp);
                            bytesWritten += nread;
                            if (roadSize > 0) {
                                int pct = static_cast<int>(bytesWritten * 30 / roadSize);
                                progress.Update(pct, wxString::Format(
                                    "Extracting road archive... %llu MB / %llu MB",
                                    (unsigned long long)(bytesWritten / (1024*1024)),
                                    (unsigned long long)(roadSize / (1024*1024))));
                            }
                        }
                        fclose(fp);
                        zip_fclose(zf);
                        zip_close(z);

                        // List GML files inside the extracted zip
                        const std::string roadVsi = "/vsizip/" + innerZipPath;
                        char** gmlFiles = VSIReadDir(roadVsi.c_str());
                        std::vector<std::string> gmlNames;
                        if (gmlFiles) {
                            for (int i = 0; gmlFiles[i]; i++) {
                                std::string fname = gmlFiles[i];
                                if (fname.size() > 4 &&
                                    fname.substr(fname.size() - 4) == ".gml")
                                    gmlNames.push_back(fname);
                            }
                            CSLDestroy(gmlFiles);
                        }

                        if (gmlNames.empty()) {
                            progress.Update(100);
                            CleanupRoadsTemp();
                        } else {
                            // Convert GML files → single GPKG with major roads only
                            // Write to persistent location next to the source zip
                            bool firstFile = true;

                            for (size_t fi = 0; fi < gmlNames.size(); fi++) {
                                int pct = 30 + static_cast<int>(fi * 65 / gmlNames.size());
                                progress.Update(pct, wxString::Format(
                                    "Converting road data (%zu/%zu)...",
                                    fi + 1, gmlNames.size()));

                                std::string gmlPath = roadVsi + "/" + gmlNames[fi];
                                GDALDataset* src = static_cast<GDALDataset*>(
                                    GDALOpenEx(gmlPath.c_str(),
                                               GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                               nullptr, nullptr, nullptr));
                                if (!src) continue;

                                OGRLayer* veglenke = src->GetLayerByName("Veglenke");
                                if (!veglenke) { GDALClose(src); continue; }

                                veglenke->SetAttributeFilter(
                                    "vegkategori IN ('E','R','F','K','P')");

                                if (veglenke->GetFeatureCount() == 0) {
                                    GDALClose(src);
                                    continue;
                                }

                                GDALDataset* dst;
                                if (firstFile) {
                                    GDALDriver* gpkgDrv =
                                        GetGDALDriverManager()->GetDriverByName("GPKG");
                                    dst = gpkgDrv->Create(cachedGpkg.c_str(),
                                                          0, 0, 0, GDT_Unknown, nullptr);
                                    if (!dst) { GDALClose(src); break; }
                                } else {
                                    dst = static_cast<GDALDataset*>(
                                        GDALOpenEx(cachedGpkg.c_str(),
                                                   GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                                                   nullptr, nullptr, nullptr));
                                    if (!dst) { GDALClose(src); break; }
                                }

                                OGRLayer* dstLayer;
                                if (firstFile) {
                                    OGRSpatialReference* srs =
                                        veglenke->GetSpatialRef() ?
                                        veglenke->GetSpatialRef()->Clone() : nullptr;
                                    dstLayer = dst->CreateLayer(
                                        "roads", srs, wkbLineString, nullptr);
                                    if (srs) srs->Release();
                                    if (dstLayer) {
                                        OGRFieldDefn fldCat("vegkategori", OFTString);
                                        fldCat.SetWidth(1);
                                        (void)dstLayer->CreateField(&fldCat);
                                        OGRFieldDefn fldNum("vegnummer", OFTInteger);
                                        (void)dstLayer->CreateField(&fldNum);
                                    }
                                    firstFile = false;
                                } else {
                                    dstLayer = dst->GetLayerByName("roads");
                                }

                                if (dstLayer) {
                                    veglenke->ResetReading();
                                    OGRFeature* feat;
                                    while ((feat = veglenke->GetNextFeature()) != nullptr) {
                                        OGRFeature* outFeat =
                                            OGRFeature::CreateFeature(dstLayer->GetLayerDefn());
                                        outFeat->SetGeometry(feat->GetGeometryRef());
                                        const char* cat = feat->GetFieldAsString("vegkategori");
                                        if (cat) outFeat->SetField("vegkategori", cat);
                                        int vegnum = feat->GetFieldAsInteger("vegnummer");
                                        outFeat->SetField("vegnummer", vegnum);
                                        (void)dstLayer->CreateFeature(outFeat);
                                        OGRFeature::DestroyFeature(outFeat);
                                        OGRFeature::DestroyFeature(feat);
                                        roadCount++;
                                    }
                                }

                                GDALClose(dst);
                                GDALClose(src);
                            }

                            progress.Update(98, "Finalizing...");

                            if (roadCount > 0)
                                m_roadsPath = cachedGpkg;

                            // Clean up temp (inner zip extraction)
                            CleanupRoadsTemp();
                            progress.Update(100);
                        }
                    }
                }
            }
        }
    }

    // Summary
    wxString msg;
    if (!m_railwayPath.empty())
        msg += wxString::Format("Railway: %d tracks, %d stations. ",
                                trackCount, stationCount);
    if (!m_roadsPath.empty())
        msg += wxString::Format("Roads: %d segments.", roadCount);
    if (msg.empty())
        msg = "No usable transport data found.";
    SetStatusText(msg);
}

void MainFrame::CleanupRoadsTemp()
{
    if (m_roadsTempDir.empty())
        return;
    wxFileName::Rmdir(m_roadsTempDir, wxPATH_RMDIR_RECURSIVE);
    m_roadsTempDir.clear();
}

MainFrame::~MainFrame()
{
    CleanupAr50Temp();
    CleanupRoadsTemp();
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

    const std::string zipPath = path.ToStdString();
    const long zipIndex = static_cast<long>(m_zipPaths.size());
    m_zipPaths.push_back(zipPath);

    const long startRow = m_list->GetItemCount();

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

        long row = m_list->InsertItem(m_list->GetItemCount(), entryName);
        m_list->SetItemData(row, zipIndex);

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

        auto bounds = GetTileBounds(zipPath, st.name);
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
        for (long r = m_list->GetItemCount() - 1; r >= startRow; --r)
            m_list->DeleteItem(r);
        m_zipPaths.pop_back();
        SetStatusText("Loading cancelled");
    }

    UpdateTitleAndStatus();
}

void MainFrame::UpdateTitleAndStatus()
{
    const long totalItems = m_list->GetItemCount();
    const size_t numZips = m_zipPaths.size();

    if (numZips == 0) {
        SetTitle("TerrainMapper");
        SetStatusText("Ready \u2014 use File > Open ZIP to load an archive");
    } else if (numZips == 1) {
        SetTitle(wxString::Format("TerrainMapper \u2014 %s",
                 wxFileName(m_zipPaths[0]).GetFullName()));
        SetStatusText(wxString::Format("%ld tiles", totalItems));
    } else {
        SetTitle(wxString::Format("TerrainMapper \u2014 %zu zips, %ld tiles",
                                  numZips, totalItems));
        SetStatusText(wxString::Format("%zu archives loaded, %ld tiles total",
                                       numZips, totalItems));
    }
}

void MainFrame::OnCloseAll(wxCommandEvent&)
{
    m_list->DeleteAllItems();
    m_zipPaths.clear();
    CleanupAr50Temp();
    CleanupRoadsTemp();
    m_railwayPath.clear();
    UpdateTitleAndStatus();
}

// Static helper: build a GDAL nested-vsizip path from the entry name.
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
    if (m_zipPaths.empty())
        return;

    const long zipIndex = m_list->GetItemData(event.GetIndex());
    if (zipIndex < 0 || static_cast<size_t>(zipIndex) >= m_zipPaths.size())
        return;

    wxListItem item;
    item.SetId(event.GetIndex());
    item.SetColumn(0);
    item.SetMask(wxLIST_MASK_TEXT);
    m_list->GetItem(item);
    const std::string entryName = item.GetText().ToStdString();

    const std::string vsiPath = BuildTileVsiPath(m_zipPaths[zipIndex], entryName);
    if (vsiPath.empty()) {
        SetStatusText("Not a recognized DTM tile");
        return;
    }

    static const std::regex sheetRe(R"(Basisdata_(\d+-\d+)_)");
    std::smatch m;
    wxString title = "Tile";
    if (std::regex_search(entryName, m, sheetRe))
        title = wxString::Format("Tile %s", m[1].str());

    auto* view = new MapView(nullptr, title, vsiPath, m_ar50Path, m_railwayPath,
                             m_roadsPath);
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

#include <wx/wx.h>
#include "MainFrame.h"

#include <cpl_conv.h>

#include <clocale>

class App : public wxApp {
public:
    bool OnInit() override {
        // Adopt the environment's character encoding (UTF-8) so wxString can
        // convert the UTF-8 string literals used across the UI. Only LC_CTYPE
        // is touched — LC_NUMERIC stays "C" so GDAL/PROJ coordinate and WKT
        // parsing keeps using '.' as the decimal separator.
        std::setlocale(LC_CTYPE, "");

        // Cap GDAL's raster block cache. It otherwise defaults to ~5% of RAM
        // and can grow further while warping the 100 MB DTM cells during
        // export; on low-RAM machines that contributes to OOM. 512 MB is
        // plenty for the export's spatially-coherent tile sampling.
        CPLSetConfigOption("GDAL_CACHEMAX", "512");

        auto* frame = new MainFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(App);

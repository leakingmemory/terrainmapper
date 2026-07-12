#include <wx/wx.h>
#include "MainFrame.h"

#include <clocale>

class App : public wxApp {
public:
    bool OnInit() override {
        // Adopt the environment's character encoding (UTF-8) so wxString can
        // convert the UTF-8 string literals used across the UI. Only LC_CTYPE
        // is touched — LC_NUMERIC stays "C" so GDAL/PROJ coordinate and WKT
        // parsing keeps using '.' as the decimal separator.
        std::setlocale(LC_CTYPE, "");

        auto* frame = new MainFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(App);

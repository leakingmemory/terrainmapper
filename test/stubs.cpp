// Linker stubs for test targets that compile ProfileData.cpp
// without the full MainFrame / wxWidgets dependency.

#include <string>

class MainFrame {
public:
    static std::string BuildTileVsiPath(const std::string&,
                                         const std::string&);
};

std::string MainFrame::BuildTileVsiPath(const std::string&,
                                         const std::string&)
{
    return {};
}

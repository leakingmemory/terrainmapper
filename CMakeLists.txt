cmake_minimum_required(VERSION 3.20)
project(terrainmapper CXX)
set(CMAKE_CXX_STANDARD 26)

# wxWidgets — non-standard install path on FreeBSD
set(wxWidgets_CONFIG_EXECUTABLE /usr/local/bin/wxgtk3u-3.2-config)
find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

# libzip + GDAL
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED libzip)
pkg_check_modules(GDAL  REQUIRED gdal)

add_executable(terrainmapper
    src/main.cpp
    src/MainFrame.cpp
    src/MapView.cpp
    src/OsmData.cpp
    src/ProfileData.cpp
    src/ProfileView.cpp
    src/LocalMapView.cpp
    src/RoadProfileData.cpp
)

target_include_directories(terrainmapper PRIVATE
    ${LIBZIP_INCLUDE_DIRS}
    ${GDAL_INCLUDE_DIRS}
)
target_link_libraries(terrainmapper PRIVATE
    ${wxWidgets_LIBRARIES}
    ${LIBZIP_LIBRARIES}
    ${GDAL_LIBRARIES}
)
target_compile_options(terrainmapper PRIVATE
    ${LIBZIP_CFLAGS_OTHER}
    ${GDAL_CFLAGS_OTHER}
)

# --- Tests ---
enable_testing()

add_executable(test_osm_extract
    test/test_osm_extract.cpp
    src/OsmData.cpp
)
target_include_directories(test_osm_extract PRIVATE
    ${GDAL_INCLUDE_DIRS}
)
target_link_libraries(test_osm_extract PRIVATE
    ${GDAL_LIBRARIES}
)
target_compile_options(test_osm_extract PRIVATE
    ${GDAL_CFLAGS_OTHER}
)

add_test(
    NAME osm_extract
    COMMAND test_osm_extract "${CMAKE_SOURCE_DIR}/test/data/oslo_central.osm.pbf"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

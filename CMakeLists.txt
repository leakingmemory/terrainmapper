cmake_minimum_required(VERSION 3.20)
project(terrainmapper CXX)
set(CMAKE_CXX_STANDARD 26)

# wxWidgets — prefer wx-config on PATH (Linux); fall back to the FreeBSD path.
find_program(WX_CONFIG NAMES wx-config wxgtk3u-3.2-config
             PATHS /usr/local/bin)
if(WX_CONFIG)
    set(wxWidgets_CONFIG_EXECUTABLE ${WX_CONFIG})
endif()
find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

# libzip + GDAL
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED libzip)
pkg_check_modules(GDAL  REQUIRED gdal)

add_executable(terrainmapper
    src/main.cpp
    src/GameExport.cpp
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

add_executable(test_elevation_filter
    test/test_elevation_filter.cpp
    src/ProfileData.cpp
    test/stubs.cpp
)
target_include_directories(test_elevation_filter PRIVATE
    ${GDAL_INCLUDE_DIRS}
    ${LIBZIP_INCLUDE_DIRS}
)
target_link_libraries(test_elevation_filter PRIVATE
    ${GDAL_LIBRARIES}
    ${LIBZIP_LIBRARIES}
)
target_compile_options(test_elevation_filter PRIVATE
    ${GDAL_CFLAGS_OTHER}
    ${LIBZIP_CFLAGS_OTHER}
)

add_test(NAME elevation_filter COMMAND test_elevation_filter)

#include "GameExport.h"
#include "MainFrame.h"

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <gdal_utils.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

// Rasterize the AR50 land-cover layer (EPSG:4258, "artype" attribute) into a
// 256x256 uint8 tile matching the export heightmap grid (row 0 = north).
// `out` gets one AR50 artype code per pixel; 0 means unclassified / no polygon.
// Returns false on failure (callers then skip writing landcover for the tile).
bool RasterizeTileLandCover(GDALDataset* ar50DS, OGRLayer* layer,
                            const char* wkt25833,
                            OGRCoordinateTransformation* to4258,
                            const ExportTile& tile,
                            std::vector<uint8_t>& out)
{
    constexpr int N = 256;

    // North-up geotransform: top-left origin, +res east, -res south.
    double gt[6] = {tile.originX, tile.resolution, 0.0,
                    tile.originY + tile.extent, 0.0, -tile.resolution};

    GDALDriver* memDrv = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!memDrv) return false;
    GDALDataset* memDs = memDrv->Create("", N, N, 1, GDT_Int32, nullptr);
    if (!memDs) return false;
    memDs->SetGeoTransform(gt);
    memDs->SetProjection(wkt25833);
    memDs->GetRasterBand(1)->Fill(0);

    // Spatial-filter AR50 to this tile's footprint (in the layer CRS) for speed.
    double xs[4] = {tile.originX, tile.originX + tile.extent,
                    tile.originX, tile.originX + tile.extent};
    double ys[4] = {tile.originY, tile.originY,
                    tile.originY + tile.extent, tile.originY + tile.extent};
    if (to4258) to4258->Transform(4, xs, ys);
    double minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, xs[i]); maxX = std::max(maxX, xs[i]);
        minY = std::min(minY, ys[i]); maxY = std::max(maxY, ys[i]);
    }
    layer->SetSpatialFilterRect(minX, minY, maxX, maxY);

    // Use the high-level rasterizer: it reprojects the AR50 polygons
    // (EPSG:4258, latitude/longitude authority axis order) into the tile's
    // EPSG:25833 grid correctly. The earlier low-level GDALRasterizeLayers +
    // GDALCreateGenImgProjTransformer2 path mishandled the source CRS axis
    // order and burned nothing (every tile came out all-zero).
    char** argv = nullptr;
    argv = CSLAddString(argv, "-a");
    argv = CSLAddString(argv, "artype");
    argv = CSLAddString(argv, "-l");
    argv = CSLAddString(argv, layer->GetName());
    GDALRasterizeOptions* ropts = GDALRasterizeOptionsNew(argv, nullptr);

    bool ok = false;
    if (ropts) {
        int usageErr = 0;
        GDALDatasetH res = GDALRasterize(nullptr, GDALDataset::ToHandle(memDs),
                                         GDALDataset::ToHandle(ar50DS), ropts,
                                         &usageErr);
        GDALRasterizeOptionsFree(ropts);
        if (res) {
            std::vector<int32_t> lc(static_cast<size_t>(N) * N, 0);
            if (memDs->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, N, N, lc.data(),
                                                  N, N, GDT_Int32, 0, 0) == CE_None) {
                out.resize(static_cast<size_t>(N) * N);
                for (size_t i = 0; i < lc.size(); ++i) {
                    const int32_t v = lc[i];
                    out[i] = (v < 0 || v > 255) ? 0 : static_cast<uint8_t>(v);
                }
                ok = true;
            }
        }
    }
    CSLDestroy(argv);

    layer->SetSpatialFilter(nullptr);
    GDALClose(memDs);
    return ok;
}

} // namespace

// ─── LOD definitions ───────────────────────────────────────────────

const std::vector<LodLevel>& GameExporter::GetLodLevels()
{
    static const std::vector<LodLevel> levels = {
        { 0,  10.0,  10.0 * 256,     0,    200 },  // 0–200m:  10m/px, 2.56km tiles
        { 1,  20.0,  20.0 * 256,   200,   1000 },  // 200m–1km: 20m/px, 5.12km tiles
        { 2,  40.0,  40.0 * 256,  1000,   5000 },  // 1–5km: 40m/px, 10.24km tiles
        { 3,  80.0,  80.0 * 256,  5000,  20000 },  // 5–20km: 80m/px, 20.48km tiles
    };
    return levels;
}

// ─── Distance from point to any rail segment ───────────────────────

double GameExporter::MinDistToRail(double x, double y) const
{
    double best = 1e18;
    for (const auto& bb : m_railBBoxes) {
        // Distance from point to axis-aligned bbox
        double dx = std::max({bb.minX - x, 0.0, x - bb.maxX});
        double dy = std::max({bb.minY - y, 0.0, y - bb.maxY});
        double d = std::sqrt(dx * dx + dy * dy);
        if (d < best) best = d;
        if (best == 0) return 0;
    }
    return best;
}

// ─── Phase 1: Collect rail geometry ────────────────────────────────

bool GameExporter::CollectRailGeometry(const std::string& railwayPath,
                                        const std::string& osmDataPath,
                                        ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };

    report(0, "Collecting rail geometry...");

    // Main lines from GML
    if (!railwayPath.empty()) {
        auto* ds = static_cast<GDALDataset*>(
            GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (ds) {
            OGRLayer* layer = ds->GetLayerByName("Banelenke");
            if (layer) {
                layer->SetAttributeFilter("banestatus = 'I'");
                layer->ResetReading();
                OGRFeature* feat;
                while ((feat = layer->GetNextFeature()) != nullptr) {
                    OGRGeometry* geom = feat->GetGeometryRef();
                    if (geom) {
                        OGREnvelope env;
                        geom->getEnvelope(&env);
                        m_railBBoxes.push_back({env.MinX, env.MinY,
                                                env.MaxX, env.MaxY});
                        // Update overall bounds
                        m_boundsMinX = std::min(m_boundsMinX, env.MinX);
                        m_boundsMinY = std::min(m_boundsMinY, env.MinY);
                        m_boundsMaxX = std::max(m_boundsMaxX, env.MaxX);
                        m_boundsMaxY = std::max(m_boundsMaxY, env.MaxY);
                    }
                    OGRFeature::DestroyFeature(feat);
                }
            }
            GDALClose(ds);
        }
    }

    report(2, "Collecting OSM sidings...");

    // Sidings from OSM GPKG
    if (!osmDataPath.empty()) {
        auto* ds = static_cast<GDALDataset*>(
            GDALOpenEx(osmDataPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (ds) {
            OGRLayer* layer = ds->GetLayerByName("railway_tracks");
            if (layer) {
                layer->SetAttributeFilter(
                    "service = 'siding' OR service = 'yard' OR "
                    "usage = 'industrial'");
                layer->ResetReading();

                // OSM is WGS84, need transform to EPSG:25833
                OGRSpatialReference wgs84, utm33;
                wgs84.SetWellKnownGeogCS("WGS84");
                wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                utm33.importFromEPSG(25833);
                auto* toUtm = OGRCreateCoordinateTransformation(&wgs84, &utm33);

                OGRFeature* feat;
                while ((feat = layer->GetNextFeature()) != nullptr) {
                    OGRGeometry* geom = feat->GetGeometryRef();
                    if (geom && toUtm) {
                        auto* clone = geom->clone();
                        clone->transform(toUtm);
                        OGREnvelope env;
                        clone->getEnvelope(&env);
                        m_railBBoxes.push_back({env.MinX, env.MinY,
                                                env.MaxX, env.MaxY});
                        m_boundsMinX = std::min(m_boundsMinX, env.MinX);
                        m_boundsMinY = std::min(m_boundsMinY, env.MinY);
                        m_boundsMaxX = std::max(m_boundsMaxX, env.MaxX);
                        m_boundsMaxY = std::max(m_boundsMaxY, env.MaxY);
                        OGRGeometryFactory::destroyGeometry(clone);
                    }
                    OGRFeature::DestroyFeature(feat);
                }
                if (toUtm)
                    OCTDestroyCoordinateTransformation(toUtm);
            }
            GDALClose(ds);
        }
    }

    // Expand bounds by 20km buffer
    constexpr double kBuffer = 20000.0;
    m_boundsMinX -= kBuffer;
    m_boundsMinY -= kBuffer;
    m_boundsMaxX += kBuffer;
    m_boundsMaxY += kBuffer;

    report(4, "Rail geometry collected: " + std::to_string(m_railBBoxes.size()) +
              " segments");
    return true;
}

// ─── Phase 2: Profile all main lines ───────────────────────────────

bool GameExporter::ProfileMainLines(const std::string& railwayPath,
                                     const std::string& roadsPath,
                                     ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };

    if (railwayPath.empty()) return true;

    auto lineNames = m_profileData.GetLineNames(railwayPath);
    report(5, "Profiling " + std::to_string(lineNames.size()) + " railway lines...");

    int done = 0;
    for (const auto& name : lineNames) {
        auto result = m_profileData.BuildProfile(railwayPath, roadsPath, name);

        // Convert ProfileResult to ExportTrack segments
        // Group consecutive points by medium into segments
        if (result.points.empty()) {
            done++;
            continue;
        }

        // Build a single polyline per line, preserving medium per-vertex
        // We split at medium changes so the game engine knows tunnel/bridge/surface
        char curMedium = result.points[0].medium;
        ExportTrack seg;
        seg.trackId = m_nextTrackId++;
        seg.trackType = 0; // main
        seg.medium = static_cast<uint8_t>(curMedium);
        seg.electrified = 0; // not available from GML
        seg.lineName = name;

        for (const auto& pt : result.points) {
            if (pt.medium != curMedium) {
                // Close current segment, start new one
                if (!seg.x.empty())
                    m_tracks.push_back(std::move(seg));

                curMedium = pt.medium;
                seg = ExportTrack{};
                seg.trackId = m_nextTrackId++;
                seg.trackType = 0;
                seg.medium = static_cast<uint8_t>(curMedium);
                seg.electrified = 0;
                seg.lineName = name;
            }
            seg.x.push_back(static_cast<float>(pt.x));
            seg.y.push_back(static_cast<float>(pt.y));
            seg.z.push_back(static_cast<float>(pt.elevation));
        }
        if (!seg.x.empty())
            m_tracks.push_back(std::move(seg));

        // Collect stations
        for (const auto& st : result.stations) {
            ExportStation es;
            es.name = st.name;
            es.x = 0; es.y = 0; es.z = static_cast<float>(st.elevation);
            es.type = st.type;
            es.lineName = name;

            // Find nearest profile point to get x,y
            double bestDist = 1e18;
            for (const auto& pt : result.points) {
                double d = std::abs(pt.km - st.km);
                if (d < bestDist) {
                    bestDist = d;
                    es.x = static_cast<float>(pt.x);
                    es.y = static_cast<float>(pt.y);
                    es.z = static_cast<float>(pt.elevation);
                }
            }
            m_stations.push_back(std::move(es));
        }

        done++;
        int pct = 5 + 25 * done / static_cast<int>(lineNames.size());
        report(pct, "Profiled " + name + " (" + std::to_string(done) + "/" +
               std::to_string(lineNames.size()) + ")");
    }

    return true;
}

// ─── Phase 3: Profile sidings from OSM ─────────────────────────────

bool GameExporter::ProfileSidings(const std::string& osmDataPath,
                                   ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };

    if (osmDataPath.empty()) return true;

    auto* ds = static_cast<GDALDataset*>(
        GDALOpenEx(osmDataPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return true;

    OGRLayer* layer = ds->GetLayerByName("railway_tracks");
    if (!layer) { GDALClose(ds); return true; }

    layer->SetAttributeFilter(
        "service = 'siding' OR service = 'yard' OR usage = 'industrial'");
    layer->ResetReading();

    // Transform WGS84 → EPSG:25833
    OGRSpatialReference wgs84, utm33;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    utm33.importFromEPSG(25833);
    auto* toUtm = OGRCreateCoordinateTransformation(&wgs84, &utm33);
    if (!toUtm) { GDALClose(ds); return true; }

    constexpr double kDensifySpacing = 25.0;
    constexpr double kMinPlausibleElev = 2.0;
    constexpr double kTrackBedOffset = 0.6;
    constexpr double kMinTrackElev = kMinPlausibleElev + kTrackBedOffset;

    int total = static_cast<int>(layer->GetFeatureCount());
    if (total <= 0) total = 1000; // estimate
    int done = 0;

    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom || wkbFlatten(geom->getGeometryType()) != wkbLineString) {
            OGRFeature::DestroyFeature(feat);
            done++;
            continue;
        }

        auto* ls = static_cast<OGRLineString*>(geom->clone());
        ls->transform(toUtm);

        double length = ls->get_Length();
        if (length < 5.0) {
            OGRGeometryFactory::destroyGeometry(ls);
            OGRFeature::DestroyFeature(feat);
            done++;
            continue;
        }

        int nSamples = std::max(2, static_cast<int>(length / kDensifySpacing) + 1);

        // Determine track type from service field
        const char* service = feat->GetFieldAsString("service");
        uint8_t trackType = 1; // siding
        if (service && std::string(service) == "yard")
            trackType = 2;

        // Check electrification
        const char* elec = feat->GetFieldAsString("electrified");
        uint8_t electrified = 0;
        if (elec && (std::string(elec) == "yes" || std::string(elec) == "contact_line"))
            electrified = 1;

        ExportTrack track;
        track.trackId = m_nextTrackId++;
        track.trackType = trackType;
        track.medium = ' ';
        track.electrified = electrified;

        for (int i = 0; i < nSamples; i++) {
            double frac = static_cast<double>(i) / (nSamples - 1);
            OGRPoint pt;
            ls->Value(frac * length, &pt);

            float elev = 0;
            if (m_profileData.SampleElevation(pt.getX(), pt.getY(), elev)) {
                if (elev < kMinPlausibleElev)
                    elev = -9999;
                else
                    elev += static_cast<float>(kTrackBedOffset);

                if (elev > -9000 && elev < kMinTrackElev)
                    elev = static_cast<float>(kMinTrackElev);
            } else {
                elev = -9999;
            }

            if (elev > -9000) {
                track.x.push_back(static_cast<float>(pt.getX()));
                track.y.push_back(static_cast<float>(pt.getY()));
                track.z.push_back(elev);
            }
        }

        if (track.x.size() >= 2)
            m_tracks.push_back(std::move(track));

        OGRGeometryFactory::destroyGeometry(ls);
        OGRFeature::DestroyFeature(feat);

        done++;
        if (done % 8 == 0 || done == total) {
            int pct = 30 + 15 * done / total;
            report(pct, "Profiling sidings... (" + std::to_string(done) + "/" +
                   std::to_string(total) + ")");
        }
    }

    OCTDestroyCoordinateTransformation(toUtm);
    GDALClose(ds);

    // Snap siding endpoints to main-line vertices
    SnapConnections();

    report(45, "Sidings profiled: " + std::to_string(done) + " tracks");
    return true;
}

// ─── Snap siding endpoints to main-line vertices ───────────────────

void GameExporter::SnapConnections()
{
    constexpr double kSnapTolerance = 50.0;
    constexpr double kSnapTolSq = kSnapTolerance * kSnapTolerance;

    // Collect main-line vertex index for spatial lookup
    // Simple brute force — check first/last point of sidings against all main-line vertices
    // For performance, build a coarse grid

    struct MainVertex {
        float x, y, z;
        uint32_t trackId;
    };
    std::vector<MainVertex> mainVerts;
    for (const auto& t : m_tracks) {
        if (t.trackType != 0) continue;
        if (!t.x.empty()) {
            mainVerts.push_back({t.x.front(), t.y.front(), t.z.front(), t.trackId});
            mainVerts.push_back({t.x.back(), t.y.back(), t.z.back(), t.trackId});
            // Also sample every 10th vertex for better snap coverage
            for (size_t i = 10; i < t.x.size(); i += 10)
                mainVerts.push_back({t.x[i], t.y[i], t.z[i], t.trackId});
        }
    }

    for (const auto& t : m_tracks) {
        if (t.trackType == 0) continue; // skip main lines
        if (t.x.empty()) continue;

        // Check both endpoints
        float endpoints[2][3] = {
            {t.x.front(), t.y.front(), t.z.front()},
            {t.x.back(), t.y.back(), t.z.back()}
        };

        for (int ep = 0; ep < 2; ep++) {
            double bestDist = kSnapTolSq;
            MainVertex bestVert{};
            bool found = false;

            for (const auto& mv : mainVerts) {
                double dx = mv.x - endpoints[ep][0];
                double dy = mv.y - endpoints[ep][1];
                double distSq = dx * dx + dy * dy;
                if (distSq < bestDist) {
                    bestDist = distSq;
                    bestVert = mv;
                    found = true;
                }
            }

            if (found) {
                TrackConnection conn;
                conn.trackIdA = bestVert.trackId;
                conn.trackIdB = t.trackId;
                conn.x = bestVert.x;
                conn.y = bestVert.y;
                conn.z = bestVert.z;
                m_connections.push_back(conn);
            }
        }
    }
}

// ─── Phase 4: Generate tile grid ───────────────────────────────────

bool GameExporter::GenerateTileGrid(ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };
    report(46, "Generating tile grid...");

    const auto& lods = GetLodLevels();

    // Generate tiles from finest LOD to coarsest.
    // For each coarser LOD, only create tiles whose area is not already
    // fully covered by finer-LOD tiles.  We track coverage on the
    // finest grid (LOD0 extent = 2560m) using a set of (col, row) keys.

    double baseExtent = lods[0].tileExtent;  // 2560m
    std::set<std::pair<int,int>> coveredBaseCells;

    for (const auto& lod : lods) {
        double extent = lod.tileExtent;

        int colMin = static_cast<int>(std::floor(m_boundsMinX / extent));
        int colMax = static_cast<int>(std::ceil(m_boundsMaxX / extent));
        int rowMin = static_cast<int>(std::floor(m_boundsMinY / extent));
        int rowMax = static_cast<int>(std::ceil(m_boundsMaxY / extent));

        for (int col = colMin; col < colMax; col++) {
            for (int row = rowMin; row < rowMax; row++) {
                double ox = col * extent;
                double oy = row * extent;

                // Check distance from tile center and corners to nearest rail
                double cx = ox + extent * 0.5;
                double cy = oy + extent * 0.5;
                double dist = MinDistToRail(cx, cy);
                double d2 = MinDistToRail(ox, oy);
                double d3 = MinDistToRail(ox + extent, oy);
                double d4 = MinDistToRail(ox, oy + extent);
                double d5 = MinDistToRail(ox + extent, oy + extent);
                double minDist = std::min({dist, d2, d3, d4, d5});

                if (minDist >= lod.maxDist) continue; // too far
                if (minDist >= lod.minDist) {
                    // This tile is in range for this LOD.
                    // For coarser LODs, check it isn't fully covered already.
                    if (lod.lod > 0) {
                        int bColMin = static_cast<int>(std::floor(ox / baseExtent));
                        int bColMax = static_cast<int>(std::ceil((ox + extent) / baseExtent));
                        int bRowMin = static_cast<int>(std::floor(oy / baseExtent));
                        int bRowMax = static_cast<int>(std::ceil((oy + extent) / baseExtent));
                        bool allCovered = true;
                        for (int bc = bColMin; bc < bColMax && allCovered; bc++)
                            for (int br = bRowMin; br < bRowMax && allCovered; br++)
                                if (coveredBaseCells.find({bc, br}) == coveredBaseCells.end())
                                    allCovered = false;
                        if (allCovered) continue;
                    }

                    ExportTile tile;
                    tile.lod = lod.lod;
                    tile.col = col;
                    tile.row = row;
                    tile.originX = ox;
                    tile.originY = oy;
                    tile.extent = extent;
                    tile.resolution = lod.resolution;
                    m_tiles.push_back(tile);

                    // Mark base cells as covered
                    int bColMin = static_cast<int>(std::floor(ox / baseExtent));
                    int bColMax = static_cast<int>(std::ceil((ox + extent) / baseExtent));
                    int bRowMin = static_cast<int>(std::floor(oy / baseExtent));
                    int bRowMax = static_cast<int>(std::ceil((oy + extent) / baseExtent));
                    for (int bc = bColMin; bc < bColMax; bc++)
                        for (int br = bRowMin; br < bRowMax; br++)
                            coveredBaseCells.insert({bc, br});
                }
            }
        }
    }

    // Sort for spatial coherence (by row-major within each LOD)
    // This improves DTM tile cache locality during heightmap sampling
    std::sort(m_tiles.begin(), m_tiles.end(),
              [](const ExportTile& a, const ExportTile& b) {
                  if (a.lod != b.lod) return a.lod < b.lod;
                  if (a.row != b.row) return a.row < b.row;
                  return a.col < b.col;
              });

    report(50, "Tile grid: " + std::to_string(m_tiles.size()) + " tiles");
    return true;
}

// ─── Phase 5: Write terrain heightmaps ─────────────────────────────

bool GameExporter::WriteTerrainTiles(const std::string& outputDir,
                                      ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };

    constexpr int kTilePixels = 256;
    int total = static_cast<int>(m_tiles.size());
    int done = 0;

    // Optional: open the AR50 land-cover dataset once for per-tile rasterisation.
    GDALDataset* ar50 = nullptr;
    OGRLayer* lcLayer = nullptr;
    OGRCoordinateTransformation* to4258 = nullptr;
    std::string wkt25833;
    if (!m_ar50Path.empty()) {
        ar50 = static_cast<GDALDataset*>(GDALOpenEx(
            m_ar50Path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr));
        if (ar50) {
            lcLayer = ar50->GetLayerByName("ar50");
            OGRSpatialReference srs25833;
            srs25833.importFromEPSG(25833);
            srs25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            char* w = nullptr;
            if (srs25833.exportToWkt(&w) == OGRERR_NONE && w) {
                wkt25833 = w;
                CPLFree(w);
            }
            OGRSpatialReference srs4258;
            srs4258.importFromEPSG(4258);
            srs4258.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            to4258 = OGRCreateCoordinateTransformation(&srs25833, &srs4258);
        }
        if (!lcLayer)
            report(50, "AR50 land cover unavailable; skipping landcover.u8");
    }

    for (auto& tile : m_tiles) {
        // Create tile directory
        std::string tileDir = outputDir + "/tiles/" +
                              std::to_string(tile.lod) + "/" +
                              std::to_string(tile.col) + "_" +
                              std::to_string(tile.row);
        fs::create_directories(tileDir);

        // Sample heightmap
        std::vector<float> heightmap(kTilePixels * kTilePixels, -9999.0f);

        for (int py = 0; py < kTilePixels; py++) {
            // Row 0 = north edge (top), row 255 = south edge (bottom)
            double y = tile.originY + tile.extent - (py + 0.5) * tile.resolution;

            for (int px = 0; px < kTilePixels; px++) {
                double x = tile.originX + (px + 0.5) * tile.resolution;

                float elev;
                if (m_profileData.SampleElevation(x, y, elev))
                    heightmap[py * kTilePixels + px] = elev;
            }
        }

        // Fill nodata gaps with nearest valid neighbor (simple flood fill)
        // First pass: identify gaps
        bool hasGaps = false;
        for (float v : heightmap) {
            if (v < -9000) { hasGaps = true; break; }
        }

        if (hasGaps) {
            // Simple iterative nearest-neighbor fill
            std::vector<float> filled = heightmap;
            for (int iter = 0; iter < 10; iter++) {
                bool changed = false;
                for (int py = 0; py < kTilePixels; py++) {
                    for (int px = 0; px < kTilePixels; px++) {
                        int idx = py * kTilePixels + px;
                        if (filled[idx] > -9000) continue;

                        // Average valid neighbors
                        double sum = 0;
                        int count = 0;
                        if (px > 0 && filled[idx - 1] > -9000)
                            { sum += filled[idx - 1]; count++; }
                        if (px < kTilePixels - 1 && filled[idx + 1] > -9000)
                            { sum += filled[idx + 1]; count++; }
                        if (py > 0 && filled[idx - kTilePixels] > -9000)
                            { sum += filled[idx - kTilePixels]; count++; }
                        if (py < kTilePixels - 1 && filled[idx + kTilePixels] > -9000)
                            { sum += filled[idx + kTilePixels]; count++; }

                        if (count > 0) {
                            filled[idx] = static_cast<float>(sum / count);
                            changed = true;
                        }
                    }
                }
                if (!changed) break;
                heightmap = filled;
            }
            heightmap = filled;
        }

        // Write raw float32
        std::string hmPath = tileDir + "/terrain.hm32";
        std::ofstream out(hmPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(heightmap.data()),
                  heightmap.size() * sizeof(float));
        out.close();

        // Optional per-tile land cover (AR50 artype), same grid as heightmap.
        if (lcLayer) {
            std::vector<uint8_t> landcover;
            if (RasterizeTileLandCover(ar50, lcLayer, wkt25833.c_str(), to4258,
                                       tile, landcover)) {
                std::string lcPath = tileDir + "/landcover.u8";
                std::ofstream lcOut(lcPath, std::ios::binary);
                lcOut.write(reinterpret_cast<const char*>(landcover.data()),
                            landcover.size());
            }
        }

        done++;
        // Report every tile: land-cover rasterisation can take ~1 s each, and
        // the progress callback is what pumps the GUI event loop. Without a
        // frequent call the window stops answering the Wayland/compositor ping
        // and GNOME force-quits it (SIGKILL) as "not responding".
        {
            int pct = 50 + 35 * done / total;
            report(pct, "Writing terrain... (" + std::to_string(done) + "/" +
                   std::to_string(total) + " tiles)");
        }
    }

    if (to4258) OCTDestroyCoordinateTransformation(to4258);
    if (ar50) GDALClose(ar50);

    return true;
}

// ─── Phase 6: Write vector data per tile ───────────────────────────

static void WriteUint32(std::ofstream& out, uint32_t v)
{
    out.write(reinterpret_cast<const char*>(&v), 4);
}

static void WriteUint8(std::ofstream& out, uint8_t v)
{
    out.write(reinterpret_cast<const char*>(&v), 1);
}

static void WriteFloat(std::ofstream& out, float v)
{
    out.write(reinterpret_cast<const char*>(&v), 4);
}

bool GameExporter::WriteVectorData(const std::string& outputDir,
                                    ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };

    int total = static_cast<int>(m_tiles.size());
    int done = 0;

    for (const auto& tile : m_tiles) {
        std::string tileDir = outputDir + "/tiles/" +
                              std::to_string(tile.lod) + "/" +
                              std::to_string(tile.col) + "_" +
                              std::to_string(tile.row);

        double tMaxX = tile.originX + tile.extent;
        double tMaxY = tile.originY + tile.extent;

        // Find tracks intersecting this tile
        std::vector<const ExportTrack*> tileTracks;
        for (const auto& t : m_tracks) {
            if (t.x.empty()) continue;
            // Quick bbox check — find min/max of track vertices
            float txMin = *std::min_element(t.x.begin(), t.x.end());
            float txMax = *std::max_element(t.x.begin(), t.x.end());
            float tyMin = *std::min_element(t.y.begin(), t.y.end());
            float tyMax = *std::max_element(t.y.begin(), t.y.end());

            if (txMin < tMaxX && txMax > tile.originX &&
                tyMin < tMaxY && tyMax > tile.originY)
                tileTracks.push_back(&t);
        }

        // Write tracks.bin
        {
            std::ofstream out(tileDir + "/tracks.bin", std::ios::binary);
            WriteUint32(out, static_cast<uint32_t>(tileTracks.size()));
            for (const auto* t : tileTracks) {
                // Clip vertices to tile bounds
                // For simplicity, include all vertices of intersecting tracks
                // (game engine can clip further if needed)
                WriteUint32(out, t->trackId);
                WriteUint8(out, t->trackType);
                WriteUint8(out, t->medium);
                WriteUint8(out, t->electrified);
                WriteUint8(out, 0); // reserved
                WriteUint32(out, static_cast<uint32_t>(t->x.size()));
                for (size_t i = 0; i < t->x.size(); i++) {
                    WriteFloat(out, t->x[i]);
                    WriteFloat(out, t->y[i]);
                    WriteFloat(out, t->z[i]);
                }
            }
        }

        // Find roads intersecting this tile
        std::vector<const ExportRoad*> tileRoads;
        for (const auto& r : m_roads) {
            if (r.x.empty()) continue;
            float rxMin = *std::min_element(r.x.begin(), r.x.end());
            float rxMax = *std::max_element(r.x.begin(), r.x.end());
            float ryMin = *std::min_element(r.y.begin(), r.y.end());
            float ryMax = *std::max_element(r.y.begin(), r.y.end());

            if (rxMin < tMaxX && rxMax > tile.originX &&
                ryMin < tMaxY && ryMax > tile.originY)
                tileRoads.push_back(&r);
        }

        // Write roads.bin
        {
            std::ofstream out(tileDir + "/roads.bin", std::ios::binary);
            WriteUint32(out, static_cast<uint32_t>(tileRoads.size()));
            for (const auto* r : tileRoads) {
                WriteUint8(out, static_cast<uint8_t>(r->kategori));
                WriteUint8(out, 0);
                WriteUint8(out, 0);
                WriteUint8(out, 0);
                WriteUint32(out, r->nummer);
                WriteUint32(out, static_cast<uint32_t>(r->x.size()));
                for (size_t i = 0; i < r->x.size(); i++) {
                    WriteFloat(out, r->x[i]);
                    WriteFloat(out, r->y[i]);
                    WriteFloat(out, r->z[i]);
                }
            }
        }

        // Find stations in this tile
        std::vector<const ExportStation*> tileStations;
        for (const auto& s : m_stations) {
            if (s.x >= tile.originX && s.x < tMaxX &&
                s.y >= tile.originY && s.y < tMaxY)
                tileStations.push_back(&s);
        }

        // Find connections in this tile
        std::vector<const TrackConnection*> tileConns;
        for (const auto& c : m_connections) {
            if (c.x >= tile.originX && c.x < tMaxX &&
                c.y >= tile.originY && c.y < tMaxY)
                tileConns.push_back(&c);
        }

        // Write meta.json
        {
            std::ofstream out(tileDir + "/meta.json");
            out << "{\n";
            out << "  \"lod\": " << tile.lod << ",\n";
            out << "  \"col\": " << tile.col << ",\n";
            out << "  \"row\": " << tile.row << ",\n";
            out << "  \"originX\": " << std::fixed << tile.originX << ",\n";
            out << "  \"originY\": " << std::fixed << tile.originY << ",\n";
            out << "  \"extent\": " << tile.extent << ",\n";
            out << "  \"resolution\": " << tile.resolution << ",\n";
            out << "  \"pixels\": 256,\n";
            out << "  \"trackSegments\": " << tileTracks.size() << ",\n";
            out << "  \"roadSegments\": " << tileRoads.size() << ",\n";

            // Stations
            out << "  \"stations\": [";
            for (size_t i = 0; i < tileStations.size(); i++) {
                if (i > 0) out << ", ";
                out << "\n    {\"name\": \"" << tileStations[i]->name
                    << "\", \"x\": " << tileStations[i]->x
                    << ", \"y\": " << tileStations[i]->y
                    << ", \"z\": " << tileStations[i]->z
                    << ", \"type\": \"" << tileStations[i]->type
                    << "\", \"line\": \"" << tileStations[i]->lineName << "\"}";
            }
            out << (tileStations.empty() ? "" : "\n  ") << "],\n";

            // Connections
            out << "  \"connections\": [";
            for (size_t i = 0; i < tileConns.size(); i++) {
                if (i > 0) out << ", ";
                out << "\n    {\"trackA\": " << tileConns[i]->trackIdA
                    << ", \"trackB\": " << tileConns[i]->trackIdB
                    << ", \"x\": " << tileConns[i]->x
                    << ", \"y\": " << tileConns[i]->y
                    << ", \"z\": " << tileConns[i]->z << "}";
            }
            out << (tileConns.empty() ? "" : "\n  ") << "]\n";
            out << "}\n";
        }

        done++;
        if (done % 25 == 0 || done == total) {
            int pct = 85 + 8 * done / total;
            report(pct, "Writing vector data... (" + std::to_string(done) + "/" +
                   std::to_string(total) + ")");
        }
    }

    return true;
}

// ─── Load roads ────────────────────────────────────────────────────

static void LoadRoads(const std::string& roadsPath,
                      const std::vector<GameExporter::RailBBox>& railBBoxes,
                      double boundsMinX, double boundsMinY,
                      double boundsMaxX, double boundsMaxY,
                      std::vector<ExportRoad>& roads)
{
    if (roadsPath.empty()) return;

    auto* ds = static_cast<GDALDataset*>(
        GDALOpenEx(roadsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return;

    OGRLayer* layer = ds->GetLayerByName("Veglenke");
    if (!layer) { GDALClose(ds); return; }

    layer->SetSpatialFilterRect(boundsMinX, boundsMinY, boundsMaxX, boundsMaxY);
    layer->ResetReading();

    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom || wkbFlatten(geom->getGeometryType()) != wkbLineString) {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

        const char* katStr = feat->GetFieldAsString("vegkategori");
        char kat = (katStr && katStr[0]) ? katStr[0] : 'F';
        int nummer = feat->GetFieldAsInteger("vegnummer");

        auto* ls = static_cast<OGRLineString*>(geom);
        int nPts = ls->getNumPoints();
        if (nPts < 2) {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

        ExportRoad road;
        road.kategori = kat;
        road.nummer = nummer;
        road.x.reserve(nPts);
        road.y.reserve(nPts);
        road.z.reserve(nPts);

        for (int i = 0; i < nPts; i++) {
            road.x.push_back(static_cast<float>(ls->getX(i)));
            road.y.push_back(static_cast<float>(ls->getY(i)));
            road.z.push_back(static_cast<float>(ls->getZ(i)));
        }

        roads.push_back(std::move(road));
        OGRFeature::DestroyFeature(feat);
    }

    GDALClose(ds);
}

// ─── Phase 7: Write manifest ───────────────────────────────────────

bool GameExporter::WriteManifest(const std::string& outputDir,
                                  ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };
    report(95, "Writing manifest...");

    // Compute stats
    double totalTrackKm = 0;
    int mainLineCount = 0;
    int sidingCount = 0;
    for (const auto& t : m_tracks) {
        double len = 0;
        for (size_t i = 1; i < t.x.size(); i++) {
            double dx = t.x[i] - t.x[i-1];
            double dy = t.y[i] - t.y[i-1];
            len += std::sqrt(dx * dx + dy * dy);
        }
        totalTrackKm += len / 1000.0;
        if (t.trackType == 0) mainLineCount++;
        else sidingCount++;
    }

    // Count unique line names
    std::set<std::string> lineNames;
    for (const auto& t : m_tracks)
        if (t.trackType == 0 && !t.lineName.empty())
            lineNames.insert(t.lineName);

    // Count tiles per LOD
    std::map<int, int> tilesPerLod;
    for (const auto& t : m_tiles)
        tilesPerLod[t.lod]++;

    std::ofstream out(outputDir + "/manifest.json");
    out << "{\n";
    out << "  \"crs\": \"EPSG:25833\",\n";
    out << "  \"origin\": [" << std::fixed << m_boundsMinX << ", " << m_boundsMinY << "],\n";
    out << "  \"bounds\": [" << m_boundsMinX << ", " << m_boundsMinY << ", "
        << m_boundsMaxX << ", " << m_boundsMaxY << "],\n";

    out << "  \"lodLevels\": [\n";
    const auto& lods = GetLodLevels();
    for (size_t i = 0; i < lods.size(); i++) {
        out << "    {\"lod\": " << lods[i].lod
            << ", \"resolution\": " << lods[i].resolution
            << ", \"tileExtent\": " << lods[i].tileExtent
            << ", \"tilePixels\": 256"
            << ", \"count\": " << tilesPerLod[lods[i].lod] << "}";
        if (i + 1 < lods.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"tiles\": [\n";
    for (size_t i = 0; i < m_tiles.size(); i++) {
        const auto& t = m_tiles[i];
        out << "    {\"lod\": " << t.lod
            << ", \"col\": " << t.col
            << ", \"row\": " << t.row
            << ", \"x\": " << t.originX
            << ", \"y\": " << t.originY
            << ", \"path\": \"tiles/" << t.lod << "/" << t.col << "_" << t.row << "/\"}";
        if (i + 1 < m_tiles.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"stats\": {\n";
    out << "    \"totalTiles\": " << m_tiles.size() << ",\n";
    out << "    \"railLines\": " << lineNames.size() << ",\n";
    out << "    \"mainLineSegments\": " << mainLineCount << ",\n";
    out << "    \"sidingSegments\": " << sidingCount << ",\n";
    out << "    \"totalTrackKm\": " << static_cast<int>(totalTrackKm) << ",\n";
    out << "    \"stationCount\": " << m_stations.size() << ",\n";
    out << "    \"connectionCount\": " << m_connections.size() << ",\n";
    out << "    \"roadSegments\": " << m_roads.size() << "\n";
    out << "  }\n";
    out << "}\n";
    out.close();

    report(100, "Export complete: " + std::to_string(m_tiles.size()) + " tiles, " +
           std::to_string(m_tracks.size()) + " track segments");
    return true;
}

// ─── Main export pipeline ──────────────────────────────────────────

bool GameExporter::Export(const std::string& outputDir,
                           const std::string& railwayPath,
                           const std::string& roadsPath,
                           const std::string& osmDataPath,
                           const std::vector<std::string>& zipPaths,
                           const std::string& ar50Path,
                           ProgressCb progress)
{
    m_ar50Path = ar50Path;

    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };

    // Create output directory
    fs::create_directories(outputDir);

    // Build tile index
    report(0, "Building DTM tile index...");
    int nTiles = m_profileData.BuildTileIndex(zipPaths, [&](int cur, int tot) -> bool {
        if (progress) {
            int pct = tot > 0 ? cur * 100 / tot : 0;
            return progress(pct / 50, "Indexing DTM tiles... (" +
                            std::to_string(cur) + "/" + std::to_string(tot) + ")");
        }
        return true;
    });
    if (nTiles == 0) {
        report(0, "No DTM tiles found");
        return false;
    }

    // Phase 1: collect rail geometry
    if (!CollectRailGeometry(railwayPath, osmDataPath, progress))
        return false;
    if (m_railBBoxes.empty()) {
        report(0, "No rail geometry found");
        return false;
    }

    // Phase 2: profile main lines
    if (!ProfileMainLines(railwayPath, roadsPath, progress))
        return false;

    // Phase 3: profile sidings
    if (!ProfileSidings(osmDataPath, progress))
        return false;

    // Load roads within bounds
    report(45, "Loading roads...");
    LoadRoads(roadsPath, m_railBBoxes,
              m_boundsMinX, m_boundsMinY, m_boundsMaxX, m_boundsMaxY,
              m_roads);

    // Phase 4: generate tile grid
    if (!GenerateTileGrid(progress))
        return false;

    // Phase 5: write terrain
    if (!WriteTerrainTiles(outputDir, progress))
        return false;

    // Phase 6: write vector data
    if (!WriteVectorData(outputDir, progress))
        return false;

    // Phase 7: write manifest
    if (!WriteManifest(outputDir, progress))
        return false;

    return true;
}

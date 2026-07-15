#include "GameExport.h"
#include "MainFrame.h"

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <gdal_utils.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

// Parallel tile-writing tuning. Each worker keeps its own DTM tile cache and
// AR50 dataset, so memory grows with the thread count — keep both modest.
constexpr int kMaxExportThreads = 4; // cap regardless of core count
constexpr int kThreadCacheSize = 2;  // DTM cells resident per worker (~100 MB each)

int ExportThreadCount() {
    unsigned hw = std::thread::hardware_concurrency();
    return std::clamp<int>(hw ? static_cast<int>(hw) : 1, 1, kMaxExportThreads);
}

// Spawn `n` worker threads running body(workerId), then join them all.
void RunPool(int n, const std::function<void(int)>& body) {
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (int i = 0; i < n; ++i) pool.emplace_back(body, i);
    for (auto& t : pool) t.join();
}

// Timestamped stdout log for the export pipeline. Flushed immediately so the
// last line survives even if the process is killed or crashes mid-phase — this
// tells you which phase was running. Run terrainmapper from a terminal to see it.
std::mutex g_logMutex;
void ExportLog(const std::string& msg) {
    static const auto start = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - start).count();
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::printf("[export %8.1fs] %s\n", t, msg.c_str());
    std::fflush(stdout);
}

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

void GameExporter::BuildRailIndex()
{
    m_railGrid.clear();
    if (m_railBBoxes.empty()) return;
    m_railGridCell = 10000.0; // 10 km cells
    m_railGridMinX = m_boundsMinX;
    m_railGridMinY = m_boundsMinY;
    m_railGridCols = std::max(1, static_cast<int>(
                         std::ceil((m_boundsMaxX - m_boundsMinX) / m_railGridCell)));
    m_railGridRows = std::max(1, static_cast<int>(
                         std::ceil((m_boundsMaxY - m_boundsMinY) / m_railGridCell)));
    m_railGrid.assign(static_cast<size_t>(m_railGridCols) * m_railGridRows, {});
    auto col = [&](double x) {
        return std::clamp(static_cast<int>((x - m_railGridMinX) / m_railGridCell),
                          0, m_railGridCols - 1);
    };
    auto row = [&](double y) {
        return std::clamp(static_cast<int>((y - m_railGridMinY) / m_railGridCell),
                          0, m_railGridRows - 1);
    };
    for (size_t i = 0; i < m_railBBoxes.size(); ++i) {
        const RailBBox& b = m_railBBoxes[i];
        int c0 = col(b.minX), c1 = col(b.maxX);
        int r0 = row(b.minY), r1 = row(b.maxY);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                m_railGrid[static_cast<size_t>(r) * m_railGridCols + c]
                    .push_back(static_cast<int>(i));
    }
}

double GameExporter::MinDistToRail(double x, double y) const
{
    auto bboxDist = [&](const RailBBox& bb) {
        double dx = std::max({bb.minX - x, 0.0, x - bb.maxX});
        double dy = std::max({bb.minY - y, 0.0, y - bb.maxY});
        return std::sqrt(dx * dx + dy * dy);
    };

    if (m_railGrid.empty()) { // no index: brute force
        double best = 1e18;
        for (const auto& bb : m_railBBoxes) {
            double d = bboxDist(bb);
            if (d < best) best = d;
            if (best == 0) return 0;
        }
        return best;
    }

    // Only search cells within the largest LOD distance (~20 km) of the point.
    const int radius = static_cast<int>(std::ceil(20000.0 / m_railGridCell)) + 1;
    int qc = std::clamp(static_cast<int>((x - m_railGridMinX) / m_railGridCell),
                        0, m_railGridCols - 1);
    int qr = std::clamp(static_cast<int>((y - m_railGridMinY) / m_railGridCell),
                        0, m_railGridRows - 1);
    double best = 1e18;
    for (int r = qr - radius; r <= qr + radius; ++r) {
        if (r < 0 || r >= m_railGridRows) continue;
        for (int c = qc - radius; c <= qc + radius; ++c) {
            if (c < 0 || c >= m_railGridCols) continue;
            for (int idx : m_railGrid[static_cast<size_t>(r) * m_railGridCols + c]) {
                double d = bboxDist(m_railBBoxes[idx]);
                if (d < best) best = d;
                if (best == 0) return 0;
            }
        }
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

    report(1, "Collecting rail geometry...");

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

    report(3, "Rail geometry collected: " + std::to_string(m_railBBoxes.size()) +
              " segments");
    return true;
}

// ─── Phase 3b: Match OSM maxspeed onto main-line track vertices ─────
//
// Ports the matching in ProfileView::PopulateSpeedLimits: OSM `railway_tracks`
// vertices that carry a `maxspeed` (and are not service=siding/yard) are snapped
// to the nearest main-line track vertex within 30 m. Short unknown gaps between
// equal speeds are filled. Sidings/yards keep an all-zero (unknown) speed array.
bool GameExporter::MatchSpeedLimits(const std::string& osmDataPath,
                                    ProgressCb progress)
{
    // Every track gets a speed array sized to its vertices, default 0 = unknown.
    for (auto& t : m_tracks) t.speed.assign(t.x.size(), 0);

    if (osmDataPath.empty()) return true;
    auto* ds = static_cast<GDALDataset*>(
        GDALOpenEx(osmDataPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return true;
    OGRLayer* layer = ds->GetLayerByName("railway_tracks");
    if (!layer) { GDALClose(ds); return true; }

    if (progress) progress(34, "Matching OSM speed limits...");

    OGRSpatialReference wgs84, utm33;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    utm33.importFromEPSG(25833);
    utm33.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto* toUtm = OGRCreateCoordinateTransformation(&wgs84, &utm33);

    // Collect all main-line OSM speed vertices (UTM33) once.
    struct SpeedPt { double x, y; uint16_t speed; };
    std::vector<SpeedPt> pts;
    layer->ResetReading();
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        int msIdx = feat->GetFieldIndex("maxspeed");
        int speed = 0;
        if (msIdx >= 0 && feat->IsFieldSetAndNotNull(msIdx))
            speed = feat->GetFieldAsInteger(msIdx);
        const char* service = feat->GetFieldAsString("service");
        if (speed <= 0 || (service && service[0])) { // skip unknown / sidings
            OGRFeature::DestroyFeature(feat);
            continue;
        }
        OGRGeometry* geom = feat->GetGeometryRef();
        if (geom && toUtm) {
            OGRGeometry* clone = geom->clone();
            clone->transform(toUtm);
            auto addLine = [&](OGRLineString* ls) {
                for (int p = 0, n = ls->getNumPoints(); p < n; ++p)
                    pts.push_back({ls->getX(p), ls->getY(p),
                                   static_cast<uint16_t>(std::min(speed, 65535))});
            };
            OGRwkbGeometryType gt = wkbFlatten(clone->getGeometryType());
            if (gt == wkbLineString)
                addLine(static_cast<OGRLineString*>(clone));
            else if (gt == wkbMultiLineString) {
                auto* m = static_cast<OGRMultiLineString*>(clone);
                for (int g = 0; g < m->getNumGeometries(); ++g)
                    addLine(static_cast<OGRLineString*>(m->getGeometryRef(g)));
            }
            OGRGeometryFactory::destroyGeometry(clone);
        }
        OGRFeature::DestroyFeature(feat);
    }
    if (toUtm) OCTDestroyCoordinateTransformation(toUtm);
    GDALClose(ds);
    if (pts.empty()) return true;

    // Uniform grid (cell == match radius) over the speed points.
    constexpr double kMatchRadius = 30.0;
    auto cellOf = [](double x, double y) {
        return std::make_pair(static_cast<long>(std::floor(x / kMatchRadius)),
                              static_cast<long>(std::floor(y / kMatchRadius)));
    };
    struct PairHash {
        size_t operator()(const std::pair<long, long>& p) const {
            return std::hash<long>()(p.first) * 1000003u ^
                   std::hash<long>()(p.second);
        }
    };
    std::unordered_map<std::pair<long, long>, std::vector<int>, PairHash> grid;
    for (int i = 0; i < static_cast<int>(pts.size()); ++i)
        grid[cellOf(pts[i].x, pts[i].y)].push_back(i);

    const double r2 = kMatchRadius * kMatchRadius;
    for (auto& t : m_tracks) {
        if (t.trackType != 0) continue; // main line only
        // Nearest speed point within radius per vertex.
        for (size_t k = 0; k < t.x.size(); ++k) {
            const double vx = t.x[k], vy = t.y[k];
            const auto c = cellOf(vx, vy);
            double best = r2;
            int bestSpeed = 0;
            for (long dx = -1; dx <= 1; ++dx)
                for (long dy = -1; dy <= 1; ++dy) {
                    auto it = grid.find({c.first + dx, c.second + dy});
                    if (it == grid.end()) continue;
                    for (int idx : it->second) {
                        const double ex = pts[idx].x - vx, ey = pts[idx].y - vy;
                        const double d2 = ex * ex + ey * ey;
                        if (d2 < best) { best = d2; bestSpeed = pts[idx].speed; }
                    }
                }
            if (bestSpeed > 0) t.speed[k] = static_cast<uint16_t>(bestSpeed);
        }
        // Fill short unknown gaps (<=5) bounded by equal known speeds.
        const int n = static_cast<int>(t.speed.size());
        for (int i = 0; i < n;) {
            if (t.speed[i] != 0) { ++i; continue; }
            int j = i;
            while (j < n && t.speed[j] == 0) ++j;
            const uint16_t left = (i > 0) ? t.speed[i - 1] : 0;
            const uint16_t right = (j < n) ? t.speed[j] : 0;
            if (j - i <= 5 && left != 0 && left == right)
                for (int f = i; f < j; ++f) t.speed[f] = left;
            i = j;
        }
    }
    return true;
}

// ─── Phase 3c: Collect OSM building footprints ─────────────────────
//
// Reads the OSM `buildings` polygon layer (as MapView::LoadBuildings does),
// transforms exterior rings to EPSG:25833, samples the DTM for a ground base
// elevation, derives height from `levels` (default 2 storeys), and classifies
// the `building` tag into a coarse kind for colouring. Runs on the main thread
// after profiling so it can share m_profileData's DTM sampler safely.
bool GameExporter::CollectBuildings(const std::string& osmDataPath,
                                    ProgressCb progress)
{
    if (osmDataPath.empty()) return true;
    auto* ds = static_cast<GDALDataset*>(
        GDALOpenEx(osmDataPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return true;
    OGRLayer* layer = ds->GetLayerByName("buildings");
    if (!layer) { GDALClose(ds); return true; }

    if (progress) progress(34, "Collecting OSM buildings...");

    OGRSpatialReference wgs84, utm33;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    utm33.importFromEPSG(25833);
    utm33.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto* toUtm = OGRCreateCoordinateTransformation(&wgs84, &utm33);

    // Spatial filter: export bounds (UTM33) transformed back to WGS84.
    if (auto* toWgs = OGRCreateCoordinateTransformation(&utm33, &wgs84)) {
        double x1 = m_boundsMinX, y1 = m_boundsMinY;
        double x2 = m_boundsMaxX, y2 = m_boundsMaxY;
        toWgs->Transform(1, &x1, &y1);
        toWgs->Transform(1, &x2, &y2);
        layer->SetSpatialFilterRect(std::min(x1, x2), std::min(y1, y2),
                                    std::max(x1, x2), std::max(y1, y2));
        OCTDestroyCoordinateTransformation(toWgs);
    }

    auto classify = [](const char* b) -> uint8_t {
        if (!b || !b[0]) return 0;
        const std::string s(b);
        auto in = [&](std::initializer_list<const char*> ks) {
            for (const char* k : ks) if (s == k) return true;
            return false;
        };
        if (in({"house", "residential", "apartments", "detached", "terrace",
                "semidetached_house", "bungalow", "cabin", "hut", "dormitory",
                "houseboat", "farm", "static_caravan"}))
            return 1;
        if (in({"commercial", "retail", "office", "hotel", "supermarket",
                "kiosk", "shop"}))
            return 2;
        if (in({"industrial", "warehouse", "factory", "hangar", "manufacture"}))
            return 3;
        return 0;
    };

    auto classifyRoof = [](const char* r) -> uint8_t {
        if (!r || !r[0]) return 0; // flat / untagged
        const std::string s(r);
        auto in = [&](std::initializer_list<const char*> ks) {
            for (const char* k : ks) if (s == k) return true;
            return false;
        };
        if (in({"gabled", "gambrel", "saltbox", "round"})) return 1;
        if (in({"hipped", "half-hipped", "mansard"})) return 2;
        if (in({"pyramidal", "dome", "onion", "cone", "conical"})) return 3;
        if (in({"skillion", "lean_to", "mono_pitch", "monopitch"})) return 4;
        return 0;
    };

    auto addRing = [&](OGRLinearRing* ring, uint8_t kind, uint8_t roof,
                       float height) {
        if (!ring) return;
        int n = ring->getNumPoints();
        if (n >= 2) { // OGR rings are closed (last == first); drop the duplicate
            if (ring->getX(0) == ring->getX(n - 1) &&
                ring->getY(0) == ring->getY(n - 1))
                --n;
        }
        if (n < 3) return;
        ExportBuilding b;
        b.kind = kind;
        b.roof = roof;
        b.height = height;
        b.x.reserve(n);
        b.y.reserve(n);
        float baseZ = 1e30f;
        bool anyZ = false;
        for (int i = 0; i < n; ++i) {
            b.x.push_back(static_cast<float>(ring->getX(i)));
            b.y.push_back(static_cast<float>(ring->getY(i)));
            float e;
            if (m_profileData.SampleElevation(ring->getX(i), ring->getY(i), e)) {
                baseZ = std::min(baseZ, e);
                anyZ = true;
            }
        }
        if (!anyZ) return; // no DTM coverage under the footprint
        b.baseZ = baseZ;
        m_buildings.push_back(std::move(b));
    };

    layer->ResetReading();
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        int levels = 0;
        const int li = feat->GetFieldIndex("levels");
        if (li >= 0 && feat->IsFieldSetAndNotNull(li))
            levels = feat->GetFieldAsInteger(li);
        float height = static_cast<float>(levels > 0 ? levels : 2) * 3.0f;
        height = std::clamp(height, 3.0f, 180.0f);
        const uint8_t kind = classify(feat->GetFieldAsString("building"));
        const uint8_t roof = classifyRoof(feat->GetFieldAsString("roof_shape"));

        OGRGeometry* geom = feat->GetGeometryRef();
        if (geom && toUtm) {
            OGRGeometry* clone = geom->clone();
            clone->transform(toUtm);
            const OGRwkbGeometryType gt = wkbFlatten(clone->getGeometryType());
            if (gt == wkbPolygon)
                addRing(static_cast<OGRPolygon*>(clone)->getExteriorRing(), kind,
                        roof, height);
            else if (gt == wkbMultiPolygon) {
                auto* mp = static_cast<OGRMultiPolygon*>(clone);
                for (int g = 0; g < mp->getNumGeometries(); ++g)
                    addRing(static_cast<OGRPolygon*>(mp->getGeometryRef(g))
                                ->getExteriorRing(),
                            kind, roof, height);
            }
            OGRGeometryFactory::destroyGeometry(clone);
        }
        OGRFeature::DestroyFeature(feat);
    }
    if (toUtm) OCTDestroyCoordinateTransformation(toUtm);
    GDALClose(ds);
    ExportLog("collected " + std::to_string(m_buildings.size()) + " buildings");
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
    report(3, "Profiling " + std::to_string(lineNames.size()) + " railway lines...");

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
        int pct = 3 + 19 * done / static_cast<int>(lineNames.size());
        if (!report(pct, "Profiled " + name + " (" + std::to_string(done) + "/" +
                         std::to_string(lineNames.size()) + ")"))
            return false;
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
            int pct = 22 + 12 * done / total;
            if (!report(pct, "Profiling sidings... (" + std::to_string(done) +
                             "/" + std::to_string(total) + ")"))
                return false;
        }
    }

    OCTDestroyCoordinateTransformation(toUtm);
    GDALClose(ds);

    // Snap siding endpoints to main-line vertices
    SnapConnections();

    report(34, "Sidings profiled: " + std::to_string(done) + " tracks");
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

    // Spatial index so the per-cell rail-distance queries below are fast.
    BuildRailIndex();

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

// Samples one tile's gap-filled heightmap and optional AR50 land cover, and
// writes terrain.hm32 (+ landcover.u8). Uses the caller-provided per-thread
// elevation sampler and AR50 handle, so it is safe to run concurrently.
static void WriteOneTerrainTile(const ExportTile& tile, ProfileData& sampler,
                                GDALDataset* ar50, OGRLayer* lcLayer,
                                const char* wkt25833,
                                OGRCoordinateTransformation* to4258,
                                const std::string& outputDir)
{
    constexpr int kTilePixels = 256;
    std::string tileDir = outputDir + "/tiles/" + std::to_string(tile.lod) + "/" +
                          std::to_string(tile.col) + "_" + std::to_string(tile.row);
    fs::create_directories(tileDir);

    // Sample heightmap (row 0 = north edge).
    std::vector<float> heightmap(kTilePixels * kTilePixels, -9999.0f);
    for (int py = 0; py < kTilePixels; py++) {
        double y = tile.originY + tile.extent - (py + 0.5) * tile.resolution;
        for (int px = 0; px < kTilePixels; px++) {
            double x = tile.originX + (px + 0.5) * tile.resolution;
            float elev;
            if (sampler.SampleElevation(x, y, elev))
                heightmap[py * kTilePixels + px] = elev;
        }
    }

    // Fill nodata gaps with iterative nearest-neighbour averaging.
    bool hasGaps = false;
    for (float v : heightmap) {
        if (v < -9000) { hasGaps = true; break; }
    }
    if (hasGaps) {
        std::vector<float> filled = heightmap;
        for (int iter = 0; iter < 10; iter++) {
            bool changed = false;
            for (int py = 0; py < kTilePixels; py++) {
                for (int px = 0; px < kTilePixels; px++) {
                    int idx = py * kTilePixels + px;
                    if (filled[idx] > -9000) continue;
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

    {
        std::ofstream out(tileDir + "/terrain.hm32", std::ios::binary);
        out.write(reinterpret_cast<const char*>(heightmap.data()),
                  heightmap.size() * sizeof(float));
    }

    if (lcLayer) {
        std::vector<uint8_t> landcover;
        if (RasterizeTileLandCover(ar50, lcLayer, wkt25833, to4258, tile,
                                   landcover)) {
            std::ofstream lcOut(tileDir + "/landcover.u8", std::ios::binary);
            lcOut.write(reinterpret_cast<const char*>(landcover.data()),
                        landcover.size());
        }
    }
}

bool GameExporter::WriteTerrainTiles(const std::string& outputDir,
                                      ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };

    const int total = static_cast<int>(m_tiles.size());

    // Probe AR50 once to validate the layer and capture the shared EPSG:25833
    // WKT. Per-worker datasets + transforms are opened (serially) further down.
    std::string wkt25833;
    bool useAr50 = false;
    if (!m_ar50Path.empty()) {
        GDALDataset* probe = static_cast<GDALDataset*>(GDALOpenEx(
            m_ar50Path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr));
        if (probe) {
            if (probe->GetLayerByName("ar50")) {
                OGRSpatialReference srs;
                srs.importFromEPSG(25833);
                srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                char* w = nullptr;
                if (srs.exportToWkt(&w) == OGRERR_NONE && w) {
                    wkt25833 = w;
                    CPLFree(w);
                }
                useAr50 = true;
            }
            GDALClose(probe);
        }
        if (!useAr50)
            report(35, "AR50 land cover unavailable; skipping landcover.u8");
    }

    // Parallel per-tile writing. Tiles are independent (each writes its own
    // directory), so workers pull from a shared atomic index. Each worker owns
    // its elevation sampler and AR50 handle for thread safety.
    std::atomic<int> nextTile{0};
    std::atomic<int> done{0};
    std::atomic<bool> cancelled{false};
    std::mutex reportMutex;

    const int nThreads = ExportThreadCount();
    ExportLog("terrain: " + std::to_string(total) + " tiles across " +
              std::to_string(nThreads) + " threads" +
              (useAr50 ? " (+ land cover)" : ""));

    // Pre-open one AR50 dataset + transform per worker, SERIALLY, and force the
    // layer definition to load now. OpenFileGDB is unreliable when the same
    // .gdb is opened/parsed from several threads at once: that raced the lazy
    // field-definition load and produced intermittent "Failed to find field
    // artype" errors, leaving those tiles with no (all-zero) land cover.
    std::vector<GDALDataset*> ar50DS(nThreads, nullptr);
    std::vector<OGRLayer*> ar50Layer(nThreads, nullptr);
    std::vector<OGRCoordinateTransformation*> ar50Tr(nThreads, nullptr);
    if (useAr50) {
        for (int t = 0; t < nThreads; ++t) {
            ar50DS[t] = static_cast<GDALDataset*>(GDALOpenEx(
                m_ar50Path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                nullptr, nullptr, nullptr));
            if (!ar50DS[t]) continue;
            ar50Layer[t] = ar50DS[t]->GetLayerByName("ar50");
            if (ar50Layer[t]) {
                // Touch the definition now so the field lookup is cached.
                (void)ar50Layer[t]->GetLayerDefn()->GetFieldIndex("artype");
            }
            OGRSpatialReference s25833, s4258;
            s25833.importFromEPSG(25833);
            s25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            s4258.importFromEPSG(4258);
            s4258.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            ar50Tr[t] = OGRCreateCoordinateTransformation(&s25833, &s4258);
        }
    }

    RunPool(nThreads, [&](int wid) {
        ProfileData sampler = m_profileData.forThread(kThreadCacheSize);
        GDALDataset* ar50 = ar50DS[wid];
        OGRLayer* lcLayer = ar50Layer[wid];
        OGRCoordinateTransformation* to4258 = ar50Tr[wid];

        for (;;) {
            if (cancelled.load(std::memory_order_relaxed)) break;
            int i = nextTile.fetch_add(1);
            if (i >= total) break;

            WriteOneTerrainTile(m_tiles[i], sampler, ar50, lcLayer,
                                wkt25833.c_str(), to4258, outputDir);

            int d = done.fetch_add(1) + 1;
            if (d % 1000 == 0 || d == total)
                ExportLog("terrain tiles " + std::to_string(d) + "/" +
                          std::to_string(total));
            if (d % 16 == 0 || d == total) {
                std::lock_guard<std::mutex> lk(reportMutex);
                int pct = 35 + 54 * d / total;
                if (!report(pct, "Writing terrain... (" + std::to_string(d) +
                                     "/" + std::to_string(total) + " tiles)"))
                    cancelled.store(true, std::memory_order_relaxed);
            }
        }
    });

    for (int t = 0; t < nThreads; ++t) {
        if (ar50Tr[t]) OCTDestroyCoordinateTransformation(ar50Tr[t]);
        if (ar50DS[t]) GDALClose(ar50DS[t]);
    }

    ExportLog("terrain done: " + std::to_string(done.load()) + " tiles" +
              std::string(cancelled.load() ? " (cancelled)" : ""));
    return !cancelled.load();
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

static void WriteUint16(std::ofstream& out, uint16_t v)
{
    out.write(reinterpret_cast<const char*>(&v), 2);
}

static void WriteFloat(std::ofstream& out, float v)
{
    out.write(reinterpret_cast<const char*>(&v), 4);
}

void GameExporter::BuildVectorIndex()
{
    auto boundsOf = [](const std::vector<float>& xs,
                       const std::vector<float>& ys) -> BBox2D {
        BBox2D b{std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max()};
        for (size_t k = 0; k < xs.size(); ++k) {
            b.minX = std::min(b.minX, xs[k]);
            b.maxX = std::max(b.maxX, xs[k]);
            b.minY = std::min(b.minY, ys[k]);
            b.maxY = std::max(b.maxY, ys[k]);
        }
        return b;
    };
    m_trackBBox.resize(m_tracks.size());
    for (size_t i = 0; i < m_tracks.size(); ++i)
        m_trackBBox[i] = boundsOf(m_tracks[i].x, m_tracks[i].y);
    m_roadBBox.resize(m_roads.size());
    for (size_t i = 0; i < m_roads.size(); ++i)
        m_roadBBox[i] = boundsOf(m_roads[i].x, m_roads[i].y);
    m_buildingBBox.resize(m_buildings.size());
    for (size_t i = 0; i < m_buildings.size(); ++i)
        m_buildingBBox[i] = boundsOf(m_buildings[i].x, m_buildings[i].y);

    // Uniform grid over the export bounds indexing roads by cell.
    m_roadGrid.clear();
    if (m_roads.empty()) return;
    m_gridCell = 4096.0;
    m_gridMinX = m_boundsMinX;
    m_gridMinY = m_boundsMinY;
    m_gridCols = std::max(1, static_cast<int>(
                     std::ceil((m_boundsMaxX - m_boundsMinX) / m_gridCell)));
    m_gridRows = std::max(1, static_cast<int>(
                     std::ceil((m_boundsMaxY - m_boundsMinY) / m_gridCell)));
    m_roadGrid.assign(static_cast<size_t>(m_gridCols) * m_gridRows, {});
    auto col = [&](double x) {
        return std::clamp(static_cast<int>((x - m_gridMinX) / m_gridCell), 0,
                          m_gridCols - 1);
    };
    auto row = [&](double y) {
        return std::clamp(static_cast<int>((y - m_gridMinY) / m_gridCell), 0,
                          m_gridRows - 1);
    };
    for (size_t i = 0; i < m_roadBBox.size(); ++i) {
        const BBox2D& b = m_roadBBox[i];
        if (b.minX > b.maxX) continue; // empty geometry
        int c0 = col(b.minX), c1 = col(b.maxX);
        int r0 = row(b.minY), r1 = row(b.maxY);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                m_roadGrid[static_cast<size_t>(r) * m_gridCols + c]
                    .push_back(static_cast<int>(i));
    }
    ExportLog("indexed " + std::to_string(m_roads.size()) + " roads into a " +
              std::to_string(m_gridCols) + "x" + std::to_string(m_gridRows) +
              " grid");
}

void GameExporter::WriteOneVectorTile(const ExportTile& tile,
                                      const std::string& outputDir,
                                      std::vector<int>& seenRoads,
                                      int tileId) const
{
        std::string tileDir = outputDir + "/tiles/" +
                              std::to_string(tile.lod) + "/" +
                              std::to_string(tile.col) + "_" +
                              std::to_string(tile.row);

        double tMaxX = tile.originX + tile.extent;
        double tMaxY = tile.originY + tile.extent;

        // Find tracks intersecting this tile (few thousand; flat bbox scan).
        std::vector<const ExportTrack*> tileTracks;
        for (size_t i = 0; i < m_tracks.size(); i++) {
            const BBox2D& bb = m_trackBBox[i];
            if (bb.minX < tMaxX && bb.maxX > tile.originX &&
                bb.minY < tMaxY && bb.maxY > tile.originY)
                tileTracks.push_back(&m_tracks[i]);
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
                // Per-vertex OSM speed (km/h, 0 = unknown).
                for (size_t i = 0; i < t->x.size(); i++)
                    WriteUint16(out, i < t->speed.size() ? t->speed[i] : 0);
            }
        }

        // Find roads intersecting this tile via the uniform grid (millions of
        // segments). seenRoads de-duplicates candidates that span several cells.
        std::vector<const ExportRoad*> tileRoads;
        if (!m_roadGrid.empty()) {
            auto col = [&](double x) {
                return std::clamp(static_cast<int>((x - m_gridMinX) / m_gridCell),
                                  0, m_gridCols - 1);
            };
            auto row = [&](double y) {
                return std::clamp(static_cast<int>((y - m_gridMinY) / m_gridCell),
                                  0, m_gridRows - 1);
            };
            int c0 = col(tile.originX), c1 = col(tMaxX);
            int r0 = row(tile.originY), r1 = row(tMaxY);
            for (int r = r0; r <= r1; r++) {
                for (int c = c0; c <= c1; c++) {
                    for (int idx : m_roadGrid[static_cast<size_t>(r) * m_gridCols + c]) {
                        if (seenRoads[idx] == tileId) continue;
                        seenRoads[idx] = tileId;
                        const BBox2D& bb = m_roadBBox[idx];
                        if (bb.minX < tMaxX && bb.maxX > tile.originX &&
                            bb.minY < tMaxY && bb.maxY > tile.originY)
                            tileRoads.push_back(&m_roads[idx]);
                    }
                }
            }
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

        // Find buildings intersecting this tile (flat bbox scan; far fewer than
        // roads) and write buildings.bin.
        std::vector<const ExportBuilding*> tileBuildings;
        for (size_t i = 0; i < m_buildings.size(); i++) {
            const BBox2D& bb = m_buildingBBox[i];
            if (bb.minX < tMaxX && bb.maxX > tile.originX &&
                bb.minY < tMaxY && bb.maxY > tile.originY)
                tileBuildings.push_back(&m_buildings[i]);
        }
        {
            std::ofstream out(tileDir + "/buildings.bin", std::ios::binary);
            WriteUint32(out, static_cast<uint32_t>(tileBuildings.size()));
            for (const auto* b : tileBuildings) {
                WriteUint8(out, b->kind);
                WriteUint8(out, b->roof); // reserved[0] = roof shape
                WriteUint8(out, 0);
                WriteUint8(out, 0);
                WriteFloat(out, b->baseZ);
                WriteFloat(out, b->height);
                WriteUint32(out, static_cast<uint32_t>(b->x.size()));
                for (size_t i = 0; i < b->x.size(); i++) {
                    WriteFloat(out, b->x[i]);
                    WriteFloat(out, b->y[i]);
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
            out << "  \"buildingSegments\": " << tileBuildings.size() << ",\n";

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

}

bool GameExporter::WriteVectorData(const std::string& outputDir,
                                    ProgressCb progress)
{
    auto report = [&](int pct, const std::string& msg) -> bool {
        return !progress || progress(pct, msg);
    };
    const int total = static_cast<int>(m_tiles.size());

    // Precompute track/road bounds + the road grid so per-tile clipping is fast.
    BuildVectorIndex();

    // Per-tile writes read only immutable geometry + indexes, so run in parallel.
    std::atomic<int> nextTile{0};
    std::atomic<int> done{0};
    std::atomic<bool> cancelled{false};
    std::mutex reportMutex;

    const int nThreads = ExportThreadCount();
    ExportLog("vector data: " + std::to_string(total) + " tiles across " +
              std::to_string(nThreads) + " threads");

    RunPool(nThreads, [&](int) {
        std::vector<int> seen(m_roads.size(), -1); // per-thread road dedup markers
        for (;;) {
            if (cancelled.load(std::memory_order_relaxed)) break;
            int i = nextTile.fetch_add(1);
            if (i >= total) break;
            WriteOneVectorTile(m_tiles[i], outputDir, seen, i);
            int d = done.fetch_add(1) + 1;
            if (d % 64 == 0 || d == total) {
                std::lock_guard<std::mutex> lk(reportMutex);
                int pct = 89 + 6 * d / total;
                if (!report(pct, "Writing vector data... (" + std::to_string(d) +
                                     "/" + std::to_string(total) + ")"))
                    cancelled.store(true, std::memory_order_relaxed);
            }
        }
    });

    ExportLog("vector data done: " + std::to_string(done.load()) + " tiles" +
              std::string(cancelled.load() ? " (cancelled)" : ""));
    return !cancelled.load();
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

    // OnLoadTransport converts the road network into a GPKG layer named
    // "roads"; fall back to the raw "Veglenke" GML name, then the first layer.
    OGRLayer* layer = ds->GetLayerByName("roads");
    if (!layer) layer = ds->GetLayerByName("Veglenke");
    if (!layer && ds->GetLayerCount() > 0) layer = ds->GetLayer(0);
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
    ExportLog("started -> " + outputDir +
              (ar50Path.empty() ? " (no land cover)" : " (AR50 land cover)"));

    // Build tile index
    ExportLog("phase 0: building DTM tile index");
    report(0, "Building DTM tile index...");
    int nTiles = m_profileData.BuildTileIndex(zipPaths, [&](int cur, int tot) -> bool {
        if (progress) {
            int pct = tot > 0 ? cur * 100 / tot : 0;
            return progress(pct / 100, "Indexing DTM tiles... (" +
                            std::to_string(cur) + "/" + std::to_string(tot) + ")");
        }
        return true;
    });
    if (nTiles == 0) {
        ExportLog("aborted: no DTM tiles found");
        report(0, "No DTM tiles found");
        return false;
    }
    ExportLog("indexed " + std::to_string(nTiles) + " DTM tiles");

    // Phase 1: collect rail geometry
    ExportLog("phase 1: collecting rail geometry");
    if (!CollectRailGeometry(railwayPath, osmDataPath, progress)) {
        ExportLog("phase 1 failed/cancelled");
        return false;
    }
    if (m_railBBoxes.empty()) {
        ExportLog("aborted: no rail geometry found");
        report(0, "No rail geometry found");
        return false;
    }

    // Rail bboxes + bounds are final after phase 1. Loading roads and generating
    // the tile grid depend only on those (not on profiling), so run them —
    // roads first, then the tile grid — on one background thread while the slow
    // profiling phases run on this thread. Separate data + GDAL datasets, so no
    // shared mutable state.
    std::thread bgThread;
    std::atomic<bool> bgOk{true};
    bgThread = std::thread([this, roadsPath, &bgOk] {
        if (!roadsPath.empty()) {
            ExportLog("phase 4: loading roads (background)");
            LoadRoads(roadsPath, m_railBBoxes,
                      m_boundsMinX, m_boundsMinY, m_boundsMaxX, m_boundsMaxY,
                      m_roads);
            ExportLog("loaded " + std::to_string(m_roads.size()) +
                      " road segments");
        }
        ExportLog("phase 5: generating tile grid (background)");
        bgOk = GenerateTileGrid(nullptr); // fast; progress bar driven by profiling
        ExportLog("tile grid: " + std::to_string(m_tiles.size()) + " tiles");
    });
    // Join the background thread on any exit path (early return / exception).
    struct BgJoin {
        std::thread& t;
        ~BgJoin() { if (t.joinable()) t.join(); }
    } bgJoin{bgThread};

    // Phase 2: profile main lines
    ExportLog("phase 2: profiling main lines");
    if (!ProfileMainLines(railwayPath, roadsPath, progress)) {
        ExportLog("phase 2 failed/cancelled");
        return false;
    }

    // Phase 3: profile sidings
    ExportLog("phase 3: profiling sidings");
    if (!ProfileSidings(osmDataPath, progress)) {
        ExportLog("phase 3 failed/cancelled");
        return false;
    }

    // Phase 3b: match OSM speed limits onto main-line tracks
    ExportLog("phase 3b: matching OSM speed limits");
    MatchSpeedLimits(osmDataPath, progress);

    // Phase 3c: collect OSM buildings (main thread — shares m_profileData's DTM
    // sampler with profiling, which has finished).
    ExportLog("phase 3c: collecting OSM buildings");
    CollectBuildings(osmDataPath, progress);

    // Wait for the background roads + tile grid to finish.
    if (bgThread.joinable()) bgThread.join();
    if (!bgOk) {
        ExportLog("tile grid failed");
        return false;
    }

    // Phase 6: write terrain heightmaps (+ land cover)
    ExportLog("phase 6: writing terrain tiles");
    if (!WriteTerrainTiles(outputDir, progress)) {
        ExportLog("phase 6 failed/cancelled");
        return false;
    }

    // Phase 7: write vector data
    ExportLog("phase 7: writing vector data");
    if (!WriteVectorData(outputDir, progress)) {
        ExportLog("phase 7 failed/cancelled");
        return false;
    }

    // Phase 8: write manifest
    ExportLog("phase 8: writing manifest");
    if (!WriteManifest(outputDir, progress)) {
        ExportLog("phase 8 failed/cancelled");
        return false;
    }

    ExportLog("export finished OK");
    return true;
}

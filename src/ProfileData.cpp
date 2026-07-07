#include "ProfileData.h"
#include "MainFrame.h"

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogrsf_frmts.h>

#include <zip.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

// ─── CachedTile lifecycle ───────────────────────────────────────────

CachedTile::~CachedTile()
{
    if (toTileCrs)
        OCTDestroyCoordinateTransformation(toTileCrs);
}

CachedTile::CachedTile(CachedTile&& o) noexcept
    : indexEntry(o.indexEntry), elevation(std::move(o.elevation)),
      w(o.w), h(o.h), projectionWkt(std::move(o.projectionWkt)),
      needsTransform(o.needsTransform), toTileCrs(o.toTileCrs)
{
    std::copy(std::begin(o.gt), std::end(o.gt), gt);
    std::copy(std::begin(o.invGt), std::end(o.invGt), invGt);
    o.toTileCrs = nullptr;
}

CachedTile& CachedTile::operator=(CachedTile&& o) noexcept
{
    if (this != &o) {
        if (toTileCrs) OCTDestroyCoordinateTransformation(toTileCrs);
        indexEntry = o.indexEntry;
        elevation = std::move(o.elevation);
        w = o.w; h = o.h;
        std::copy(std::begin(o.gt), std::end(o.gt), gt);
        std::copy(std::begin(o.invGt), std::end(o.invGt), invGt);
        projectionWkt = std::move(o.projectionWkt);
        needsTransform = o.needsTransform;
        toTileCrs = o.toTileCrs;
        o.toTileCrs = nullptr;
    }
    return *this;
}

// ─── Tile index building ────────────────────────────────────────────

int ProfileData::BuildTileIndex(const std::vector<std::string>& zipPaths,
                                std::function<bool(int, int)> progress)
{
    m_tileIndex.clear();

    // Collect all entry names from all zips
    struct ZipEntry { std::string zipPath; std::string entryName; };
    std::vector<ZipEntry> entries;

    for (const auto& zipPath : zipPaths) {
        int err = 0;
        zip_t* z = zip_open(zipPath.c_str(), ZIP_RDONLY, &err);
        if (!z) continue;

        zip_int64_t n = zip_get_num_entries(z, 0);
        for (zip_int64_t i = 0; i < n; i++) {
            zip_stat_t st;
            zip_stat_init(&st);
            if (zip_stat_index(z, i, 0, &st) != 0) continue;
            entries.push_back({zipPath, st.name});
        }
        zip_close(z);
    }

    int total = static_cast<int>(entries.size());
    int indexed = 0;

    // Set up EPSG:25833 for uniform bounding boxes
    OGRSpatialReference srs25833;
    srs25833.importFromEPSG(25833);
    srs25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    for (int i = 0; i < total; i++) {
        if (progress && !progress(i, total))
            break;

        std::string vsiPath = MainFrame::BuildTileVsiPath(entries[i].zipPath,
                                                          entries[i].entryName);
        if (vsiPath.empty()) continue;

        GDALDataset* ds = static_cast<GDALDataset*>(
            GDALOpen(vsiPath.c_str(), GA_ReadOnly));
        if (!ds) continue;

        TileIndexEntry tile;
        tile.vsiPath = vsiPath;
        tile.rasterW = ds->GetRasterXSize();
        tile.rasterH = ds->GetRasterYSize();

        if (ds->GetGeoTransform(tile.gt) != CE_None) {
            GDALClose(ds);
            continue;
        }

        tile.projectionWkt = ds->GetProjectionRef();
        GDALClose(ds);

        // Compute bounding box in tile's native CRS
        double ulx = tile.gt[0];
        double uly = tile.gt[3];
        double lrx = tile.gt[0] + tile.rasterW * tile.gt[1];
        double lry = tile.gt[3] + tile.rasterH * tile.gt[5];

        // Transform to EPSG:25833
        OGRSpatialReference tileSrs;
        tileSrs.importFromWkt(tile.projectionWkt.c_str());
        tileSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        OGRCoordinateTransformation* toUniform =
            OGRCreateCoordinateTransformation(&tileSrs, &srs25833);
        if (toUniform) {
            double x1 = ulx, y1 = uly;
            double x2 = lrx, y2 = lry;
            toUniform->Transform(1, &x1, &y1);
            toUniform->Transform(1, &x2, &y2);
            tile.minX = std::min(x1, x2);
            tile.maxX = std::max(x1, x2);
            tile.minY = std::min(y1, y2);
            tile.maxY = std::max(y1, y2);
            OCTDestroyCoordinateTransformation(toUniform);
        } else {
            tile.minX = std::min(ulx, lrx);
            tile.maxX = std::max(ulx, lrx);
            tile.minY = std::min(uly, lry);
            tile.maxY = std::max(uly, lry);
        }

        m_tileIndex.push_back(std::move(tile));
        indexed++;
    }

    return indexed;
}

// ─── Line name discovery ────────────────────────────────────────────

std::vector<std::string> ProfileData::GetLineNames(const std::string& railwayPath) const
{
    std::set<std::string> names;

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return {};

    OGRLayer* tracks = ds->GetLayerByName("Banelenke");
    if (tracks) {
        // Only include operational lines
        tracks->SetAttributeFilter("banestatus = 'I'");
        tracks->ResetReading();
        OGRFeature* feat;
        while ((feat = tracks->GetNextFeature()) != nullptr) {
            const char* name = feat->GetFieldAsString("banenavn");
            if (name && name[0])
                names.insert(name);
            OGRFeature::DestroyFeature(feat);
        }
    }

    GDALClose(ds);

    std::vector<std::string> result(names.begin(), names.end());
    std::sort(result.begin(), result.end());
    return result;
}

// ─── Elevation sampling ─────────────────────────────────────────────

const CachedTile* ProfileData::LoadTileIntoCache(int tileIdx) const
{
    // Check if already cached
    for (auto& ct : m_tileCache) {
        if (ct.indexEntry == tileIdx) {
            // Move to front (MRU)
            if (&ct != &m_tileCache.front()) {
                CachedTile tmp = std::move(ct);
                m_tileCache.erase(m_tileCache.begin() +
                    (&ct - m_tileCache.data()));
                m_tileCache.insert(m_tileCache.begin(), std::move(tmp));
            }
            return &m_tileCache.front();
        }
    }

    // Evict LRU if at capacity
    if (static_cast<int>(m_tileCache.size()) >= kMaxCachedTiles)
        m_tileCache.pop_back();

    const auto& entry = m_tileIndex[tileIdx];

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpen(entry.vsiPath.c_str(), GA_ReadOnly));
    if (!ds) return nullptr;

    CachedTile ct;
    ct.indexEntry = tileIdx;
    ct.w = ds->GetRasterXSize();
    ct.h = ds->GetRasterYSize();
    ct.projectionWkt = entry.projectionWkt;
    std::copy(std::begin(entry.gt), std::end(entry.gt), ct.gt);

    if (!GDALInvGeoTransform(ct.gt, ct.invGt)) {
        GDALClose(ds);
        return nullptr;
    }

    ct.elevation.resize(static_cast<size_t>(ct.w) * ct.h);
    GDALRasterBand* band = ds->GetRasterBand(1);
    if (band->RasterIO(GF_Read, 0, 0, ct.w, ct.h,
                        ct.elevation.data(), ct.w, ct.h,
                        GDT_Float32, 0, 0) != CE_None) {
        GDALClose(ds);
        return nullptr;
    }

    // Check nodata
    int hasND = 0;
    double nodata = band->GetNoDataValue(&hasND);
    if (hasND) {
        float nd = static_cast<float>(nodata);
        for (auto& v : ct.elevation) {
            if (v == nd) v = -9999.0f;
        }
    }

    GDALClose(ds);

    // Set up cached coordinate transform from EPSG:25833 to tile CRS
    OGRSpatialReference srs25833, tileSrs;
    srs25833.importFromEPSG(25833);
    srs25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    tileSrs.importFromWkt(ct.projectionWkt.c_str());
    tileSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    if (!srs25833.IsSame(&tileSrs)) {
        ct.needsTransform = true;
        ct.toTileCrs = OGRCreateCoordinateTransformation(&srs25833, &tileSrs);
    }

    m_tileCache.insert(m_tileCache.begin(), std::move(ct));
    return &m_tileCache.front();
}

bool ProfileData::SampleElevation(double x25833, double y25833, float& elevOut) const
{
    // Find a tile covering this point
    for (int i = 0; i < static_cast<int>(m_tileIndex.size()); i++) {
        const auto& t = m_tileIndex[i];
        if (x25833 < t.minX || x25833 > t.maxX ||
            y25833 < t.minY || y25833 > t.maxY)
            continue;

        const CachedTile* ct = LoadTileIntoCache(i);
        if (!ct) continue;

        // Transform from EPSG:25833 to tile's native CRS
        double x = x25833, y = y25833;
        if (ct->needsTransform && ct->toTileCrs)
            ct->toTileCrs->Transform(1, &x, &y);

        // Apply inverse geotransform to get pixel coordinates
        double px = ct->invGt[0] + x * ct->invGt[1] + y * ct->invGt[2];
        double py = ct->invGt[3] + x * ct->invGt[4] + y * ct->invGt[5];

        // Bilinear interpolation
        int ix = static_cast<int>(std::floor(px));
        int iy = static_cast<int>(std::floor(py));
        if (ix < 0 || ix >= ct->w - 1 || iy < 0 || iy >= ct->h - 1)
            continue;

        double fx = px - ix;
        double fy = py - iy;

        auto sample = [&](int cx, int cy) -> float {
            return ct->elevation[static_cast<size_t>(cy) * ct->w + cx];
        };

        float v00 = sample(ix, iy);
        float v10 = sample(ix + 1, iy);
        float v01 = sample(ix, iy + 1);
        float v11 = sample(ix + 1, iy + 1);

        // Skip nodata pixels
        if (v00 < -9000 || v10 < -9000 || v01 < -9000 || v11 < -9000)
            continue;

        elevOut = static_cast<float>(
            v00 * (1 - fx) * (1 - fy) +
            v10 * fx * (1 - fy) +
            v01 * (1 - fx) * fy +
            v11 * fx * fy);
        return true;
    }
    return false;
}

// ─── Profile building ───────────────────────────────────────────────

ProfileResult ProfileData::BuildProfile(const std::string& railwayPath,
                                         const std::string& roadsPath,
                                         const std::string& lineName,
                                         ProgressCb progress) const
{
    auto report = [&](int pct, const std::string& msg) {
        if (progress) progress(pct, msg);
    };
    ProfileResult result;
    result.stats.lineName = lineName;

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return result;

    report(0, "Loading track segments...");

    // --- Collect segments for this line ---
    struct RawSegment {
        double startKm, endKm;
        char medium;
        std::vector<std::pair<double, double>> coords;  // (x, y) in EPSG:25833
    };
    std::vector<RawSegment> segments;

    OGRLayer* tracks = ds->GetLayerByName("Banelenke");
    if (tracks) {
        std::string filter = "banenavn = '" + lineName + "' AND banestatus = 'I'";
        tracks->SetAttributeFilter(filter.c_str());
        tracks->ResetReading();
        OGRFeature* feat;
        while ((feat = tracks->GetNextFeature()) != nullptr) {
            OGRGeometry* geom = feat->GetGeometryRef();
            if (!geom) { OGRFeature::DestroyFeature(feat); continue; }

            double startKm = feat->GetFieldAsDouble("startposisjon");
            double endKm = feat->GetFieldAsDouble("sluttposisjon");
            const char* medField = feat->GetFieldAsString("medium");
            char medium = (medField && medField[0]) ? medField[0] : ' ';

            auto processLine = [&](OGRLineString* line) {
                RawSegment seg;
                seg.startKm = startKm;
                seg.endKm = endKm;
                seg.medium = medium;
                int nPts = line->getNumPoints();
                for (int p = 0; p < nPts; p++)
                    seg.coords.push_back({line->getX(p), line->getY(p)});
                if (!seg.coords.empty())
                    segments.push_back(std::move(seg));
            };

            OGRwkbGeometryType gtype = wkbFlatten(geom->getGeometryType());
            if (gtype == wkbLineString) {
                processLine(static_cast<OGRLineString*>(geom));
            } else if (gtype == wkbMultiLineString) {
                auto* multi = static_cast<OGRMultiLineString*>(geom);
                for (int g = 0; g < multi->getNumGeometries(); g++)
                    processLine(static_cast<OGRLineString*>(
                        multi->getGeometryRef(g)));
            }

            OGRFeature::DestroyFeature(feat);
        }
    }

    // Sort by startposisjon
    std::sort(segments.begin(), segments.end(),
              [](const RawSegment& a, const RawSegment& b) {
                  return a.startKm < b.startKm;
              });

    // Remove duplicate km ranges (parallel tracks at stations) — keep first
    {
        std::vector<RawSegment> unique;
        for (auto& seg : segments) {
            bool dup = false;
            for (const auto& u : unique) {
                if (std::abs(u.startKm - seg.startKm) < 0.001 &&
                    std::abs(u.endKm - seg.endKm) < 0.001) {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                unique.push_back(std::move(seg));
        }
        segments = std::move(unique);
    }

    // --- Densify and sample elevation ---
    report(5, "Sampling elevation...");
    constexpr double kDensifySpacing = 50.0;  // metres between sample points

    int segsDone = 0;
    for (const auto& seg : segments) {
        if (seg.coords.size() < 2) continue;

        // Compute total geometry length
        double geomLength = 0;
        for (size_t i = 1; i < seg.coords.size(); i++) {
            double dx = seg.coords[i].first - seg.coords[i - 1].first;
            double dy = seg.coords[i].second - seg.coords[i - 1].second;
            geomLength += std::sqrt(dx * dx + dy * dy);
        }
        if (geomLength < 1.0) continue;

        // Map geometry distance to km
        double kmRange = seg.endKm - seg.startKm;
        if (kmRange <= 0) kmRange = geomLength / 1000.0;

        // Walk along geometry, sampling at kDensifySpacing intervals
        auto interpolatePoint = [&](double targetDist) -> std::pair<double, double> {
            double d = 0;
            for (size_t i = 1; i < seg.coords.size(); i++) {
                double dx = seg.coords[i].first - seg.coords[i - 1].first;
                double dy = seg.coords[i].second - seg.coords[i - 1].second;
                double segLen = std::sqrt(dx * dx + dy * dy);
                if (d + segLen >= targetDist) {
                    double f = (segLen > 0) ? (targetDist - d) / segLen : 0;
                    return {seg.coords[i - 1].first + f * dx,
                            seg.coords[i - 1].second + f * dy};
                }
                d += segLen;
            }
            return seg.coords.back();
        };

        int nSamples = std::max(2, static_cast<int>(geomLength / kDensifySpacing) + 1);
        for (int s = 0; s < nSamples; s++) {
            double frac = static_cast<double>(s) / (nSamples - 1);
            double dist = frac * geomLength;
            auto [x, y] = interpolatePoint(dist);
            double km = seg.startKm + frac * kmRange;

            ProfilePoint pt;
            pt.km = km;
            pt.x = x;
            pt.y = y;
            pt.medium = seg.medium;
            pt.interpolated = false;

            float elev = 0;
            if (SampleElevation(x, y, elev))
                pt.elevation = elev;
            else
                pt.elevation = -9999;

            result.points.push_back(pt);
        }

        segsDone++;
        if (segsDone % 50 == 0 || segsDone == static_cast<int>(segments.size()))
            report(5 + 60 * segsDone / static_cast<int>(segments.size()),
                   "Sampling elevation... (" + std::to_string(segsDone) + "/" +
                   std::to_string(segments.size()) + " segments)");
    }

    // Sort all points by km
    std::sort(result.points.begin(), result.points.end(),
              [](const ProfilePoint& a, const ProfilePoint& b) {
                  return a.km < b.km;
              });

    // Remove points with failed elevation (unless they're tunnel)
    result.points.erase(
        std::remove_if(result.points.begin(), result.points.end(),
                        [](const ProfilePoint& p) {
                            return p.elevation < -9000 && p.medium != 'U';
                        }),
        result.points.end());

    // Reject implausible elevations: track below ~2m above sea level is
    // extremely unlikely (flooding risk from storm surge / high tide).
    // DTM artifacts near coastlines and fjords can produce spurious low
    // readings.  Mark these as failed so they get interpolated through.
    constexpr double kMinPlausibleElev = 2.0;
    for (auto& pt : result.points) {
        if (pt.elevation > -9000 && pt.elevation < kMinPlausibleElev
            && pt.medium != 'U')
            pt.elevation = -9999;
    }

    // Add track bed offset: rail sits on ballast + sleepers, typically
    // ~0.6m above natural ground (0.3m ballast + 0.25m sleeper + 0.05m rail)
    constexpr double kTrackBedOffset = 0.6;
    for (auto& pt : result.points) {
        if (pt.elevation > -9000 && pt.medium != 'U')
            pt.elevation += kTrackBedOffset;
    }

    // --- Stations ---
    OGRLayer* stations = ds->GetLayerByName("Stasjonsnode");
    if (stations) {
        std::string filter = "banenavn = '" + lineName + "'";
        stations->SetAttributeFilter(filter.c_str());
        stations->ResetReading();
        OGRFeature* feat;
        while ((feat = stations->GetNextFeature()) != nullptr) {
            const char* name = feat->GetFieldAsString("stasjonsnavn");
            double km = feat->GetFieldAsDouble("sporkilometer");
            const char* typeField = feat->GetFieldAsString("stasjonstype");
            char type = (typeField && typeField[0]) ? typeField[0] : 'S';

            OGRGeometry* geom = feat->GetGeometryRef();
            double elev = 0;
            if (geom && wkbFlatten(geom->getGeometryType()) == wkbPoint) {
                OGRPoint* pt = static_cast<OGRPoint*>(geom);
                float e;
                if (SampleElevation(pt->getX(), pt->getY(), e))
                    elev = e;
            }

            ProfileStation ps;
            ps.km = km;
            ps.elevation = elev;
            ps.name = name ? name : "";
            ps.type = type;
            result.stations.push_back(std::move(ps));

            OGRFeature::DestroyFeature(feat);
        }
    }

    std::sort(result.stations.begin(), result.stations.end(),
              [](const ProfileStation& a, const ProfileStation& b) {
                  return a.km < b.km;
              });

    GDALClose(ds);

    report(68, "Smoothing profile...");

    // --- Smooth surface sections to simulate railway earthworks ---
    // Must happen before tunnel interpolation so tunnel portals use
    // the smoothed track elevation, not raw DTM surface elevation
    SmoothProfile(result.points);

    report(72, "Interpolating tunnels...");

    // --- Tunnel interpolation ---
    InterpolateTunnels(result.points);

    // --- Station elevations from profile ---
    // Use the smoothed profile elevation at each station's km position
    for (auto& st : result.stations) {
        double bestDist = 1e9;
        for (const auto& pt : result.points) {
            double d = std::abs(pt.km - st.km);
            if (d < bestDist) {
                bestDist = d;
                st.elevation = pt.elevation;
            }
        }
    }

    report(78, "Computing statistics...");

    // --- Compute stats ---
    result.stats = ComputeStats(lineName, result.points, result.stations);

    report(82, "Finding junctions and crossings...");

    // --- Find junctions and crossings ---
    result.junctions = FindTrackJunctions(railwayPath, roadsPath, lineName,
                                          result.points);

    return result;
}

// ─── Profile smoothing ──────────────────────────────────────────────
//
// Parameterized smoothing used by both railway and road profiles.
// Four passes:
//  1. Gaussian-weighted moving average to absorb local terrain roughness.
//  2. Forward gradient-limiting sweep.
//  3. Backward gradient-limiting sweep.
//  4. Vertical curve smoothing (smooth gradient, reconstruct elevations).

void ProfileData::SmoothWithParams(std::vector<ProfilePoint>& points,
                                   double elevSigmaKm,
                                   double maxGrade,
                                   double gradeSigmaKm)
{
    if (points.size() < 3) return;

    // --- Pass 1: Gaussian elevation smoothing ---
    {
        double windowKm = elevSigmaKm * 3.0;
        std::vector<double> smoothed(points.size());

        for (size_t i = 0; i < points.size(); i++) {
            if (points[i].medium == 'U' || points[i].elevation < -9000) {
                smoothed[i] = points[i].elevation;
                continue;
            }

            double weightSum = 0, valueSum = 0;
            double centerKm = points[i].km;

            for (size_t j = 0; j < points.size(); j++) {
                if (points[j].medium == 'U' || points[j].elevation < -9000)
                    continue;
                double dkm = points[j].km - centerKm;
                if (std::abs(dkm) > windowKm) {
                    if (dkm > windowKm) break;
                    continue;
                }
                double w = std::exp(-0.5 * (dkm / elevSigmaKm) *
                                           (dkm / elevSigmaKm));
                weightSum += w;
                valueSum += w * points[j].elevation;
            }

            smoothed[i] = (weightSum > 0) ? valueSum / weightSum
                                           : points[i].elevation;
        }

        for (size_t i = 0; i < points.size(); i++) {
            if (points[i].medium != 'U' && points[i].elevation > -9000)
                points[i].elevation = smoothed[i];
        }
    }

    // --- Pass 2 & 3: Gradient limiting ---
    for (size_t i = 1; i < points.size(); i++) {
        if (points[i].medium == 'U' || points[i].elevation < -9000) continue;
        if (points[i - 1].elevation < -9000) continue;
        double dkm = points[i].km - points[i - 1].km;
        if (dkm <= 0) continue;
        double maxRise = maxGrade * dkm * 1000.0;
        double diff = points[i].elevation - points[i - 1].elevation;
        if (diff > maxRise)
            points[i].elevation = points[i - 1].elevation + maxRise;
    }

    for (size_t i = points.size() - 1; i > 0; i--) {
        if (points[i - 1].medium == 'U' || points[i - 1].elevation < -9000) continue;
        if (points[i].elevation < -9000) continue;
        double dkm = points[i].km - points[i - 1].km;
        if (dkm <= 0) continue;
        double maxRise = maxGrade * dkm * 1000.0;
        double diff = points[i - 1].elevation - points[i].elevation;
        if (diff > maxRise)
            points[i - 1].elevation = points[i].elevation + maxRise;
    }

    // --- Pass 4: Vertical curve smoothing ---
    std::vector<size_t> surfIdx;
    for (size_t i = 0; i < points.size(); i++) {
        if (points[i].medium != 'U' && points[i].elevation > -9000)
            surfIdx.push_back(i);
    }
    if (surfIdx.size() < 3) return;

    size_t nSurf = surfIdx.size();
    std::vector<double> grades(nSurf, 0.0);
    for (size_t si = 1; si < nSurf; si++) {
        size_t i = surfIdx[si], ip = surfIdx[si - 1];
        double dkm = points[i].km - points[ip].km;
        if (dkm > 0)
            grades[si] = (points[i].elevation - points[ip].elevation) /
                         (dkm * 1000.0);
    }
    grades[0] = grades[1];

    double gradeWindowKm = gradeSigmaKm * 3.0;
    std::vector<double> smoothGrades(nSurf);
    for (size_t si = 0; si < nSurf; si++) {
        double centerKm = points[surfIdx[si]].km;
        double wSum = 0, vSum = 0;
        for (size_t sj = 0; sj < nSurf; sj++) {
            double dkm = points[surfIdx[sj]].km - centerKm;
            if (std::abs(dkm) > gradeWindowKm) {
                if (dkm > gradeWindowKm) break;
                continue;
            }
            double w = std::exp(-0.5 * (dkm / gradeSigmaKm) *
                                       (dkm / gradeSigmaKm));
            wSum += w;
            vSum += w * grades[sj];
        }
        smoothGrades[si] = (wSum > 0) ? vSum / wSum : grades[si];
    }

    std::vector<double> reconstructed(nSurf);
    reconstructed[0] = points[surfIdx[0]].elevation;
    for (size_t si = 1; si < nSurf; si++) {
        double dkm = points[surfIdx[si]].km - points[surfIdx[si - 1]].km;
        reconstructed[si] = reconstructed[si - 1] +
                            smoothGrades[si] * dkm * 1000.0;
    }

    double reconEnd = reconstructed[nSurf - 1];
    double origEnd = points[surfIdx[nSurf - 1]].elevation;
    double drift = origEnd - reconEnd;
    double totalKm = points[surfIdx[nSurf - 1]].km - points[surfIdx[0]].km;

    for (size_t si = 0; si < nSurf; si++) {
        double frac = (totalKm > 0) ?
            (points[surfIdx[si]].km - points[surfIdx[0]].km) / totalKm : 0;
        points[surfIdx[si]].elevation = reconstructed[si] + drift * frac;
    }
}

// Railway-specific wrapper: uses railway smoothing parameters
void ProfileData::SmoothProfile(std::vector<ProfilePoint>& points) const
{
    SmoothWithParams(points, 0.21, 0.025, 0.42);
}

// ─── Tunnel interpolation ───────────────────────────────────────────

void ProfileData::InterpolateTunnels(std::vector<ProfilePoint>& points) const
{
    if (points.empty()) return;

    size_t i = 0;
    while (i < points.size()) {
        // Find start of tunnel section
        if (points[i].medium != 'U') { i++; continue; }

        size_t tunnelStart = i;

        // Find end of tunnel section
        size_t tunnelEnd = i;
        while (tunnelEnd < points.size() && points[tunnelEnd].medium == 'U')
            tunnelEnd++;

        // Get entrance elevation (last surface point before tunnel)
        double enterElev = -9999;
        double enterKm = points[tunnelStart].km;
        if (tunnelStart > 0) {
            enterElev = points[tunnelStart - 1].elevation;
            enterKm = points[tunnelStart - 1].km;
        }

        // Get exit elevation (first surface point after tunnel)
        double exitElev = -9999;
        double exitKm = points[tunnelEnd < points.size() ? tunnelEnd : tunnelEnd - 1].km;
        if (tunnelEnd < points.size()) {
            exitElev = points[tunnelEnd].elevation;
            exitKm = points[tunnelEnd].km;
        }

        // If we have both entrance and exit, linearly interpolate
        if (enterElev > -9000 && exitElev > -9000) {
            double kmSpan = exitKm - enterKm;
            for (size_t j = tunnelStart; j < tunnelEnd; j++) {
                double f = (kmSpan > 0) ?
                    (points[j].km - enterKm) / kmSpan : 0.5;
                points[j].elevation = enterElev + f * (exitElev - enterElev);
                points[j].interpolated = true;
            }
        } else if (enterElev > -9000) {
            // Line starts or ends in tunnel — use flat grade from known end
            for (size_t j = tunnelStart; j < tunnelEnd; j++) {
                points[j].elevation = enterElev;
                points[j].interpolated = true;
            }
        } else if (exitElev > -9000) {
            for (size_t j = tunnelStart; j < tunnelEnd; j++) {
                points[j].elevation = exitElev;
                points[j].interpolated = true;
            }
        }

        i = tunnelEnd;
    }
}

// ─── Stats computation ──────────────────────────────────────────────

ProfileStats ProfileData::ComputeStats(const std::string& lineName,
                                        const std::vector<ProfilePoint>& points,
                                        const std::vector<ProfileStation>& stations) const
{
    ProfileStats s;
    s.lineName = lineName;
    s.segmentCount = 0;
    s.stationCount = static_cast<int>(stations.size());
    s.totalLengthKm = 0;
    s.minElev = 1e9;
    s.maxElev = -1e9;
    s.totalClimb = 0;
    s.totalDescent = 0;
    s.maxGradePct = 0;
    s.tunnelLengthKm = 0;
    s.bridgeLengthKm = 0;

    if (points.empty()) return s;

    s.totalLengthKm = points.back().km - points.front().km;

    // Count distinct segments (transitions in medium or gaps > 0.5km)
    s.segmentCount = 1;
    for (size_t i = 1; i < points.size(); i++) {
        if (points[i].km - points[i - 1].km > 0.5)
            s.segmentCount++;
    }

    for (size_t i = 0; i < points.size(); i++) {
        double e = points[i].elevation;
        if (e < -9000) continue;
        s.minElev = std::min(s.minElev, e);
        s.maxElev = std::max(s.maxElev, e);

        if (i > 0 && points[i - 1].elevation > -9000) {
            double de = e - points[i - 1].elevation;
            double dk = (points[i].km - points[i - 1].km) * 1000.0;  // to metres
            if (de > 0) s.totalClimb += de;
            else s.totalDescent += -de;
            if (dk > 0) {
                double grade = std::abs(de / dk) * 100.0;
                s.maxGradePct = std::max(s.maxGradePct, grade);
            }
        }

        // Tunnel/bridge length
        if (i > 0) {
            double dkm = points[i].km - points[i - 1].km;
            if (points[i].medium == 'U')
                s.tunnelLengthKm += dkm;
            else if (points[i].medium == 'L' || points[i].medium == 'B')
                s.bridgeLengthKm += dkm;
        }
    }

    if (s.minElev > 1e8) s.minElev = 0;
    if (s.maxElev < -1e8) s.maxElev = 0;

    return s;
}

// ─── Junction and crossing detection ───────────────────────────────

namespace {

// 2D segment intersection test — returns true with parameter tA on seg A
bool SegIntersect2D(double a1x, double a1y, double a2x, double a2y,
                    double b1x, double b1y, double b2x, double b2y,
                    double& tA)
{
    double dx = a2x - a1x, dy = a2y - a1y;
    double ex = b2x - b1x, ey = b2y - b1y;
    double denom = dx * ey - dy * ex;
    if (std::abs(denom) < 1e-12) return false;
    double fx = b1x - a1x, fy = b1y - a1y;
    tA = (fx * ey - fy * ex) / denom;
    double tB = (fx * dy - fy * dx) / denom;
    return tA >= 0 && tA <= 1 && tB >= 0 && tB <= 1;
}

} // anon

std::vector<TrackJunction> ProfileData::FindTrackJunctions(
    const std::string& railwayPath,
    const std::string& roadsPath,
    const std::string& lineName,
    const std::vector<ProfilePoint>& points) const
{
    std::vector<TrackJunction> result;
    if (points.empty()) return result;

    // Helper: find km and elevation on the profile nearest to (px, py)
    auto findOnProfile = [&](double px, double py)
        -> std::tuple<double, double, char> {
        double bestD2 = 1e18;
        double km = 0, elev = 0;
        char medium = ' ';
        for (const auto& pt : points) {
            double dx = pt.x - px, dy = pt.y - py;
            double d2 = dx * dx + dy * dy;
            if (d2 < bestD2) {
                bestD2 = d2;
                km = pt.km;
                elev = pt.elevation;
                medium = pt.medium;
            }
        }
        return {km, elev, medium};
    };

    // ── Part A: Track-track junctions ──

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return result;

    OGRLayer* tracks = ds->GetLayerByName("Banelenke");
    if (!tracks) { GDALClose(ds); return result; }

    // Load all active track segments
    struct TrackSeg {
        std::string lineName;
        char medium;
        std::vector<std::pair<double, double>> coords;
    };
    std::vector<TrackSeg> allSegs;

    tracks->SetAttributeFilter("banestatus = 'I'");
    tracks->ResetReading();
    OGRFeature* feat;
    while ((feat = tracks->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom) { OGRFeature::DestroyFeature(feat); continue; }

        const char* name = feat->GetFieldAsString("banenavn");
        const char* medField = feat->GetFieldAsString("medium");
        char medium = (medField && medField[0]) ? medField[0] : ' ';

        auto processLine = [&](OGRLineString* line) {
            TrackSeg seg;
            seg.lineName = name ? name : "";
            seg.medium = medium;
            int nPts = line->getNumPoints();
            for (int p = 0; p < nPts; p++)
                seg.coords.push_back({line->getX(p), line->getY(p)});
            if (seg.coords.size() >= 2)
                allSegs.push_back(std::move(seg));
        };

        OGRwkbGeometryType gtype = wkbFlatten(geom->getGeometryType());
        if (gtype == wkbLineString)
            processLine(static_cast<OGRLineString*>(geom));
        else if (gtype == wkbMultiLineString) {
            auto* multi = static_cast<OGRMultiLineString*>(geom);
            for (int g = 0; g < multi->getNumGeometries(); g++)
                processLine(static_cast<OGRLineString*>(
                    multi->getGeometryRef(g)));
        }
        OGRFeature::DestroyFeature(feat);
    }

    GDALClose(ds);

    // Deduplicate segments — the GML data contains each segment twice.
    // Two segments are duplicates if they share the same line name, medium,
    // and their start/end coordinates match within tolerance.
    {
        constexpr double kDupTol2 = 1.0;  // 1m²
        auto isDup = [&](const TrackSeg& a, const TrackSeg& b) {
            if (a.lineName != b.lineName || a.medium != b.medium) return false;
            if (a.coords.size() != b.coords.size()) return false;
            double dsx = a.coords.front().first - b.coords.front().first;
            double dsy = a.coords.front().second - b.coords.front().second;
            double dex = a.coords.back().first - b.coords.back().first;
            double dey = a.coords.back().second - b.coords.back().second;
            return (dsx*dsx + dsy*dsy < kDupTol2) &&
                   (dex*dex + dey*dey < kDupTol2);
        };

        std::vector<TrackSeg> unique;
        unique.reserve(allSegs.size());
        for (auto& seg : allSegs) {
            bool dup = false;
            for (const auto& u : unique) {
                if (isDup(seg, u)) { dup = true; break; }
            }
            if (!dup)
                unique.push_back(std::move(seg));
        }
        allSegs = std::move(unique);
    }

    // Build spatial hash of endpoints
    constexpr double kNodeTol = 10.0;
    constexpr double kNodeCell = 10.0;

    struct NodeEntry { int segIdx; bool isEnd; double x, y; };
    std::unordered_map<int64_t, std::vector<NodeEntry>> nodeHash;

    auto nodeKey = [](double x, double y) -> int64_t {
        int gx = static_cast<int>(std::floor(x / kNodeCell));
        int gy = static_cast<int>(std::floor(y / kNodeCell));
        return static_cast<int64_t>(gx) * 10000007LL + gy;
    };

    for (int i = 0; i < static_cast<int>(allSegs.size()); i++) {
        auto& s = allSegs[i];
        auto& front = s.coords.front();
        auto& back = s.coords.back();
        nodeHash[nodeKey(front.first, front.second)].push_back(
            {i, false, front.first, front.second});
        nodeHash[nodeKey(back.first, back.second)].push_back(
            {i, true, back.first, back.second});
    }

    auto queryNode = [&](double x, double y) -> std::vector<NodeEntry> {
        std::vector<NodeEntry> hits;
        int cx = static_cast<int>(std::floor(x / kNodeCell));
        int cy = static_cast<int>(std::floor(y / kNodeCell));
        double tol2 = kNodeTol * kNodeTol;
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                int64_t k = static_cast<int64_t>(cx + dx) * 10000007LL + (cy + dy);
                auto it = nodeHash.find(k);
                if (it == nodeHash.end()) continue;
                for (auto& e : it->second) {
                    double ddx = e.x - x, ddy = e.y - y;
                    if (ddx * ddx + ddy * ddy <= tol2)
                        hits.push_back(e);
                }
            }
        }
        return hits;
    };

    // For each endpoint of our line's segments, check for junctions
    std::set<int64_t> processedNodes;  // avoid duplicate junctions at same spot

    for (int si = 0; si < static_cast<int>(allSegs.size()); si++) {
        if (allSegs[si].lineName != lineName) continue;

        for (int endpt = 0; endpt < 2; endpt++) {
            bool isEnd = (endpt == 1);
            auto& ep = isEnd ? allSegs[si].coords.back()
                             : allSegs[si].coords.front();

            // Check if we already processed this node
            int64_t nk = nodeKey(ep.first, ep.second);
            if (processedNodes.count(nk)) continue;
            processedNodes.insert(nk);

            auto neighbors = queryNode(ep.first, ep.second);
            if (neighbors.size() <= 2) continue;  // just continuation

            // Collect unique segments at this node
            std::set<int> segSet;
            for (auto& n : neighbors) segSet.insert(n.segIdx);
            int ways = static_cast<int>(segSet.size());
            if (ways <= 2) continue;

            // Check mediums at this node
            std::set<char> mediums;
            std::set<std::string> otherLines;
            bool ourLineHere = false;
            char ourMedium = ' ';

            for (int idx : segSet) {
                mediums.insert(allSegs[idx].medium);
                if (allSegs[idx].lineName == lineName) {
                    ourLineHere = true;
                    ourMedium = allSegs[idx].medium;
                } else {
                    otherLines.insert(allSegs[idx].lineName);
                }
            }
            if (!ourLineHere) continue;

            auto [km, elev, profMedium] = findOnProfile(ep.first, ep.second);

            // Check if there's a grade separation
            bool gradeSeparated = false;
            for (int idx : segSet) {
                if (allSegs[idx].lineName == lineName) continue;
                char otherMed = allSegs[idx].medium;
                // Different medium = grade separated
                if ((ourMedium == 'U' || ourMedium == 'T') && otherMed == ' ') {
                    gradeSeparated = true;
                    break;
                }
                if (ourMedium == ' ' && (otherMed == 'U' || otherMed == 'T')) {
                    gradeSeparated = true;
                    break;
                }
                if ((ourMedium == 'L' || ourMedium == 'B') && otherMed == ' ') {
                    gradeSeparated = true;
                    break;
                }
                if (ourMedium == ' ' && (otherMed == 'L' || otherMed == 'B')) {
                    gradeSeparated = true;
                    break;
                }
            }

            // Build description
            std::string desc;
            for (auto& ol : otherLines) {
                if (!desc.empty()) desc += ", ";
                desc += ol;
            }

            if (gradeSeparated) {
                TrackJunction jn;
                jn.km = km;
                jn.elevation = elev;
                jn.x = ep.first;
                jn.y = ep.second;
                if (ourMedium == 'U' || ourMedium == 'T')
                    jn.type = TrackJunction::Type::Underpass;
                else if (ourMedium == 'L' || ourMedium == 'B')
                    jn.type = TrackJunction::Type::Overpass;
                else
                    jn.type = TrackJunction::Type::Overpass;  // other is under
                jn.description = desc;
                result.push_back(jn);
                continue;
            }

            // Same level — classify by angles.
            // Compute direction vectors for each segment at this node,
            // then cluster nearby directions so that multiple segments
            // heading the same way count as one "route".
            std::vector<std::pair<double, double>> rawDirs;
            for (int idx : segSet) {
                auto& seg = allSegs[idx];
                double fx = seg.coords.front().first, fy = seg.coords.front().second;
                double dfx = fx - ep.first, dfy = fy - ep.second;
                bool frontNear = (dfx*dfx + dfy*dfy < kNodeTol*kNodeTol);

                double targetDist = 50.0;
                double dirX = 0, dirY = 0;

                if (frontNear) {
                    double cumDist = 0;
                    for (size_t j = 1; j < seg.coords.size(); j++) {
                        double dx = seg.coords[j].first - seg.coords[j-1].first;
                        double dy = seg.coords[j].second - seg.coords[j-1].second;
                        cumDist += std::sqrt(dx*dx + dy*dy);
                        if (cumDist >= targetDist || j == seg.coords.size() - 1) {
                            dirX = seg.coords[j].first - ep.first;
                            dirY = seg.coords[j].second - ep.second;
                            break;
                        }
                    }
                } else {
                    double cumDist = 0;
                    for (int j = static_cast<int>(seg.coords.size()) - 2; j >= 0; j--) {
                        double dx = seg.coords[j+1].first - seg.coords[j].first;
                        double dy = seg.coords[j+1].second - seg.coords[j].second;
                        cumDist += std::sqrt(dx*dx + dy*dy);
                        if (cumDist >= targetDist || j == 0) {
                            dirX = seg.coords[j].first - ep.first;
                            dirY = seg.coords[j].second - ep.second;
                            break;
                        }
                    }
                }

                double len = std::sqrt(dirX*dirX + dirY*dirY);
                if (len > 0) { dirX /= len; dirY /= len; }
                rawDirs.push_back({dirX, dirY});
            }

            // Cluster directions within 20° of each other (dot > 0.94)
            // so that multiple segments heading the same way = one route
            constexpr double kClusterDot = 0.94;  // cos(20°) ≈ 0.94
            std::vector<std::pair<double, double>> clustered;
            std::vector<bool> assigned(rawDirs.size(), false);

            for (size_t i = 0; i < rawDirs.size(); i++) {
                if (assigned[i]) continue;
                assigned[i] = true;
                double cx = rawDirs[i].first, cy = rawDirs[i].second;
                int cnt = 1;
                for (size_t j = i + 1; j < rawDirs.size(); j++) {
                    if (assigned[j]) continue;
                    double dot = cx * rawDirs[j].first + cy * rawDirs[j].second;
                    if (dot > kClusterDot) {
                        cx += rawDirs[j].first;
                        cy += rawDirs[j].second;
                        cnt++;
                        assigned[j] = true;
                    }
                }
                double len = std::sqrt(cx*cx + cy*cy);
                if (len > 0) { cx /= len; cy /= len; }
                clustered.push_back({cx, cy});
            }

            int effectiveWays = static_cast<int>(clustered.size());
            if (effectiveWays <= 2) continue;  // just continuation

            TrackJunction jn;
            jn.km = km;
            jn.elevation = elev;
            jn.x = ep.first;
            jn.y = ep.second;
            jn.description = desc;

            if (effectiveWays == 3) {
                jn.type = TrackJunction::Type::Switch;
                jn.numSwitches = 1;
            } else {
                // 4+ distinct directions: check for diamond crossing
                // (two independent through-routes crossing at >30°)
                // vs multiple switches (diverging tracks off one through-route)

                // Find through-route pairs (roughly opposing, dot < -0.85)
                struct ThroughRoute {
                    double axisX, axisY;
                };
                std::vector<ThroughRoute> throughRoutes;
                std::vector<bool> paired(clustered.size(), false);

                for (size_t a = 0; a < clustered.size(); a++) {
                    if (paired[a]) continue;
                    double bestDot = 0;
                    size_t bestB = a;
                    for (size_t b = a + 1; b < clustered.size(); b++) {
                        if (paired[b]) continue;
                        double dot = clustered[a].first * clustered[b].first +
                                     clustered[a].second * clustered[b].second;
                        if (dot < bestDot) { bestDot = dot; bestB = b; }
                    }
                    if (bestDot < -0.85 && bestB != a) {
                        paired[a] = paired[bestB] = true;
                        throughRoutes.push_back({clustered[a].first,
                                                 clustered[a].second});
                    }
                }

                bool isDiamond = false;
                if (throughRoutes.size() >= 2) {
                    double dot = throughRoutes[0].axisX * throughRoutes[1].axisX +
                                 throughRoutes[0].axisY * throughRoutes[1].axisY;
                    double crossAngle = std::acos(
                        std::clamp(std::abs(dot), 0.0, 1.0));
                    if (crossAngle > 30.0 * M_PI / 180.0)
                        isDiamond = true;
                }

                if (isDiamond) {
                    jn.type = TrackJunction::Type::DiamondCrossing;
                    jn.numSwitches = 0;
                } else {
                    // Count switches: each extra direction beyond the
                    // through-route is one switch
                    jn.type = (effectiveWays == 4)
                        ? TrackJunction::Type::DoubleSwitch
                        : TrackJunction::Type::Switch;
                    jn.numSwitches = effectiveWays - 2;
                }
            }

            result.push_back(jn);
        }
    }

    // ── Part B: Road-railway crossings ──

    if (!roadsPath.empty()) {
        // Compute bounding box of the profile
        double bboxMinX = 1e18, bboxMinY = 1e18;
        double bboxMaxX = -1e18, bboxMaxY = -1e18;
        for (const auto& pt : points) {
            bboxMinX = std::min(bboxMinX, pt.x);
            bboxMaxX = std::max(bboxMaxX, pt.x);
            bboxMinY = std::min(bboxMinY, pt.y);
            bboxMaxY = std::max(bboxMaxY, pt.y);
        }
        constexpr double kPad = 200.0;
        bboxMinX -= kPad; bboxMinY -= kPad;
        bboxMaxX += kPad; bboxMaxY += kPad;

        // Build polyline segments from profile for intersection testing
        // Use a subset of profile points (every ~200m) for efficiency
        struct RailSeg {
            double x1, y1, x2, y2;
            double km1, km2;
        };
        std::vector<RailSeg> railSegs;
        for (size_t i = 1; i < points.size(); i++) {
            if (points[i].km - points[i-1].km > 1.0) continue;
            railSegs.push_back({points[i-1].x, points[i-1].y,
                                points[i].x, points[i].y,
                                points[i-1].km, points[i].km});
        }

        GDALDataset* roadDs = static_cast<GDALDataset*>(
            GDALOpenEx(roadsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (roadDs) {
            OGRLayer* roads = roadDs->GetLayerByName("roads");
            if (roads) {
                const OGRSpatialReference* roadSrs = roads->GetSpatialRef();
                OGRSpatialReference srs25833;
                srs25833.importFromEPSG(25833);
                srs25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

                OGRCoordinateTransformation* toUniform = nullptr;
                if (roadSrs) {
                    OGRSpatialReference src(*roadSrs);
                    src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    if (!src.IsSame(&srs25833))
                        toUniform = OGRCreateCoordinateTransformation(&src, &srs25833);
                }

                // Set spatial filter
                if (toUniform) {
                    OGRSpatialReference src25833(srs25833);
                    OGRSpatialReference roadCrs(*roadSrs);
                    roadCrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    OGRCoordinateTransformation* fromUniform =
                        OGRCreateCoordinateTransformation(&src25833, &roadCrs);
                    if (fromUniform) {
                        double x1 = bboxMinX, y1 = bboxMinY;
                        double x2 = bboxMaxX, y2 = bboxMaxY;
                        fromUniform->Transform(1, &x1, &y1);
                        fromUniform->Transform(1, &x2, &y2);
                        roads->SetSpatialFilterRect(
                            std::min(x1, x2), std::min(y1, y2),
                            std::max(x1, x2), std::max(y1, y2));
                        OCTDestroyCoordinateTransformation(fromUniform);
                    }
                } else {
                    roads->SetSpatialFilterRect(bboxMinX, bboxMinY,
                                                 bboxMaxX, bboxMaxY);
                }

                // Only E/R/F roads for intersection markers
                roads->SetAttributeFilter(
                    "vegkategori IN ('E','R','F')");

                // Track hits for deduplication
                struct CrossingKey {
                    char kat; int num;
                    bool operator<(const CrossingKey& o) const {
                        return kat != o.kat ? kat < o.kat : num < o.num;
                    }
                };
                std::map<CrossingKey, std::vector<TrackJunction>> roadHits;

                roads->ResetReading();
                OGRFeature* rfeat;
                while ((rfeat = roads->GetNextFeature()) != nullptr) {
                    OGRGeometry* geom = rfeat->GetGeometryRef();
                    if (!geom || wkbFlatten(geom->getGeometryType()) != wkbLineString) {
                        OGRFeature::DestroyFeature(rfeat);
                        continue;
                    }

                    const char* kat = rfeat->GetFieldAsString("vegkategori");
                    int num = rfeat->GetFieldAsInteger("vegnummer");
                    char katC = (kat && kat[0]) ? kat[0] : '?';
                    std::string roadLabel;
                    switch (katC) {
                        case 'E': roadLabel = "E" + std::to_string(num); break;
                        case 'R': roadLabel = "Rv" + std::to_string(num); break;
                        default: roadLabel = "Fv" + std::to_string(num); break;
                    }

                    OGRLineString* line = static_cast<OGRLineString*>(geom->clone());
                    if (toUniform)
                        line->transform(toUniform);

                    int nPts = line->getNumPoints();
                    for (int p = 0; p + 1 < nPts; p++) {
                        double bx1 = line->getX(p), by1 = line->getY(p);
                        double bz1 = line->getZ(p);
                        double bx2 = line->getX(p + 1), by2 = line->getY(p + 1);
                        double bz2 = line->getZ(p + 1);

                        for (const auto& rs : railSegs) {
                            double tA;
                            if (!SegIntersect2D(rs.x1, rs.y1, rs.x2, rs.y2,
                                                bx1, by1, bx2, by2, tA))
                                continue;

                            double km = rs.km1 + tA * (rs.km2 - rs.km1);
                            double ix = rs.x1 + tA * (rs.x2 - rs.x1);
                            double iy = rs.y1 + tA * (rs.y2 - rs.y1);

                            // Road Z at intersection
                            // tB parameter on road segment
                            double rdx = bx2 - bx1, rdy = by2 - by1;
                            double denom = (rs.x2-rs.x1)*(by2-by1) - (rs.y2-rs.y1)*(bx2-bx1);
                            double tB = 0.5;
                            if (std::abs(denom) > 1e-12) {
                                double fx = bx1 - rs.x1, fy = by1 - rs.y1;
                                tB = (fx*(rs.y2-rs.y1) - fy*(rs.x2-rs.x1)) / denom;
                                tB = std::clamp(tB, 0.0, 1.0);
                            }
                            double roadZ = bz1 + tB * (bz2 - bz1);

                            // Railway elevation and medium at this km
                            auto [rkm, railElev, railMed] =
                                findOnProfile(ix, iy);

                            TrackJunction jn;
                            jn.km = km;
                            jn.elevation = railElev;
                            jn.x = ix;
                            jn.y = iy;
                            jn.description = roadLabel;

                            // Classify crossing type
                            if (railMed == 'U' || railMed == 'T') {
                                jn.type = TrackJunction::Type::RoadOverpass;
                            } else if (railMed == 'L' || railMed == 'B') {
                                jn.type = TrackJunction::Type::RoadUnderpass;
                            } else {
                                double diff = roadZ - railElev;
                                if (std::abs(diff) < 5.0)
                                    jn.type = TrackJunction::Type::RoadLevelCrossing;
                                else if (diff > 0)
                                    jn.type = TrackJunction::Type::RoadOverpass;
                                else
                                    jn.type = TrackJunction::Type::RoadUnderpass;
                            }

                            CrossingKey key{katC, num};
                            roadHits[key].push_back(jn);
                        }
                    }

                    delete line;
                    OGRFeature::DestroyFeature(rfeat);
                }

                if (toUniform)
                    OCTDestroyCoordinateTransformation(toUniform);

                // Deduplicate within 0.2 km
                for (auto& [key, hits] : roadHits) {
                    std::sort(hits.begin(), hits.end(),
                              [](const TrackJunction& a, const TrackJunction& b) {
                                  return a.km < b.km;
                              });
                    size_t i = 0;
                    while (i < hits.size()) {
                        size_t j = i + 1;
                        while (j < hits.size() && hits[j].km - hits[i].km < 0.2)
                            j++;
                        result.push_back(hits[i + (j - i) / 2]);
                        i = j;
                    }
                }
            }
            GDALClose(roadDs);
        }
    }

    // Sort all junctions by km
    std::sort(result.begin(), result.end(),
              [](const TrackJunction& a, const TrackJunction& b) {
                  return a.km < b.km;
              });

    return result;
}

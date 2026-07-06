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
                                         const std::string& lineName) const
{
    ProfileResult result;
    result.stats.lineName = lineName;

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return result;

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
    constexpr double kDensifySpacing = 50.0;  // metres between sample points

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

    // --- Smooth surface sections to simulate railway earthworks ---
    // Must happen before tunnel interpolation so tunnel portals use
    // the smoothed track elevation, not raw DTM surface elevation
    SmoothProfile(result.points);

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

    // --- Compute stats ---
    result.stats = ComputeStats(lineName, result.points, result.stations);

    return result;
}

// ─── Profile smoothing ──────────────────────────────────────────────
//
// Real railways use extensive earthworks (cuts through hills, fills across
// valleys) to maintain gentle, consistent grades.  The raw DTM elevation
// follows natural terrain, which is far too rough for a railway profile.
//
// We apply four passes:
//  1. Gaussian-weighted moving average (σ ≈ 250m) to absorb local terrain
//     roughness — simulates cuts and fills during construction.
//  2. Forward gradient-limiting sweep: caps uphill grade at kMaxGrade.
//  3. Backward gradient-limiting sweep: caps downhill grade at kMaxGrade.
//  4. Vertical curve smoothing: smooth the gradient profile with a Gaussian
//     so that grade transitions are gradual (parabolic) rather than abrupt.
//     Real railways use vertical curves of 200-1000m radius at every grade
//     change point.

void ProfileData::SmoothProfile(std::vector<ProfilePoint>& points) const
{
    if (points.size() < 3) return;

    // Helper: Gaussian-smooth an elevation profile (surface points only)
    auto gaussianSmooth = [&](double sigmaKm) {
        double windowKm = sigmaKm * 3.0;
        std::vector<double> smoothed(points.size());

        for (size_t i = 0; i < points.size(); i++) {
            if (points[i].medium == 'U' || points[i].elevation < -9000) {
                smoothed[i] = points[i].elevation;
                continue;
            }

            double weightSum = 0;
            double valueSum = 0;
            double centerKm = points[i].km;

            for (size_t j = 0; j < points.size(); j++) {
                if (points[j].medium == 'U' || points[j].elevation < -9000)
                    continue;
                double dkm = points[j].km - centerKm;
                if (std::abs(dkm) > windowKm) {
                    if (dkm > windowKm) break;  // sorted by km
                    continue;
                }
                double w = std::exp(-0.5 * (dkm / sigmaKm) * (dkm / sigmaKm));
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
    };

    // --- Pass 1: Gaussian smoothing (σ = 210m) ---
    gaussianSmooth(0.21);

    // --- Pass 2 & 3: Gradient limiting ---
    // Norwegian main lines typically max out at 2.0-2.5% grade
    // (Bergensbanen has sections at 2.1%, Flåmsbana at 5.5% but that's
    // exceptional).  We use 2.5% as a generous upper bound.
    constexpr double kMaxGrade = 0.025;  // 2.5% = 25‰

    // Forward sweep: limit how fast elevation can rise
    for (size_t i = 1; i < points.size(); i++) {
        if (points[i].medium == 'U' || points[i].elevation < -9000) continue;
        if (points[i - 1].elevation < -9000) continue;

        double dkm = points[i].km - points[i - 1].km;
        if (dkm <= 0) continue;
        double maxRise = kMaxGrade * dkm * 1000.0;  // km to metres

        double diff = points[i].elevation - points[i - 1].elevation;
        if (diff > maxRise)
            points[i].elevation = points[i - 1].elevation + maxRise;
    }

    // Backward sweep: limit how fast elevation can drop
    for (size_t i = points.size() - 1; i > 0; i--) {
        if (points[i - 1].medium == 'U' || points[i - 1].elevation < -9000) continue;
        if (points[i].elevation < -9000) continue;

        double dkm = points[i].km - points[i - 1].km;
        if (dkm <= 0) continue;
        double maxRise = kMaxGrade * dkm * 1000.0;

        double diff = points[i - 1].elevation - points[i].elevation;
        if (diff > maxRise)
            points[i - 1].elevation = points[i].elevation + maxRise;
    }

    // --- Pass 4: Vertical curve smoothing ---
    // The gradient limiting can create sharp "V" and "Λ" shapes where
    // the grade snaps from e.g. +2.5% to -2.5%.  Real railways use
    // parabolic vertical curves (200-1000m long) at every grade change.
    //
    // We approximate this by smoothing the gradient profile itself:
    //  1. Compute gradient at each point
    //  2. Smooth gradients with Gaussian (σ = 500m ≈ typical vertical curve)
    //  3. Reconstruct elevations from the smoothed gradients
    //  4. Anchor the reconstructed profile to minimize drift

    // Collect indices of surface points with valid elevation
    std::vector<size_t> surfIdx;
    for (size_t i = 0; i < points.size(); i++) {
        if (points[i].medium != 'U' && points[i].elevation > -9000)
            surfIdx.push_back(i);
    }

    if (surfIdx.size() < 3) return;

    // Compute gradients between consecutive surface points (‰)
    size_t nSurf = surfIdx.size();
    std::vector<double> grades(nSurf, 0.0);
    for (size_t si = 1; si < nSurf; si++) {
        size_t i = surfIdx[si], ip = surfIdx[si - 1];
        double dkm = points[i].km - points[ip].km;
        if (dkm > 0)
            grades[si] = (points[i].elevation - points[ip].elevation) / (dkm * 1000.0);
    }
    grades[0] = grades[1];  // extend first gradient

    // Gaussian-smooth the gradient series (σ = 0.42 km)
    constexpr double kGradeSigmaKm = 0.42;
    constexpr double kGradeWindowKm = kGradeSigmaKm * 3.0;
    std::vector<double> smoothGrades(nSurf);

    for (size_t si = 0; si < nSurf; si++) {
        double centerKm = points[surfIdx[si]].km;
        double wSum = 0, vSum = 0;

        for (size_t sj = 0; sj < nSurf; sj++) {
            double dkm = points[surfIdx[sj]].km - centerKm;
            if (std::abs(dkm) > kGradeWindowKm) {
                if (dkm > kGradeWindowKm) break;
                continue;
            }
            double w = std::exp(-0.5 * (dkm / kGradeSigmaKm) *
                                       (dkm / kGradeSigmaKm));
            wSum += w;
            vSum += w * grades[sj];
        }

        smoothGrades[si] = (wSum > 0) ? vSum / wSum : grades[si];
    }

    // Reconstruct elevations from smoothed gradients, anchored at start
    std::vector<double> reconstructed(nSurf);
    reconstructed[0] = points[surfIdx[0]].elevation;
    for (size_t si = 1; si < nSurf; si++) {
        double dkm = points[surfIdx[si]].km - points[surfIdx[si - 1]].km;
        reconstructed[si] = reconstructed[si - 1] +
                            smoothGrades[si] * dkm * 1000.0;
    }

    // Correct drift: the reconstructed profile may drift away from
    // the gradient-limited one.  Blend to keep it close: apply a
    // linear correction so start and end match the original.
    double origStart = points[surfIdx[0]].elevation;
    double origEnd   = points[surfIdx[nSurf - 1]].elevation;
    double reconEnd  = reconstructed[nSurf - 1];
    double drift = (origEnd - reconEnd);
    double totalKm = points[surfIdx[nSurf - 1]].km - points[surfIdx[0]].km;

    for (size_t si = 0; si < nSurf; si++) {
        double frac = (totalKm > 0) ?
            (points[surfIdx[si]].km - points[surfIdx[0]].km) / totalKm : 0;
        points[surfIdx[si]].elevation = reconstructed[si] + drift * frac;
    }
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

#include "RoadProfileData.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

// ─── RoadId ─────────────────────────────────────────────────────────

std::string RoadId::Label() const
{
    switch (kategori) {
        case 'E': return "E" + std::to_string(nummer);
        case 'R': return "Rv" + std::to_string(nummer);
        case 'F': return "Fv" + std::to_string(nummer);
        case 'K': return "Kv" + std::to_string(nummer);
        case 'P': return "Pv" + std::to_string(nummer);
        default:  return std::string(1, kategori) + std::to_string(nummer);
    }
}

bool RoadId::operator<(const RoadId& o) const
{
    // Sort order: E < R < F < K < P, then by number
    static const char* order = "ERFKP";
    auto rank = [](char c) {
        const char* p = std::strchr(order, c);
        return p ? static_cast<int>(p - order) : 99;
    };
    int ra = rank(kategori), rb = rank(o.kategori);
    if (ra != rb) return ra < rb;
    return nummer < o.nummer;
}

bool RoadId::operator==(const RoadId& o) const
{
    return kategori == o.kategori && nummer == o.nummer;
}

// ─── Smoothing parameters ───────────────────────────────────────────

RoadProfileData::SmoothParams RoadProfileData::GetSmoothParams(char kategori)
{
    switch (kategori) {
        case 'E': return {0.10, 0.05, 0.20, 25.0};
        case 'R': return {0.08, 0.06, 0.15, 25.0};
        case 'F': return {0.05, 0.08, 0.10, 25.0};
        case 'K': return {0.03, 0.10, 0.08, 50.0};
        default:  return {0.02, 0.12, 0.05, 50.0};  // P and others
    }
}

// ─── Road list ──────────────────────────────────────────────────────

std::vector<RoadId> RoadProfileData::GetRoadList(const std::string& roadsPath) const
{
    std::set<RoadId> roads;

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(roadsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return {};

    OGRLayer* layer = ds->GetLayerByName("roads");
    if (!layer) { GDALClose(ds); return {}; }

    layer->ResetReading();
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        const char* kat = feat->GetFieldAsString("vegkategori");
        int num = feat->GetFieldAsInteger("vegnummer");
        if (kat && kat[0] && num > 0)
            roads.insert({kat[0], num});
        OGRFeature::DestroyFeature(feat);
    }

    GDALClose(ds);

    return {roads.begin(), roads.end()};
}

// ─── Segment chaining ───────────────────────────────────────────────

namespace {

using Coord3D = std::tuple<double, double, double>;

struct RawSeg {
    std::vector<Coord3D> coords;
    double length = 0;
};

// Compute 2D length of a segment (horizontal distance only)
double SegLength(const std::vector<Coord3D>& c) {
    double len = 0;
    for (size_t i = 1; i < c.size(); i++) {
        double dx = std::get<0>(c[i]) - std::get<0>(c[i-1]);
        double dy = std::get<1>(c[i]) - std::get<1>(c[i-1]);
        len += std::sqrt(dx * dx + dy * dy);
    }
    return len;
}

} // anon namespace

std::vector<Coord3D> RoadProfileData::ChainSegments(
    const std::string& roadsPath,
    const RoadId& road) const
{
    // Load all segments for this road
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(roadsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return {};

    OGRLayer* layer = ds->GetLayerByName("roads");
    if (!layer) { GDALClose(ds); return {}; }

    std::string filter = "vegkategori = '" + std::string(1, road.kategori) +
                         "' AND vegnummer = " + std::to_string(road.nummer);
    layer->SetAttributeFilter(filter.c_str());

    // Get road CRS and set up transform to EPSG:25833
    const OGRSpatialReference* roadSrs = layer->GetSpatialRef();
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

    std::vector<RawSeg> segments;
    layer->ResetReading();
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        OGRGeometry* geom = feat->GetGeometryRef();
        if (!geom || wkbFlatten(geom->getGeometryType()) != wkbLineString) {
            OGRFeature::DestroyFeature(feat);
            continue;
        }

        OGRLineString* line = static_cast<OGRLineString*>(geom);
        RawSeg seg;
        int nPts = line->getNumPoints();
        for (int p = 0; p < nPts; p++) {
            double x = line->getX(p), y = line->getY(p);
            double z = line->getZ(p);
            if (toUniform)
                toUniform->Transform(1, &x, &y);
            seg.coords.push_back({x, y, z});
        }
        if (seg.coords.size() >= 2) {
            seg.length = SegLength(seg.coords);
            segments.push_back(std::move(seg));
        }

        OGRFeature::DestroyFeature(feat);
    }

    if (toUniform) OCTDestroyCoordinateTransformation(toUniform);
    GDALClose(ds);

    if (segments.empty()) return {};
    if (segments.size() == 1) return segments[0].coords;

    // ── Build a graph over segment endpoints ──
    // Nodes are unique positions (endpoints snapped within tolerance).
    // Edges are segments connecting two nodes.
    constexpr double kMatchTol = 10.0;  // metres
    constexpr double kSnapGrid = 1.0;   // 1m grid for node identification

    // Snap a coordinate to a grid cell key
    auto snapKey = [](double x, double y) -> int64_t {
        int gx = static_cast<int>(std::round(x));
        int gy = static_cast<int>(std::round(y));
        return static_cast<int64_t>(gx) * 10000007LL + gy;
    };

    // Assign each endpoint to a node ID.  Use a spatial approach:
    // collect all endpoints, sort, and merge nearby ones.
    struct EndPt {
        double x, y;
        int segIdx;
        bool isEnd;
    };
    std::vector<EndPt> allEndpoints;
    allEndpoints.reserve(segments.size() * 2);
    for (int i = 0; i < static_cast<int>(segments.size()); i++) {
        auto& s = segments[i];
        allEndpoints.push_back({std::get<0>(s.coords.front()), std::get<1>(s.coords.front()), i, false});
        allEndpoints.push_back({std::get<0>(s.coords.back()), std::get<1>(s.coords.back()), i, true});
    }

    // Assign node IDs: endpoints within kMatchTol get the same node.
    // Use a grid-cell approach with multi-cell lookup.
    int nextNodeId = 0;
    std::vector<int> epNodeId(allEndpoints.size(), -1);

    // Grid: cell → list of (endpoint index, node ID, x, y)
    struct GridEntry { int epIdx; int nodeId; double x, y; };
    std::unordered_map<int64_t, std::vector<GridEntry>> nodeGrid;
    constexpr double kNodeCell = 10.0;

    auto nodeGridKey = [&](double x, double y) -> int64_t {
        int gx = static_cast<int>(std::floor(x / kNodeCell));
        int gy = static_cast<int>(std::floor(y / kNodeCell));
        return static_cast<int64_t>(gx) * 10000007LL + gy;
    };

    auto findNearNode = [&](double x, double y) -> int {
        int cx = static_cast<int>(std::floor(x / kNodeCell));
        int cy = static_cast<int>(std::floor(y / kNodeCell));
        double bestD2 = kMatchTol * kMatchTol;
        int bestNode = -1;
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                int64_t k = static_cast<int64_t>(cx + dx) * 10000007LL + (cy + dy);
                auto it = nodeGrid.find(k);
                if (it == nodeGrid.end()) continue;
                for (auto& ge : it->second) {
                    double ddx = ge.x - x, ddy = ge.y - y;
                    double d2 = ddx * ddx + ddy * ddy;
                    if (d2 < bestD2) {
                        bestD2 = d2;
                        bestNode = ge.nodeId;
                    }
                }
            }
        }
        return bestNode;
    };

    for (int i = 0; i < static_cast<int>(allEndpoints.size()); i++) {
        auto& ep = allEndpoints[i];
        int existing = findNearNode(ep.x, ep.y);
        if (existing >= 0) {
            epNodeId[i] = existing;
        } else {
            int nid = nextNodeId++;
            epNodeId[i] = nid;
            int64_t k = nodeGridKey(ep.x, ep.y);
            nodeGrid[k].push_back({i, nid, ep.x, ep.y});
        }
    }

    int numNodes = nextNodeId;

    // Map segment → (startNode, endNode)
    struct SegEdge { int segIdx; int nodeA, nodeB; };
    std::vector<SegEdge> edges(segments.size());
    for (int i = 0; i < static_cast<int>(segments.size()); i++) {
        edges[i] = {i, epNodeId[2 * i], epNodeId[2 * i + 1]};
    }

    // Adjacency list: node → [(neighborNode, segIdx)]
    std::vector<std::vector<std::pair<int, int>>> adj(numNodes);
    for (int i = 0; i < static_cast<int>(segments.size()); i++) {
        int a = edges[i].nodeA, b = edges[i].nodeB;
        if (a == b) continue;  // degenerate
        adj[a].push_back({b, i});
        adj[b].push_back({a, i});
    }

    // ── Find the diameter of the largest connected component ──
    // BFS from node 0 of the largest component → farthest node A
    // BFS from A → farthest node B, using weighted distance (segment length)
    // The path A→B is the main road spine.

    // Weighted BFS (Dijkstra) to find farthest node, returning parent edges
    auto dijkstra = [&](int start) -> std::pair<int, std::vector<int>> {
        // Returns (farthest node, parent-edge-index for each node)
        std::vector<double> dist(numNodes, -1);
        std::vector<int> parentEdge(numNodes, -1);
        // Simple priority queue: (distance, node)
        using PQE = std::pair<double, int>;
        std::priority_queue<PQE, std::vector<PQE>, std::greater<PQE>> pq;
        dist[start] = 0;
        pq.push({0, start});

        int farthest = start;
        double maxDist = 0;

        while (!pq.empty()) {
            auto [d, u] = pq.top(); pq.pop();
            if (d > dist[u]) continue;

            if (d > maxDist) {
                maxDist = d;
                farthest = u;
            }

            for (auto [v, segIdx] : adj[u]) {
                double nd = d + segments[segIdx].length;
                if (dist[v] < 0 || nd < dist[v]) {
                    dist[v] = nd;
                    parentEdge[v] = segIdx;
                    pq.push({nd, v});
                }
            }
        }

        return {farthest, std::move(parentEdge)};
    };

    // Find largest connected component by total segment length
    std::vector<bool> visited(numNodes, false);
    int bestCompStart = 0;
    double bestCompLen = 0;

    for (int n = 0; n < numNodes; n++) {
        if (visited[n]) continue;
        // BFS to find component size
        std::vector<int> comp;
        std::queue<int> q;
        q.push(n);
        visited[n] = true;
        double compLen = 0;
        while (!q.empty()) {
            int u = q.front(); q.pop();
            comp.push_back(u);
            for (auto [v, segIdx] : adj[u]) {
                compLen += segments[segIdx].length * 0.5; // each edge counted twice
                if (!visited[v]) {
                    visited[v] = true;
                    q.push(v);
                }
            }
        }
        if (compLen > bestCompLen) {
            bestCompLen = compLen;
            bestCompStart = n;
        }
    }

    // Double Dijkstra to find diameter path
    auto [nodeA, parentA] = dijkstra(bestCompStart);
    auto [nodeB, parentB] = dijkstra(nodeA);

    // Reconstruct path from nodeA to nodeB using parentB edges
    std::vector<int> pathSegments;  // segment indices in order
    std::vector<bool> pathDirection; // true = traverse segment reversed (B→A)
    {
        int cur = nodeB;
        while (parentB[cur] >= 0) {
            int segIdx = parentB[cur];
            pathSegments.push_back(segIdx);
            // Determine direction: the segment goes from nodeA to nodeB
            // If edges[segIdx].nodeB == cur, traversal is A→B (normal)
            // If edges[segIdx].nodeA == cur, traversal is B→A (reversed)
            bool rev = (edges[segIdx].nodeA == cur);
            pathDirection.push_back(rev);
            cur = rev ? edges[segIdx].nodeB : edges[segIdx].nodeA;
        }
        // Path is built backwards (from B to A), reverse it
        std::reverse(pathSegments.begin(), pathSegments.end());
        std::reverse(pathDirection.begin(), pathDirection.end());
    }

    // ── Collect coordinates along the path ──
    constexpr double kDupTol2 = kMatchTol * kMatchTol;
    std::vector<Coord3D> chain;
    chain.reserve(pathSegments.size() * 10);

    for (size_t pi = 0; pi < pathSegments.size(); pi++) {
        auto& seg = segments[pathSegments[pi]];
        bool rev = pathDirection[pi];

        if (rev) {
            for (int i = static_cast<int>(seg.coords.size()) - 1; i >= 0; i--) {
                if (!chain.empty() && i == static_cast<int>(seg.coords.size()) - 1) {
                    double dx = std::get<0>(seg.coords[i]) - std::get<0>(chain.back());
                    double dy = std::get<1>(seg.coords[i]) - std::get<1>(chain.back());
                    if (dx * dx + dy * dy < kDupTol2) continue;
                }
                chain.push_back(seg.coords[i]);
            }
        } else {
            for (size_t i = 0; i < seg.coords.size(); i++) {
                if (!chain.empty() && i == 0) {
                    double dx = std::get<0>(seg.coords[i]) - std::get<0>(chain.back());
                    double dy = std::get<1>(seg.coords[i]) - std::get<1>(chain.back());
                    if (dx * dx + dy * dy < kDupTol2) continue;
                }
                chain.push_back(seg.coords[i]);
            }
        }
    }

    return chain;
}

// ─── Intersection detection ─────────────────────────────────────────

namespace {

// 2D line segment intersection test
// Returns true if segment (a1,a2) intersects (b1,b2), with parameter t on seg A
bool SegmentIntersect(double a1x, double a1y, double a2x, double a2y,
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

// Compute cumulative 2D distances along a polyline
std::vector<double> CumulativeDistances(
    const std::vector<std::pair<double, double>>& coords)
{
    std::vector<double> dist(coords.size(), 0.0);
    for (size_t i = 1; i < coords.size(); i++) {
        double dx = coords[i].first - coords[i - 1].first;
        double dy = coords[i].second - coords[i - 1].second;
        dist[i] = dist[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    return dist;
}

// Overload for 3D coordinates (uses 2D horizontal distance)
std::vector<double> CumulativeDistances(
    const std::vector<Coord3D>& coords)
{
    std::vector<double> dist(coords.size(), 0.0);
    for (size_t i = 1; i < coords.size(); i++) {
        double dx = std::get<0>(coords[i]) - std::get<0>(coords[i - 1]);
        double dy = std::get<1>(coords[i]) - std::get<1>(coords[i - 1]);
        dist[i] = dist[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    return dist;
}

} // anon namespace

std::vector<RoadIntersection> RoadProfileData::FindIntersections(
    const std::string& roadsPath,
    const std::string& railwayPath,
    const RoadId& excludeRoad,
    const std::vector<std::pair<double, double>>& chainedCoords,
    const std::vector<ProfilePoint>& profilePoints,
    ProfileData& profileData) const
{
    if (chainedCoords.size() < 2) return {};

    std::vector<RoadIntersection> result;

    // Compute bounding box of the chained road (with padding)
    double bboxMinX = 1e18, bboxMinY = 1e18;
    double bboxMaxX = -1e18, bboxMaxY = -1e18;
    for (const auto& [x, y] : chainedCoords) {
        bboxMinX = std::min(bboxMinX, x);
        bboxMaxX = std::max(bboxMaxX, x);
        bboxMinY = std::min(bboxMinY, y);
        bboxMaxY = std::max(bboxMaxY, y);
    }
    constexpr double kPad = 100.0;
    bboxMinX -= kPad; bboxMinY -= kPad;
    bboxMaxX += kPad; bboxMaxY += kPad;

    auto cumDist = CumulativeDistances(chainedCoords);
    double totalLen = cumDist.back();

    // Helper: find km on the profile for a given intersection point
    auto findKm = [&](size_t segIdx, double t) -> double {
        double distAlongChain = cumDist[segIdx] +
            t * (cumDist[segIdx + 1] - cumDist[segIdx]);
        return distAlongChain / 1000.0;
    };

    // Helper: test a foreign linestring against our chain
    auto testLine = [&](OGRLineString* line,
                        const std::function<void(double km, double ix, double iy)>& onHit) {
        int nPts = line->getNumPoints();
        for (int p = 0; p + 1 < nPts; p++) {
            double bx1 = line->getX(p), by1 = line->getY(p);
            double bx2 = line->getX(p + 1), by2 = line->getY(p + 1);

            // Quick bbox reject
            double segMinX = std::min(bx1, bx2), segMaxX = std::max(bx1, bx2);
            double segMinY = std::min(by1, by2), segMaxY = std::max(by1, by2);
            if (segMaxX < bboxMinX || segMinX > bboxMaxX ||
                segMaxY < bboxMinY || segMinY > bboxMaxY)
                continue;

            for (size_t ci = 0; ci + 1 < chainedCoords.size(); ci++) {
                double tA;
                if (SegmentIntersect(
                        chainedCoords[ci].first, chainedCoords[ci].second,
                        chainedCoords[ci + 1].first, chainedCoords[ci + 1].second,
                        bx1, by1, bx2, by2, tA)) {
                    double km = findKm(ci, tA);
                    double ix = chainedCoords[ci].first +
                        tA * (chainedCoords[ci + 1].first - chainedCoords[ci].first);
                    double iy = chainedCoords[ci].second +
                        tA * (chainedCoords[ci + 1].second - chainedCoords[ci].second);
                    onHit(km, ix, iy);
                }
            }
        }
    };

    // --- Road-road intersections ---
    {
        GDALDataset* ds = static_cast<GDALDataset*>(
            GDALOpenEx(roadsPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (ds) {
            OGRLayer* layer = ds->GetLayerByName("roads");
            if (layer) {
                // Set up CRS transform if needed
                const OGRSpatialReference* roadSrs = layer->GetSpatialRef();
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

                // Spatial filter in road CRS
                if (toUniform) {
                    // Transform bbox back to road CRS for spatial filter
                    OGRSpatialReference src25833;
                    src25833.importFromEPSG(25833);
                    src25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    OGRSpatialReference roadCrs(*roadSrs);
                    roadCrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    OGRCoordinateTransformation* fromUniform =
                        OGRCreateCoordinateTransformation(&src25833, &roadCrs);
                    if (fromUniform) {
                        double x1 = bboxMinX, y1 = bboxMinY;
                        double x2 = bboxMaxX, y2 = bboxMaxY;
                        fromUniform->Transform(1, &x1, &y1);
                        fromUniform->Transform(1, &x2, &y2);
                        layer->SetSpatialFilterRect(
                            std::min(x1, x2), std::min(y1, y2),
                            std::max(x1, x2), std::max(y1, y2));
                        OCTDestroyCoordinateTransformation(fromUniform);
                    }
                } else {
                    layer->SetSpatialFilterRect(bboxMinX, bboxMinY,
                                                bboxMaxX, bboxMaxY);
                }

                // Only show crossings with roads of significant categories
                // For E-roads: show E, R, F crossings
                // For R-roads: show E, R, F crossings
                // For F-roads: show E, R, F crossings
                // For K-roads: show E, R, F, K crossings
                // For P-roads: show all
                std::string catFilter;
                switch (excludeRoad.kategori) {
                    case 'E': case 'R': case 'F':
                        catFilter = "vegkategori IN ('E','R','F')";
                        break;
                    case 'K':
                        catFilter = "vegkategori IN ('E','R','F','K')";
                        break;
                    default:
                        catFilter = "vegkategori IN ('E','R','F','K','P')";
                        break;
                }
                layer->SetAttributeFilter(catFilter.c_str());

                // Track which roads we've already recorded intersections for
                // to deduplicate closely spaced crossings of the same road
                struct CrossingKey {
                    char kat; int num;
                    bool operator<(const CrossingKey& o) const {
                        return kat != o.kat ? kat < o.kat : num < o.num;
                    }
                };
                std::map<CrossingKey, std::vector<RoadIntersection>> roadHits;

                layer->ResetReading();
                OGRFeature* feat;
                while ((feat = layer->GetNextFeature()) != nullptr) {
                    const char* kat = feat->GetFieldAsString("vegkategori");
                    int num = feat->GetFieldAsInteger("vegnummer");
                    char katC = (kat && kat[0]) ? kat[0] : '?';

                    // Skip self
                    if (katC == excludeRoad.kategori && num == excludeRoad.nummer) {
                        OGRFeature::DestroyFeature(feat);
                        continue;
                    }

                    OGRGeometry* geom = feat->GetGeometryRef();
                    if (!geom || wkbFlatten(geom->getGeometryType()) != wkbLineString) {
                        OGRFeature::DestroyFeature(feat);
                        continue;
                    }

                    OGRLineString* line = static_cast<OGRLineString*>(geom->clone());
                    if (toUniform)
                        line->transform(toUniform);

                    RoadId crossId{katC, num};
                    CrossingKey key{katC, num};

                    testLine(line, [&](double km, double ix, double iy) {
                        RoadIntersection ri;
                        ri.km = km;
                        ri.x = ix;
                        ri.y = iy;
                        ri.crossingRoad = crossId;
                        ri.isRailway = false;

                        float elev;
                        if (profileData.SampleElevation(ix, iy, elev))
                            ri.elevation = elev;
                        else
                            ri.elevation = 0;

                        roadHits[key].push_back(ri);
                    });

                    delete line;
                    OGRFeature::DestroyFeature(feat);
                }

                if (toUniform)
                    OCTDestroyCoordinateTransformation(toUniform);

                // Deduplicate: for each crossing road, merge hits within 0.5 km
                for (auto& [key, hits] : roadHits) {
                    std::sort(hits.begin(), hits.end(),
                              [](const RoadIntersection& a, const RoadIntersection& b) {
                                  return a.km < b.km;
                              });

                    size_t i = 0;
                    while (i < hits.size()) {
                        // Find cluster of hits within 0.5 km
                        size_t j = i + 1;
                        while (j < hits.size() && hits[j].km - hits[i].km < 0.5)
                            j++;
                        // Pick the middle one
                        result.push_back(hits[i + (j - i) / 2]);
                        i = j;
                    }
                }
            }
            GDALClose(ds);
        }
    }

    // --- Road-railway intersections ---
    if (!railwayPath.empty()) {
        GDALDataset* ds = static_cast<GDALDataset*>(
            GDALOpenEx(railwayPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (ds) {
            OGRLayer* tracks = ds->GetLayerByName("Banelenke");
            if (tracks) {
                tracks->SetAttributeFilter("banestatus = 'I'");

                // Transform railway geometry to EPSG:25833
                const OGRSpatialReference* railSrs = tracks->GetSpatialRef();
                OGRSpatialReference srs25833;
                srs25833.importFromEPSG(25833);
                srs25833.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

                OGRCoordinateTransformation* toUniform = nullptr;
                if (railSrs) {
                    OGRSpatialReference src(*railSrs);
                    src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    if (!src.IsSame(&srs25833))
                        toUniform = OGRCreateCoordinateTransformation(&src, &srs25833);
                }

                // Track rail line crossings to deduplicate
                std::map<std::string, std::vector<RoadIntersection>> railHits;

                tracks->ResetReading();
                OGRFeature* feat;
                while ((feat = tracks->GetNextFeature()) != nullptr) {
                    OGRGeometry* geom = feat->GetGeometryRef();
                    if (!geom) { OGRFeature::DestroyFeature(feat); continue; }

                    const char* name = feat->GetFieldAsString("banenavn");
                    std::string lineName = name ? name : "";

                    OGRGeometry* clone = geom->clone();
                    if (toUniform) clone->transform(toUniform);

                    // Quick bbox check
                    OGREnvelope env;
                    clone->getEnvelope(&env);
                    if (env.MaxX < bboxMinX || env.MinX > bboxMaxX ||
                        env.MaxY < bboxMinY || env.MinY > bboxMaxY) {
                        delete clone;
                        OGRFeature::DestroyFeature(feat);
                        continue;
                    }

                    auto processLine = [&](OGRLineString* line) {
                        testLine(line, [&](double km, double ix, double iy) {
                            RoadIntersection ri;
                            ri.km = km;
                            ri.x = ix;
                            ri.y = iy;
                            ri.railLine = lineName;
                            ri.isRailway = true;

                            float elev;
                            if (profileData.SampleElevation(ix, iy, elev))
                                ri.elevation = elev;
                            else
                                ri.elevation = 0;

                            railHits[lineName].push_back(ri);
                        });
                    };

                    OGRwkbGeometryType gtype = wkbFlatten(clone->getGeometryType());
                    if (gtype == wkbLineString) {
                        processLine(static_cast<OGRLineString*>(clone));
                    } else if (gtype == wkbMultiLineString) {
                        auto* multi = static_cast<OGRMultiLineString*>(clone);
                        for (int g = 0; g < multi->getNumGeometries(); g++)
                            processLine(static_cast<OGRLineString*>(
                                multi->getGeometryRef(g)));
                    }

                    delete clone;
                    OGRFeature::DestroyFeature(feat);
                }

                if (toUniform)
                    OCTDestroyCoordinateTransformation(toUniform);

                // Deduplicate rail crossings within 0.5 km
                for (auto& [name, hits] : railHits) {
                    std::sort(hits.begin(), hits.end(),
                              [](const RoadIntersection& a, const RoadIntersection& b) {
                                  return a.km < b.km;
                              });
                    size_t i = 0;
                    while (i < hits.size()) {
                        size_t j = i + 1;
                        while (j < hits.size() && hits[j].km - hits[i].km < 0.5)
                            j++;
                        result.push_back(hits[i + (j - i) / 2]);
                        i = j;
                    }
                }
            }
            GDALClose(ds);
        }
    }

    // Sort all intersections by km
    std::sort(result.begin(), result.end(),
              [](const RoadIntersection& a, const RoadIntersection& b) {
                  return a.km < b.km;
              });

    return result;
}

// ─── Build road profile ─────────────────────────────────────────────

RoadProfileResult RoadProfileData::BuildRoadProfile(
    const std::string& roadsPath,
    const std::string& railwayPath,
    const RoadId& road,
    ProfileData& profileData) const
{
    RoadProfileResult result;
    result.roadId = road;
    result.stats.lineName = road.Label();

    // Chain segments — returns 3D coordinates with embedded elevation
    auto chain3d = ChainSegments(roadsPath, road);
    if (chain3d.size() < 2) return result;

    // Compute cumulative 2D distance along the chain
    auto cumDist = CumulativeDistances(chain3d);
    double totalLen = cumDist.back();
    if (totalLen < 1.0) return result;

    // Resample at regular spacing for a clean profile
    auto params = GetSmoothParams(road.kategori);
    int nSamples = std::max(2, static_cast<int>(totalLen / params.sampleSpacing) + 1);

    auto interpolatePoint = [&](double targetDist) -> std::tuple<double, double, double> {
        auto it = std::upper_bound(cumDist.begin(), cumDist.end(), targetDist);
        size_t idx = (it == cumDist.begin()) ? 0 :
                     static_cast<size_t>(it - cumDist.begin() - 1);
        if (idx >= chain3d.size() - 1) idx = chain3d.size() - 2;

        double segLen = cumDist[idx + 1] - cumDist[idx];
        double f = (segLen > 0) ? (targetDist - cumDist[idx]) / segLen : 0;
        f = std::clamp(f, 0.0, 1.0);

        auto& [x0, y0, z0] = chain3d[idx];
        auto& [x1, y1, z1] = chain3d[idx + 1];
        return {x0 + f * (x1 - x0),
                y0 + f * (y1 - y0),
                z0 + f * (z1 - z0)};
    };

    for (int s = 0; s < nSamples; s++) {
        double frac = static_cast<double>(s) / (nSamples - 1);
        double dist = frac * totalLen;
        auto [x, y, z] = interpolatePoint(dist);

        ProfilePoint pt;
        pt.km = dist / 1000.0;
        pt.x = x;
        pt.y = y;
        pt.elevation = z;
        pt.medium = ' ';
        pt.interpolated = false;

        result.points.push_back(pt);
    }

    if (result.points.empty()) return result;

    // Extract 2D chain for intersection detection
    std::vector<std::pair<double, double>> chain2d;
    chain2d.reserve(chain3d.size());
    for (auto& [x, y, z] : chain3d)
        chain2d.push_back({x, y});

    // Find intersections
    result.intersections = FindIntersections(
        roadsPath, railwayPath, road, chain2d, result.points, profileData);

    // Update intersection elevations from profile
    for (auto& ix : result.intersections) {
        double bestDist = 1e9;
        for (const auto& pt : result.points) {
            double d = std::abs(pt.km - ix.km);
            if (d < bestDist) {
                bestDist = d;
                ix.elevation = pt.elevation;
            }
        }
    }

    // Compute stats
    auto& st = result.stats;
    st.lineName = road.Label();
    st.totalLengthKm = result.points.back().km - result.points.front().km;
    st.minElev = 1e9;
    st.maxElev = -1e9;
    st.totalClimb = 0;
    st.totalDescent = 0;
    st.maxGradePct = 0;
    st.tunnelLengthKm = 0;
    st.bridgeLengthKm = 0;
    st.segmentCount = static_cast<int>(chain3d.size());
    st.stationCount = static_cast<int>(result.intersections.size());

    for (size_t i = 0; i < result.points.size(); i++) {
        double e = result.points[i].elevation;
        st.minElev = std::min(st.minElev, e);
        st.maxElev = std::max(st.maxElev, e);

        if (i > 0) {
            double de = e - result.points[i - 1].elevation;
            double dk = (result.points[i].km - result.points[i - 1].km) * 1000.0;
            if (de > 0) st.totalClimb += de;
            else st.totalDescent += -de;
            if (dk > 0) {
                double grade = std::abs(de / dk) * 100.0;
                st.maxGradePct = std::max(st.maxGradePct, grade);
            }
        }
    }

    if (st.minElev > 1e8) st.minElev = 0;
    if (st.maxElev < -1e8) st.maxElev = 0;

    return result;
}

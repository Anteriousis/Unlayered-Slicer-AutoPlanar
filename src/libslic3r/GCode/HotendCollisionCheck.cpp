#include "HotendCollisionCheck.hpp"

#include "../libslic3r.h"
#include "../I18N.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Slic3r {

namespace {

// Rotation R with R * n == +Z (Rodrigues); identity when n already points up.
Eigen::Matrix3d rotation_to_z(const Vec3d &axis)
{
    const Vec3d n = axis.normalized();
    const Vec3d z = Vec3d::UnitZ();
    const Vec3d v = n.cross(z);
    const double s = v.norm();
    const double c = n.dot(z);
    if (s < 1e-12)
        return c > 0. ? Eigen::Matrix3d::Identity() : Eigen::DiagonalMatrix<double, 3>(1., -1., -1.).toDenseMatrix();
    const Vec3d k = v / s;
    Eigen::Matrix3d K;
    K << 0., -k.z(), k.y(),
         k.z(), 0., -k.x(),
        -k.y(), k.x(), 0.;
    return Eigen::Matrix3d::Identity() + s * K + (1. - c) * (K * K);
}

struct Sample
{
    Vec3d        p;        // bed frame
    unsigned int gcode_id; // originating G-code line
};

struct Episode
{
    unsigned int        first_gcode_id = 0;
    int                 object_label   = -1;
    int                 segment        = -1;
    Vec3d               axis           = Vec3d::UnitZ();
    std::vector<Sample> tips;      // every move, extruding or not, sampled every 1 mm
    std::vector<Sample> material;  // extrusions, sampled every 0.5 mm
};

void sample_segment(const Vec3d &a, const Vec3d &b, double step, unsigned int gcode_id, std::vector<Sample> &out)
{
    const Vec3d  d   = b - a;
    const double len = d.norm();
    const int    n   = std::max(2, int(len / step) + 1);
    for (int i = 0; i < n; ++ i)
        out.push_back({a + d * (double(i) / double(n - 1)), gcode_id});
}

// Octagonal iterated dilation: D[cell] = number of steps until reached from an occupied cell
// (max_r + 1 = never). Alternating 4- and 8-neighbour steps approximates the Euclidean distance to
// within ~8%, on the conservative side.
struct DistanceField
{
    int                  nu = 0, nv = 0;
    std::vector<int16_t> d;
    int at(int u, int v) const { return (u < 0 || v < 0 || u >= nu || v >= nv) ? INT16_MAX : d[size_t(u) * nv + v]; }
};

DistanceField distance_field(const std::vector<char> &occ, int nu, int nv, int max_r)
{
    DistanceField out;
    out.nu = nu; out.nv = nv;
    out.d.assign(size_t(nu) * nv, int16_t(max_r + 1));
    std::vector<char> cur = occ, nxt;
    for (size_t i = 0; i < cur.size(); ++ i)
        if (cur[i]) out.d[i] = 0;
    for (int r = 1; r <= max_r; ++ r) {
        nxt = cur;
        bool any = false;
        const bool diag = (r % 2 == 0);
        for (int u = 0; u < nu; ++ u)
            for (int v = 0; v < nv; ++ v) {
                const size_t i = size_t(u) * nv + v;
                if (cur[i]) continue;
                bool reach = (u > 0 && cur[i - nv]) || (u + 1 < nu && cur[i + nv]) || (v > 0 && cur[i - 1]) || (v + 1 < nv && cur[i + 1]);
                if (! reach && diag)
                    reach = (u > 0 && v > 0 && cur[i - nv - 1]) || (u > 0 && v + 1 < nv && cur[i - nv + 1]) ||
                            (u + 1 < nu && v > 0 && cur[i + nv - 1]) || (u + 1 < nu && v + 1 < nv && cur[i + nv + 1]);
                if (reach) { nxt[i] = 1; out.d[i] = int16_t(r); any = true; }
            }
        if (! any) break;
        cur.swap(nxt);
    }
    return out;
}

} // namespace

std::optional<ConflictResult> check_hotend_collisions(const GCodeProcessorResult &result, const HotendCollisionParams &params)
{
    if (params.cone_angle_deg <= 0. || params.cone_height <= 0.)
        return std::nullopt;
    if (result.tool_axes.size() <= 1 || result.moves.size() < 2)
        return std::nullopt;   // planar print: nothing above the tip can have been printed yet

    // --- split the move stream into tool-axis episodes and sample tips / material ---
    std::vector<Episode> episodes;
    auto axis_of = [&result](const GCodeProcessorResult::MoveVertex &m) -> Vec3d {
        const size_t id = m.tool_axis_id < result.tool_axes.size() ? m.tool_axis_id : 0;
        return result.tool_axes[id].cast<double>();
    };
    for (size_t i = 1; i < result.moves.size(); ++ i) {
        const auto &prev = result.moves[i - 1];
        const auto &m    = result.moves[i];
        const Vec3d axis = axis_of(m);
        if (episodes.empty() || (axis - episodes.back().axis).norm() > 1e-6) {
            Episode e;
            e.axis = axis; e.first_gcode_id = m.gcode_id; e.object_label = m.object_label_id; e.segment = m.cut_segment_id;
            episodes.push_back(std::move(e));
        }
        Episode &e = episodes.back();
        if (m.type != EMoveType::Extrude && m.type != EMoveType::Travel)
            continue;
        const Vec3d a = prev.position.cast<double>();
        const Vec3d b = m.position.cast<double>();
        if ((b - a).norm() < 1e-9)
            continue;
        sample_segment(a, b, 1.0, m.gcode_id, e.tips);
        if (m.type == EMoveType::Extrude && m.delta_extruder > 0.f)
            sample_segment(a, b, 0.5, m.gcode_id, e.material);
    }
    if (episodes.size() < 2)
        return std::nullopt;

    const double tan_half = std::tan(0.5 * params.cone_angle_deg * M_PI / 180.);
    const double H        = params.cone_height;
    const double band     = 0.2;
    const int    max_r    = int(std::ceil(H * tan_half + params.margin)) + 1;

    struct Hit { double pen = -1.; size_t episode = 0, source = 0; Sample tip; Sample mat; double height = 0., dist = 0.; size_t count = 0; };
    Hit worst;
    size_t total_hits = 0;

    // material of all earlier episodes, accumulated as we go
    std::vector<Sample> earlier;
    std::vector<size_t> earlier_episode;
    for (size_t ei = 0; ei < episodes.size(); ++ ei) {
        const Episode &e = episodes[ei];
        if (ei > 0 && ! earlier.empty() && ! e.tips.empty()) {
            const Eigen::Matrix3d R = rotation_to_z(e.axis);
            // tool frame: (u, v, a) = R * p
            std::vector<Vec3d> T(e.tips.size()), M(earlier.size());
            for (size_t i = 0; i < T.size(); ++ i) T[i] = R * e.tips[i].p;
            for (size_t i = 0; i < M.size(); ++ i) M[i] = R * earlier[i].p;
            double umin = 1e300, vmin = 1e300, umax = -1e300, vmax = -1e300;
            for (const Vec3d &p : T) { umin = std::min(umin, p.x()); vmin = std::min(vmin, p.y()); umax = std::max(umax, p.x()); vmax = std::max(vmax, p.y()); }
            for (const Vec3d &p : M) { umin = std::min(umin, p.x()); vmin = std::min(vmin, p.y()); umax = std::max(umax, p.x()); vmax = std::max(vmax, p.y()); }
            const int u0 = int(std::floor(umin)) - 1, v0 = int(std::floor(vmin)) - 1;
            const int nu = int(std::ceil(umax)) - u0 + 2, nv = int(std::ceil(vmax)) - v0 + 2;
            if (nu <= 0 || nv <= 0 || size_t(nu) * size_t(nv) > 40'000'000)
                continue; // absurd extents: do not blow memory, skip silently (logged below)

            // tips binned by height band
            std::vector<int> tip_band(T.size());
            int bmin = INT32_MAX, bmax = INT32_MIN;
            for (size_t i = 0; i < T.size(); ++ i) { tip_band[i] = int(std::floor(T[i].z() / band)); bmin = std::min(bmin, tip_band[i]); bmax = std::max(bmax, tip_band[i]); }
            std::vector<std::vector<size_t>> by_band(size_t(bmax - bmin + 1));
            for (size_t i = 0; i < T.size(); ++ i) by_band[size_t(tip_band[i] - bmin)].push_back(i);

            // material voxels (1 mm x 1 mm x band), deduped
            struct MVox { int u, v; double a; size_t sample; };
            std::vector<MVox> mv;
            {
                std::unordered_set<int64_t> seen;
                mv.reserve(M.size());
                for (size_t i = 0; i < M.size(); ++ i) {
                    const int u = int(std::floor(M[i].x())) - u0, v = int(std::floor(M[i].y())) - v0, ab = int(std::floor(M[i].z() / band));
                    const int64_t key = (int64_t(u) * 100003 + v) * 1000003 + ab;
                    if (seen.insert(key).second)
                        mv.push_back({u, v, M[i].z(), i});
                }
            }
            // per band: distance field of the tips, then test every material voxel in the cone's height range
            for (size_t bi = 0; bi < by_band.size(); ++ bi) {
                if (by_band[bi].empty()) continue;
                const double a_t = (double(int(bi) + bmin) + 0.5) * band;
                bool any_candidate = false;
                for (const MVox &q : mv) { const double h = q.a - a_t; if (h > params.eps && h <= H) { any_candidate = true; break; } }
                if (! any_candidate) continue;
                std::vector<char> occ(size_t(nu) * nv, 0);
                for (size_t ti : by_band[bi]) {
                    const int u = int(std::floor(T[ti].x())) - u0, v = int(std::floor(T[ti].y())) - v0;
                    if (u >= 0 && v >= 0 && u < nu && v < nv) occ[size_t(u) * nv + v] = 1;
                }
                const DistanceField D = distance_field(occ, nu, nv, max_r);
                for (const MVox &q : mv) {
                    const double h = q.a - a_t;
                    if (h <= params.eps || h > H) continue;
                    const double d = double(D.at(q.u, q.v));
                    const double reach = h * tan_half + params.margin;
                    if (d > reach) continue;
                    ++ total_hits;
                    const double pen = reach - d;
                    if (pen > worst.pen) {
                        // nearest tip of this band to the voxel, for the report
                        size_t best = by_band[bi].front(); double bestd = 1e300;
                        for (size_t ti : by_band[bi]) {
                            const double du = T[ti].x() - (q.u + u0 + 0.5), dv = T[ti].y() - (q.v + v0 + 0.5);
                            const double dd = du * du + dv * dv;
                            if (dd < bestd) { bestd = dd; best = ti; }
                        }
                        worst.pen = pen; worst.episode = ei; worst.source = earlier_episode[q.sample];
                        worst.tip = e.tips[best]; worst.mat = earlier[q.sample]; worst.height = h; worst.dist = d;
                    }
                }
            }
        }
        for (const Sample &s : e.material) { earlier.push_back(s); earlier_episode.push_back(ei); }
    }

    if (total_hits == 0)
        return std::nullopt;

    auto chunk_name = [&episodes](size_t ei) {
        const Episode &e = episodes[ei];
        std::string name = (boost::format("chunk %1%") % (ei + 1)).str();
        if (e.segment >= 0)          name += (boost::format(" (section %1%)") % (e.segment + 1)).str();
        else if (e.object_label >= 0) name += (boost::format(" (object %1%)") % e.object_label).str();
        name += (boost::format(", tool axis %.2f,%.2f,%.2f") % e.axis.x() % e.axis.y() % e.axis.z()).str();
        return name;
    };
    ConflictResult res(chunk_name(worst.episode), chunk_name(worst.source), worst.tip.p.z(), nullptr, nullptr);
    res.message = (boost::format(_u8L("Hotend collision: modelled as a %1%° cone %2% mm tall, the hotend enters material printed earlier while printing %3% "
                                       "(G-code line %4%, tip at X%5% Y%6% Z%7%): material of %8% (G-code line %9%) sits %10% mm up the tool axis and %11% mm off it, "
                                       "%12% mm inside the cone; %13% hits in total. Re-orient the chunk, add clearance, or shorten the hotend model."))
                   % int(std::lround(params.cone_angle_deg)) % int(std::lround(H)) % res._objName1 % worst.tip.gcode_id
                   % (boost::format("%.1f") % worst.tip.p.x()).str() % (boost::format("%.1f") % worst.tip.p.y()).str() % (boost::format("%.2f") % worst.tip.p.z()).str()
                   % res._objName2 % worst.mat.gcode_id % (boost::format("%.1f") % worst.height).str() % (boost::format("%.0f") % worst.dist).str()
                   % (boost::format("%.1f") % worst.pen).str() % total_hits).str();
    BOOST_LOG_TRIVIAL(error) << "hotend collision check: " << res.message;
    return res;
}

} // namespace Slic3r

#include "SkeletonBuild.hpp"
#include "QuadricEdgeCollapse.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>

namespace Slic3r {

// ── cut ordering ──────────────────────────────────────────────────────────────

std::vector<std::pair<int,int>> build_skeleton_cut_edges(const SkeletonDAG& dag)
{
    const int n = dag.num_nodes();
    if (n == 0)
        return {};

    // One BFS queue per DAG root, round-robin interleaved so parallel branches zigzag.
    std::vector<std::queue<int>> qs;
    std::vector<bool> visited(n, false);
    for (int i = 0; i < n; ++i) {
        if (dag.pred[i].empty()) {
            qs.emplace_back();
            qs.back().push(i);
            visited[i] = true;
        }
    }
    if (qs.empty())
        return {};

    std::vector<std::pair<int,int>> edges;
    edges.reserve(std::max(0, n - 1));

    bool any = true;
    while (any) {
        any = false;
        for (auto& q : qs) {
            if (q.empty())
                continue;
            any = true;
            const int u = q.front();
            q.pop();
            for (int v : dag.succ[u]) {
                if (!visited[v]) {
                    visited[v] = true;
                    edges.emplace_back(u, v);
                    q.push(v);
                }
            }
        }
    }
    return edges;
}

int skeleton_cycle_count(const SkeletonGraph& G)
{
    const int n = G.num_nodes();
    if (n == 0)
        return 0;

    int n_edges = 0;
    for (const auto& a : G.adj)
        n_edges += (int) a.size();
    n_edges /= 2;

    // connected components
    std::vector<bool> seen(n, false);
    int comps = 0;
    for (int i = 0; i < n; ++i) {
        if (seen[i])
            continue;
        ++comps;
        std::queue<int> q;
        q.push(i);
        seen[i] = true;
        while (!q.empty()) {
            const int u = q.front();
            q.pop();
            for (int v : G.adj[u])
                if (!seen[v]) { seen[v] = true; q.push(v); }
        }
    }
    return n_edges - n + comps;
}

// ── the pipeline ──────────────────────────────────────────────────────────────

SkeletonResult build_skeleton(const indexed_triangle_set& its_local,
                              const SkeletonParams&       p,
                              const Transform3d&          world,
                              const SkeletonProgress*     progress)
{
    SkeletonResult r;
    if (its_local.vertices.empty() || its_local.indices.empty()) {
        r.note = "empty mesh";
        return r;
    }

    // Work in world space; see the header note. Only the vertices need moving — face indices and
    // winding are unchanged for a non-mirroring transform, and a mirroring one is handled by the
    // outward-orientation fix inside build_cgal_mesh().
    const bool           world_is_identity = world.matrix().isApprox(Transform3d::Identity().matrix());
    indexed_triangle_set its_world;
    if (!world_is_identity) {
        its_world = its_local;
        for (auto& v : its_world.vertices)
            v = (world * v.cast<double>()).cast<float>();
    }
    const indexed_triangle_set& its = world_is_identity ? its_local : its_world;

    // Phase timings. Skeletonization cost scales steeply with face count and there are three
    // candidates for a slow run (skeletonize / bore tracing / the rest), so the log says which.
    using Clock = std::chrono::steady_clock;
    const auto  t_start = Clock::now();
    auto        t_mark  = t_start;
    // Stage boundaries double as the progress and cancellation checkpoints — the stages
    // themselves are single uninterruptible CGAL calls, so this is as fine-grained as it gets.
    int pct = 0;
    auto lap = [&t_mark, &pct, progress](const char* what, int pct_after = -1) {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - t_mark).count();
        std::cout << "[skeleton timing] " << what << ": " << (int) ms << " ms\n";
        t_mark = now;
        if (pct_after >= 0)
            pct = pct_after;
        if (progress && progress->report)
            progress->report(pct, what);
        return ms;
    };
    auto canceled = [progress]() { return progress && progress->canceled && progress->canceled(); };

    // ── decimate ──
    // MCF cost grows steeply with triangle count while the skeleton it produces is a coarse
    // 1D curve, so density past a few thousand faces buys nothing. Measured on a 42908-face
    // intake: minutes at full density. The decimated copy is used for skeletonization only —
    // bore tracing and bed snapping still slice the ORIGINAL mesh, so accuracy is unaffected.
    const indexed_triangle_set* mesh_for_skel = &its;
    indexed_triangle_set        decimated;
    if (p.decimate_faces > 0 && (int) its.indices.size() > p.decimate_faces) {
        decimated = its;
        uint32_t target = (uint32_t) p.decimate_faces;
        try {
            its_quadric_edge_collapse(decimated, target);
            if (!decimated.indices.empty() && !decimated.vertices.empty()) {
                std::cout << "[skeleton] decimated " << its.indices.size() << " -> "
                          << decimated.indices.size() << " faces for skeletonization\n";
                mesh_for_skel = &decimated;
            }
        } catch (const std::exception& e) {
            std::cout << "[skeleton] decimation failed (" << e.what()
                      << "); skeletonizing at full density\n";
        }
        lap("decimate", 10);
    }

    auto V_f = trianglemesh_to_eigen_vertices(its);
    auto F   = trianglemesh_to_eigen_faces(its);
    Eigen::MatrixXd V = V_f.cast<double>();

    auto Vs_f = trianglemesh_to_eigen_vertices(*mesh_for_skel);
    auto Fs   = trianglemesh_to_eigen_faces(*mesh_for_skel);
    Eigen::MatrixXd Vs = Vs_f.cast<double>();

    const Eigen::Vector3d bb_min = V.colwise().minCoeff().transpose();
    const Eigen::Vector3d bb_max = V.colwise().maxCoeff().transpose();
    const double diag = (bb_max - bb_min).norm();

    r.genus = mesh_genus(Vs, Fs);

    if (p.method == 0)
        r.graph = skeletonize_mesh(Vs, Fs, (double) p.quality_speed, (double) p.min_edge_factor,
                                   (double) p.medial_centering);
    else
        r.graph = skeletonize_mesh_voxel(Vs, Fs, p.voxel_resolution);
    lap(p.method == 0 ? "skeletonize (MCF)" : "skeletonize (voxel)", 55);
    if (canceled()) { r.note = "canceled"; return r; }

    r.cycles_raw = skeleton_cycle_count(r.graph);

    // ── graph cleaning ──
    // These exist to tame MCF noise, so they run BEFORE a traced bore centreline is
    // spliced in: Laplacian smoothing pulls a curve toward its chord, and the traced
    // centreline of a bend is already exact — smoothing it would only flatten the arc.
    if (p.resample_edge_len > 0.f)
        r.graph = resample_skeleton_uniform(r.graph, (double) p.resample_edge_len);
    // F is an Eigen::Map over the mesh's index buffer. It binds to the const& parameters
    // elsewhere by way of a temporary, but a pointer needs a real MatrixXi to point at.
    const Eigen::MatrixXi F_mat = F;
    smooth_skeleton_graph(r.graph, p.smooth_iterations, p.smooth_alpha, true, &V, &F_mat);
    if (p.collapse_junctions)
        r.graph = collapse_nearby_junctions(r.graph);
    if (p.prune_leaves > 0.f)
        r.graph = prune_short_leaves(r.graph, (double) p.prune_leaves);
    lap("clean", 60);
    if (canceled()) { r.note = "canceled"; return r; }

    // ── hollow geometry: trace bores and replace the wall skeleton with the axis ──
    if (p.tunnel_fill != TunnelFill::Off) {
        const double station = p.tunnel_station > 0.f ? (double) p.tunnel_station
                                                      : std::max(0.5, diag / 120.0);
        const double aspect  = p.tunnel_fill == TunnelFill::On ? 0.0 : (double) p.tunnel_min_aspect;
        r.tunnels = find_tunnel_spines(V, F, station, aspect);
        if (!r.tunnels.empty()) {
            r.graph = splice_tunnel_spines(r.graph, r.tunnels);
            std::ostringstream os;
            os << r.tunnels.size() << " bore(s) traced:";
            for (const auto& t : r.tunnels)
                os << " [depth=" << t.depth << " width=" << t.width
                   << " aspect=" << t.aspect() << " pts=" << t.points.size() << "]";
            r.note = os.str();
        } else {
            r.note = "no bore accepted";
        }
        lap("bore tracing", 85);
        if (canceled()) { r.note = "canceled"; return r; }
    } else {
        r.note = "tunnel handling off";
    }

    // ── bed ──
    double bed_z = 0.0;
    r.snapped_nodes = snap_to_bed(r.graph, V, F, (double) p.bed_snap_band, &bed_z);
    r.bed_z = bed_z;
    lap("bed snap", 92);
    if (canceled()) { r.note = "canceled"; return r; }

    // ── extremities ──
    // Undo MCF's tip retraction — but only at the ends the bed snap did not already settle,
    // which is why this runs AFTER it and not before.
    //
    // The two stages reach an end differently on purpose. snap_to_bed drops a tip onto the bed
    // plane, so the branch arrives perpendicular to it and the first cut is flat: the base
    // chunk sits square and the print starts on a clean vertical. extend_skeleton_tips follows
    // the cross-section centroid, which is the right thing in free space but drifts sideways
    // against a face you want to start flat on — run first, it walked the trunk tip of a
    // trident down AND 7 mm sideways, leaving a kinked, diagonal start where there had been a
    // straight one.
    //
    // Ordered this way each stage owns the ends it is good at: the bed snap grounds feet
    // straight, and extension reaches the tips that have no bed to snap to (a trident's prongs
    // stop 13-24 mm short of the top, where nothing else can help them). A tip already sitting
    // on the bed fails on its first step here and is left alone.
    //
    // Still after splicing either way, so tips the bore corridor is about to delete are never
    // extended first.
    {
        const int n_ext = extend_skeleton_tips(r.graph, V, F);
        if (n_ext > 0)
            lap("extend tips", 94);
    }

    // Normalise node spacing again: the traced centreline comes in at the march step
    // (sub-millimetre), and cut planes land at edge midpoints, so segment height is edge
    // length. Arc-length resampling preserves the curve, unlike the smoothing above.
    if (p.resample_edge_len > 0.f)
        r.graph = resample_skeleton_uniform(r.graph, (double) p.resample_edge_len);

    r.dag = orient_skeleton_as_dag(r.graph, find_skeleton_roots(r.graph));
    if (p.collapse_to_spine) {
        r.graph = collapse_skeleton_to_spine(r.graph, p.smooth_iterations, p.smooth_alpha, &V, &F_mat);
        r.dag   = orient_skeleton_as_dag(r.graph, find_skeleton_roots(r.graph));
    }
    if (p.resample_edge_len > 0.f) {
        r.graph = resample_skeleton_graph(r.graph, (double) p.resample_edge_len);
        r.dag   = orient_skeleton_as_dag(r.graph, find_skeleton_roots(r.graph));
    }

    r.cycles_final      = skeleton_cycle_count(r.graph);
    r.roots             = find_skeleton_roots(r.graph);
    r.convergence_nodes = p.correct_convergence ? find_convergence_nodes(r.dag) : std::vector<int>{};
    r.tangents          = compute_skeleton_tangents(r.graph, r.dag, r.roots, r.convergence_nodes,
                                                    (double) p.tangent_W);
    r.cut_edges         = build_skeleton_cut_edges(r.dag);

    // Back to the object's local frame. Node positions take the full inverse; tangents take the
    // inverse of the linear part only, because the consumer multiplies them by world.linear()
    // again (perform_skeleton_cut: `world.linear() * m_skel_tangents.get(...)`), so storing a
    // world-space tangent here would apply the rotation twice.
    if (!world_is_identity) {
        const Transform3d     inv     = world.inverse();
        const Eigen::Matrix3d inv_lin = world.linear().inverse();
        for (auto& n : r.graph.nodes)
            n = inv * n;
        for (auto& n : r.dag.nodes)
            n = inv * n;
        for (auto& t : r.tangents.smoothed) {
            const Eigen::Vector3d v = inv_lin * t;
            const double          l = v.norm();
            t = (l > 1e-12) ? Eigen::Vector3d(v / l) : Eigen::Vector3d::UnitZ();
        }
        for (auto& t : r.tangents.raw) {
            const Eigen::Vector3d v = inv_lin * t;
            const double          l = v.norm();
            t = (l > 1e-12) ? Eigen::Vector3d(v / l) : Eigen::Vector3d::UnitZ();
        }
        for (auto& sp : r.tunnels)
            for (auto& q : sp.points)
                q = inv * q;
    }
    lap("dag + tangents", 100);
    r.build_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();
    std::cout << "[skeleton timing] TOTAL: " << (int) r.build_ms << " ms\n";
    return r;
}

// ── parameter string ──────────────────────────────────────────────────────────

bool parse_skeleton_params(const std::string& spec, SkeletonParams& out, std::string& error)
{
    auto trim = [](std::string s) {
        const auto b = s.find_first_not_of(" \t");
        const auto e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };

    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ';')) {
        item = trim(item);
        if (item.empty())
            continue;
        const auto eq = item.find('=');
        if (eq == std::string::npos) {
            error = "expected key=value, got '" + item + "'";
            return false;
        }
        // Case-folded so `tangent_W` and `tangent_w` both land: the struct field is spelled
        // with a capital W and the key with a lowercase one, and that mismatch silently
        // failed every run of a sweep that was measuring whether the control did anything.
        std::string k = trim(item.substr(0, eq));
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return (char) std::tolower(c); });
        const std::string v = trim(item.substr(eq + 1));
        try {
            if      (k == "method")             out.method             = std::stoi(v);
            else if (k == "quality_speed")      out.quality_speed      = std::stof(v);
            else if (k == "min_edge_factor")    out.min_edge_factor    = std::stof(v);
            else if (k == "medial_centering")   out.medial_centering   = std::stof(v);
            else if (k == "voxel_resolution")   out.voxel_resolution   = std::stoi(v);
            else if (k == "resample")           out.resample_edge_len  = std::stof(v);
            else if (k == "smooth_iterations")  out.smooth_iterations  = std::stoi(v);
            else if (k == "smooth_alpha")       out.smooth_alpha       = std::stof(v);
            else if (k == "collapse_junctions") out.collapse_junctions = (std::stoi(v) != 0);
            else if (k == "collapse_to_spine")  out.collapse_to_spine  = (std::stoi(v) != 0);
            else if (k == "prune_leaves")       out.prune_leaves       = std::stof(v);
            else if (k == "decimate")         out.decimate_faces     = std::stoi(v);
            else if (k == "test_rot") {
                float a = 0.f, b = 0.f, cc = 0.f;
                if (std::sscanf(v.c_str(), "%f,%f,%f", &a, &b, &cc) != 3) {
                    error = "test_rot needs three comma-separated degrees, got '" + v + "'";
                    return false;
                }
                out.test_rot = Vec3f(a, b, cc);
            }
            else if (k == "tangent_w")          out.tangent_W          = std::stof(v);
            else if (k == "correct_convergence")out.correct_convergence= (std::stoi(v) != 0);
            else if (k == "tunnel_min_aspect")  out.tunnel_min_aspect  = std::stof(v);
            else if (k == "tunnel_station")     out.tunnel_station     = std::stof(v);
            else if (k == "bed_snap_band")      out.bed_snap_band      = std::stof(v);
            else if (k == "tunnel_fill") {
                if      (v == "off"  || v == "0") out.tunnel_fill = TunnelFill::Off;
                else if (v == "on"   || v == "2") out.tunnel_fill = TunnelFill::On;
                else if (v == "threshold" || v == "1") out.tunnel_fill = TunnelFill::Threshold;
                else { error = "tunnel_fill must be off|threshold|on"; return false; }
            }
            else { error = "unknown skeleton parameter '" + k + "'"; return false; }
        } catch (const std::exception&) {
            error = "bad value for '" + k + "': '" + v + "'";
            return false;
        }
    }
    return true;
}

// ── dumps ─────────────────────────────────────────────────────────────────────

bool write_skeleton_obj(const SkeletonResult& r, const std::string& path)
{
    std::ofstream f(path);
    if (!f)
        return false;
    f << "# skeleton dump: " << r.graph.num_nodes() << " nodes\n";
    for (const auto& n : r.graph.nodes)
        f << "v " << n.x() << ' ' << n.y() << ' ' << n.z() << '\n';
    for (int u = 0; u < r.graph.num_nodes(); ++u)
        for (int v : r.graph.adj[u])
            if (v > u)
                f << "l " << (u + 1) << ' ' << (v + 1) << '\n';
    return true;
}

static void json_vec(std::ostream& f, const Eigen::Vector3d& v)
{
    f << '[' << v.x() << ',' << v.y() << ',' << v.z() << ']';
}

bool write_skeleton_json(const SkeletonResult& r, const std::string& path, const std::string& source_name)
{
    std::ofstream f(path);
    if (!f)
        return false;
    f.precision(9);

    f << "{\n  \"source\": \"" << source_name << "\",\n";
    f << "  \"genus\": " << r.genus << ",\n";
    f << "  \"cycles_raw\": " << r.cycles_raw << ",\n";
    f << "  \"cycles_final\": " << r.cycles_final << ",\n";
    f << "  \"bed_z\": " << r.bed_z << ",\n";
    f << "  \"snapped_nodes\": " << r.snapped_nodes << ",\n";
    f << "  \"note\": \"" << r.note << "\",\n";

    f << "  \"tunnels\": [\n";
    for (size_t i = 0; i < r.tunnels.size(); ++i) {
        const auto& t = r.tunnels[i];
        f << "    {\"depth\": " << t.depth << ", \"width\": " << t.width
          << ", \"aspect\": " << t.aspect() << ", \"axis\": ";
        json_vec(f, t.axis);
        f << ", \"points\": [";
        for (size_t k = 0; k < t.points.size(); ++k) {
            if (k) f << ',';
            json_vec(f, t.points[k]);
        }
        f << "]}" << (i + 1 < r.tunnels.size() ? "," : "") << '\n';
    }
    f << "  ],\n";

    f << "  \"nodes\": [";
    for (int i = 0; i < r.graph.num_nodes(); ++i) {
        if (i) f << ',';
        json_vec(f, r.graph.nodes[i]);
    }
    f << "],\n";

    f << "  \"edges\": [";
    bool first = true;
    for (int u = 0; u < r.graph.num_nodes(); ++u)
        for (int v : r.graph.adj[u])
            if (v > u) {
                if (!first) f << ',';
                first = false;
                f << '[' << u << ',' << v << ']';
            }
    f << "],\n";

    auto int_list = [&f](const char* key, const std::vector<int>& v, bool last = false) {
        f << "  \"" << key << "\": [";
        for (size_t i = 0; i < v.size(); ++i) { if (i) f << ','; f << v[i]; }
        f << ']' << (last ? "\n" : ",\n");
    };
    int_list("roots", r.roots);
    int_list("convergence_nodes", r.convergence_nodes);

    f << "  \"cut_edges\": [";
    for (size_t i = 0; i < r.cut_edges.size(); ++i) {
        if (i) f << ',';
        f << '[' << r.cut_edges[i].first << ',' << r.cut_edges[i].second << ']';
    }
    f << "],\n";

    // per cut edge: the plane normal that will be used, in the same order as cut_edges
    f << "  \"cut_normals\": [";
    for (size_t i = 0; i < r.cut_edges.size(); ++i) {
        if (i) f << ',';
        json_vec(f, r.tangents.get(r.cut_edges[i].first, r.cut_edges[i].second));
    }
    f << "]\n}\n";
    return true;
}

} // namespace Slic3r

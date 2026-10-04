#pragma once

// UNLAYERED_MOD — the skeleton pipeline as a headless, GUI-free unit.
//
// The stages themselves live in Skeleton.cpp; this is the one place that decides which
// stages run and in what order. It used to be inlined in GLGizmoCut3D::compute_skeleton(),
// which meant the only way to exercise automated multiplanar was to drive the gizmo by
// hand. Both the gizmo and the `--export-skeleton` CLI action now call build_skeleton(),
// so what the CLI dumps is exactly what the GUI draws and cuts.
//
// Deliberately NOT included from Plater.hpp (unlike SkeletonPipeline.hpp) so that adding a
// parameter here does not rebuild the whole GUI.

#include "Skeleton.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

// How hollow geometry (pipes, bores, pockets) is handled. See find_tunnel_spines().
enum class TunnelFill : int {
    Off       = 0, // never trace bores; skeletonize the walls as-is (pre-2026-08 behaviour)
    Threshold = 1, // trace a bore only when it is deep relative to its width (default)
    On        = 2, // trace every bore, however shallow
};

struct SkeletonParams
{
    // ── skeletonization ──
    int   method{0};                 // 0 = MCF, 1 = voxel thinning
    float quality_speed{0.1f};       // MCF W_L
    float min_edge_factor{0.002f};   // MCF min edge length, fraction of bbox diagonal
    // CGAL's omega_P: how hard contraction is biased toward the medial axis. 0 disables the
    // term; CGAL's own default is 0.2.
    //
    // Raised to 0.5 because MCF does NOT keep the skeleton inside the solid on its own — it is
    // a contraction, not a medial-axis computation, and across a concave feature the average of
    // a vertex's neighbours lies in the void. Measured on a solid, watertight, genus-0 trident,
    // as raw MCF output before any cleaning stage:
    //
    //     omega_P   nodes outside   skeleton length outside   MCF time
    //       0            40          22.4 mm (18.2%)            210 ms
    //       0.2          18           8.5 mm  (5.9%)            320 ms
    //       0.5           1           0.6 mm  (0.4%)            391 ms
    //       1.0           0           0.0 mm  (0.0%)            451 ms
    //
    // 1.0 removes the escape entirely but is NOT the default, because pushing this hard
    // destabilises plate-like geometry: the medial axis of a slab is a 2D sheet rather than a
    // curve, so the curve skeleton starts branching inside it. At 1.0 the trident's flat trunk
    // went from 1 root / 0 convergence to 5 roots / 4 convergence, and tee_solid from 2/1 to
    // 3/2 — artefacts that would become real extra cuts. At 0.5 the topology of every corpus
    // mesh is unchanged, which is the trade worth taking by default.
    //
    // Read the table above as RAW MCF only. Through the FULL pipeline the win is much smaller,
    // because Laplacian smoothing puts the skeleton back outside afterwards:
    //
    //     omega_P    branch_up        branch_down
    //       0.2      14.2% outside    9.8% outside
    //       0.5       7.3% outside    8.5% outside
    //       1.0       1.8% outside    1.7% outside
    //
    // So this setting is worth having but is NOT the main lever any more: smoothing is now the
    // dominant source of skeleton lying outside the solid, and making it material-aware is what
    // would actually close this. Do not raise omega_P to chase that number — 1.0 looks best in
    // this table for the wrong reason, and pays for it in the slab branching described above.
    //
    // Cost is about +22% on MCF, the pipeline's dominant stage (intake: 9.4 s -> 11.2 s).
    float medial_centering{0.5f};
    int   voxel_resolution{64};
    // Face budget for skeletonization; 0 = off. When set, a denser mesh is decimated to this
    // before contraction (bore tracing and bed snapping always use the ORIGINAL mesh).
    //
    // DEFAULT OFF, deliberately. MCF is roughly quadratic in face count — on a 42908-face intake
    // it took 126 s at full density, 10.3 s at 8000 faces and 0.8 s at 2000 — but the skeleton
    // is NOT stable under decimation on an organic part: full density gave 51 nodes spanning
    // x72 y56 z60, 8000 gave 53 nodes spanning only z29, and 2000 collapsed the whole graph to
    // ONE node. A fast wrong skeleton is worse than a slow right one, so this is opt-in.
    //
    // (An earlier measurement appeared to show decimation was free. It was reading the node
    // count of the spliced bore centreline, which is traced from the full-density mesh and so
    // cannot respond to this setting at all. Measure with tunnel_fill=off.)
    int   decimate_faces{0};

    // ── graph cleaning ──
    float resample_edge_len{5.0f};
    int   smooth_iterations{10};
    float smooth_alpha{0.5f};
    bool  collapse_junctions{true};
    bool  collapse_to_spine{false};
    float prune_leaves{0.0f};        // 0 = off; else remove leaf edges shorter than this

    // ── tangents ──
    float tangent_W{0.5f};
    bool  correct_convergence{true};

    // ── hollow geometry ──
    TunnelFill tunnel_fill{TunnelFill::Threshold};
    // Bore depth / bore width required by Threshold. Measured on the test corpus: a 60 mm
    // pipe with a 12 mm bore scores 5.0 and a 90 deg elbow about 2.2, while the shapes whose
    // loop must be kept score far lower — windowed plate 0.18, washer 0.27, torus 0.32 — so
    // there is roughly a 4x margin on both sides of this value.
    float tunnel_min_aspect{1.2f};
    float tunnel_station{0.0f};      // march step in mm; 0 = auto from the mesh size

    // ── bed ──
    float bed_snap_band{0.0f};       // 0 = auto (see snap_to_bed)

    // Test-only: synthesises an instance rotation (degrees, XYZ) in the CLI so the
    // rotated-instance path is reachable from the generated corpus. Not a GUI control.
    Vec3f test_rot{Vec3f::Zero()};
};

struct SkeletonResult
{
    SkeletonGraph                   graph;
    SkeletonDAG                     dag;
    SkeletonTangents                tangents;
    std::vector<int>                roots;
    std::vector<int>                convergence_nodes;
    std::vector<std::pair<int,int>> cut_edges;   // ordered (parent, child) cut list
    std::vector<TunnelSpine>        tunnels;

    // diagnostics
    int    genus{-1};
    int    cycles_raw{0};      // independent cycles straight out of skeletonization
    int    cycles_final{0};    // ... and in the graph that gets cut
    double bed_z{0.0};
    double snapped_nodes{0};
    double build_ms{0.0};      // wall-clock for the whole pipeline; per-phase goes to stdout
    std::string note;          // human-readable summary of what the tunnel stage decided
};

// Ordered root-first BFS over the DAG, one queue per root, round-robin interleaved so
// parallel branches zigzag instead of one branch being fully cut before others begin.
std::vector<std::pair<int,int>> build_skeleton_cut_edges(const SkeletonDAG& dag);

// Number of independent cycles: E - V + (connected components).
int skeleton_cycle_count(const SkeletonGraph& G);

// `world` is the instance transform: the mesh is skeletonized in WORLD space so that "up" and
// "the bed" mean what they say, then the resulting nodes and tangents are mapped back into the
// object's local frame, which is the frame perform_skeleton_cut() expects.
//
// This matters because the bed and root heuristics are all expressed along +Z. An instance
// rotated to stand a part up (measured on a real intake: local +Z mapped to a horizontal world
// axis) made "the bottom of the skeleton" a sideways direction — the DAG root landed at the top
// of the part and the bed snap pushed a node sideways. Passing Identity reproduces the previous
// behaviour exactly, which is what the un-rotated test corpus asserts.
// Optional progress/cancellation hooks so the pipeline can be driven from a worker thread.
//
// Cancellation is checked at STAGE BOUNDARIES only. The individual stages are single calls into
// CGAL (mean-curvature-flow contraction in particular) with no interruption points of their own,
// so a cancel takes effect when the current stage ends, not immediately. On a dense part that can
// still be a long wait — which is a reason to prefer a cheaper method, not a reason to pretend
// the cancel is instant.
struct SkeletonProgress
{
    std::function<bool()>                        canceled; // true => abandon at the next boundary
    std::function<void(int, const std::string&)> report;   // percent 0-100, stage name
};

SkeletonResult build_skeleton(const indexed_triangle_set& its,
                              const SkeletonParams&       params,
                              const Transform3d&          world = Transform3d::Identity(),
                              const SkeletonProgress*     progress = nullptr);

// "key=value;key=value" — used by the CLI so the whole parameter set is reachable without
// one config option per knob. Unknown keys are reported and ignored.
bool parse_skeleton_params(const std::string& spec, SkeletonParams& out, std::string& error);

// Wavefront OBJ: nodes as vertices, edges as `l` lines. Opens in any mesh viewer.
bool write_skeleton_obj(const SkeletonResult& r, const std::string& path);

// Machine-checkable dump consumed by tools/skeleton_check.py.
bool write_skeleton_json(const SkeletonResult& r, const std::string& path, const std::string& source_name);

} // namespace Slic3r

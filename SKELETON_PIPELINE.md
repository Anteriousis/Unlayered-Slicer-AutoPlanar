# Skeleton Slicing Pipeline — Theory & Implementation

## Overview

The goal is to decompose a closed 3D mesh into printable segments by cutting along planes derived from a 1D skeleton. Each cut plane is oriented to the local skeleton tangent, so segments are printed along their structural axis rather than the global Z axis. The pipeline has six stages:

1. Skeleton extraction (MCF or voxel thinning)
2. Graph cleaning and refinement
3. **Bore tracing** — replaces the wall skeleton of hollow geometry with the bore axis
4. DAG construction (multi-root Dijkstra)
5. Convergence detection and tangent computation
6. Cut ordering and execution

### Where the code lives

The stages are implemented in `src/libslic3r/Skeleton.cpp`. Which of them run, and in what
order, is decided in exactly one place — `build_skeleton()` in
`src/libslic3r/SkeletonBuild.cpp`. Both the Skeleton Cut gizmo and the `--export-skeleton`
CLI action call it, so a headless dump is what the GUI would draw and cut. Before that
extraction the order was inlined in `GLGizmoCut3D::compute_skeleton()` and the only way to
exercise any of this was to drive the gizmo by hand.

---

## 1. Skeleton Extraction — Mean Curvature Flow

### Theory

Given a closed triangulated surface $\mathcal{M} = (V, F)$, the skeleton is extracted via **mean curvature flow contraction** (Tagliasacchi et al., 2012; implemented in CGAL).

The mesh is iteratively shrunk by solving:

$$\left( W_L L + W_H H \right) \mathbf{p}^{t+1} = W_H H \mathbf{p}^t$$

where:
- $L$ is the cotangent Laplacian of the current mesh
- $H$ is a diagonal attraction matrix (pulls vertices toward their original positions)
- $W_L$ is the Laplacian weight (controls contraction speed — **quality/speed tradeoff**)
- $W_H$ is the positional weight (controls fidelity)
- $\mathbf{p}^t$ is the matrix of vertex positions at iteration $t$

As $t \to \infty$ the mesh degenerates to a 1D curve-skeleton. After contraction, edges shorter than:

$$\ell_{\min} = \alpha \cdot \text{diag}(\text{bbox}(\mathcal{M}))$$

are collapsed, where $\alpha$ is the **min edge length factor** parameter.

### Implementation

```
skeletonize_mesh(V, F, quality_speed, min_edge_factor)
→ SkeletonGraph G = (V_G, E_G)
```

`G` is an undirected graph of 3D points. Parallel edges and self-loops are collapsed by CGAL before export.

---

## 2. Graph Cleaning

### 2a. Bed Snapping — `snap_to_bed`

**Problem:** MCF contraction places leaf nodes (degree-1 tips) at the centroid of their associated mesh vertices, which is typically above the actual bottom surface. If no tip sits on the bed, the DAG has no root there and the base segment is never oriented against the bed.

**Why a raycast cannot do this.** The original implementation cast a ray straight down from each tip and required a hit within a hardcoded 2 mm of $z_{\min}(\mathcal{M})$. For hollow geometry that fails by construction: the axis of a pipe sits *in the bore*, so a downward ray hits no material at all and nothing is ever snapped.

**Correction.** The target is the **bed plane** $z = z_{\min}(\mathcal{M})$, and validity is decided by containment in the object's *footprint* — the outer contour of the cross section taken just above the bed — not by a material hit. A tip is a candidate only when all three hold:

1. it lies within a band of the bed: `bed_snap_band`, defaulting to $\max(5\,\text{mm},\, 0.15 \cdot \Delta z)$;
2. its own branch runs downward, $\hat{d} \cdot (-\hat{n}_{\text{bed}}) > 0.5$, where $\hat d$ points from the tip's neighbour to the tip;
3. the target point lies inside a depth-0 loop of the bed cross section.

The target is found by extending the tip along its own branch direction $\hat d$ to the bed plane, falling back to straight down, then to the centroid of the nearest footprint loop.

Condition (2) is the one that earns its keep: a part **lying on its side** — a pipe on the bed, say — has both tips at mid height with horizontal branches, and dragging either to the bed would invent a spurious vertical branch and a bogus root. Condition (1) bounds how far a tip may travel; the previous behaviour would pull a tip at mid height the entire way down whenever a ray happened to reach the bottom face.

If no tip qualifies, the lowest downward-running tip is *extended* with a new node on the bed so the DAG still gets a root there.

### 2b. Uniform Resampling — `resample_skeleton_uniform`

Traces each branch (chain of degree-2 interior nodes between two anchor nodes) and replaces it with $n_{\text{seg}} = \lceil \ell_{\text{branch}} / \delta \rceil$ evenly spaced nodes. Sampling is by **arc length along the chain**, not linear interpolation between the branch endpoints — the node at arc distance $d$ is found by locating the containing chain segment and interpolating within it, so a curved branch keeps its shape. (An earlier revision of this document gave the formula as $\mathbf{p}_i = \mathbf{p}_{\text{start}} + \frac{i}{n_{\text{seg}}}(\mathbf{p}_{\text{end}} - \mathbf{p}_{\text{start}})$, which would flatten every bend to its chord. The code has never done that.)

Anchor positions (tips and junctions) are preserved exactly. Because cut planes land at edge midpoints, $\delta$ is also the segment height: it is the de facto "how tall is a chunk" control.

### 2c. Laplacian Smoothing — `smooth_skeleton_graph`

Each interior node (non-tip) is updated:

$$\mathbf{p}_v^{t+1} = (1 - \alpha)\, \mathbf{p}_v^t + \frac{\alpha}{\deg(v)} \sum_{u \in \mathcal{N}(v)} \mathbf{p}_u^t$$

Tip nodes ($\deg = 1$) are fixed throughout. Iterated for $k$ steps.

**Cyclic components use Taubin instead.** The pinned tips are what bound the shrinkage inherent in
Laplacian smoothing, so a component containing a *cycle* has nothing holding the cycle out and it
collapses toward its own centroid without limit. Measured on the torus corpus mesh (ideal loop
radius 25, tube radius 6): $r = 22.9$ at $k=0$, $20.6$ at $k=10$ (the default), $15.9$ at $k=40$ — by
then the skeleton has left the material. A washer or a windowed plate behaves the same way.

For any component whose edge count reaches its node count, the step alternates between $\lambda$ and
a slightly larger negative $\mu$:

$$\frac{1}{\lambda} + \frac{1}{\mu} = 0.1, \qquad \lambda = \min(\alpha,\ 0.45)$$

which passes low frequencies at unit gain and damps high ones, so noise is removed without net
shrinkage — 22.93 / 22.96 / 23.07 at $k = 0/10/40$. The cap on $\lambda$ is for stability: the
amplification $(1-\lambda k)(1-\mu k)$ must stay within $\pm 1$ for graph-Laplacian eigenvalues
$k \le 2$, which fails as $\lambda \to 1$.

The gate is "the component has a cycle", not "the component has no tips": the torus skeleton is a
loop with one stray MCF twig hanging off it, and that twig is enough to make a tip-based test miss
the very case it exists for. Acyclic components keep the plain Laplacian, so no tree skeleton
changes behaviour.

### 2d. Junction Collapse — `collapse_nearby_junctions`

Directly adjacent junction nodes ($\deg \geq 3$) are grouped by Union-Find into clusters. Each cluster is replaced by a single representative node at the centroid:

$$\mathbf{p}_{\text{rep}} = \frac{1}{|C|} \sum_{v \in C} \mathbf{p}_v$$

Edges to eliminated nodes are redirected to the representative; duplicate edges are suppressed.

### 2e. Short Leaf Pruning — `prune_short_leaves`

Iteratively removes degree-1 nodes whose single edge is shorter than a threshold $\ell_{\min}$:

$$\text{remove } v \iff \deg(v) = 1 \text{ and } \|\mathbf{p}_v - \mathbf{p}_{\mathcal{N}(v)}\| < \ell_{\min}$$

Repeated until no more candidates remain (cascade: removing one leaf can expose its neighbor as a new short leaf). Used to eliminate spurious stray branches produced by skeletonization noise before DAG construction.

---

## 2.5 Bore Tracing — hollow and tubular geometry

### The problem

Mean curvature flow contracts the **material**. An open tube is topologically a solid torus, so its curve skeleton is a closed loop around the middle of the wall at mid-height, and the axial structure is gone — not approximated badly, *absent*. No amount of graph cleaning recovers it, because the information was never in the skeleton. A blind bore (a cup, a drilled boss) is just as bad: genus 0, but MCF still runs the skeleton up the wall rather than up the axis.

Sealed cavities, by contrast, need no work: `keep_largest_connected_components(mesh, 1)` in `build_prepared_mesh()` drops the inner shell, so a hollow closed body is skeletonised as the solid it encloses. **The gap is bores that are open to the outside**, and the axis has to be measured from the geometry rather than derived from the skeleton.

### Detection — cross-section hole persistence

For each candidate direction $\hat d$ — global $+\hat z$ plus the three PCA axes of the vertices, deduplicated at $|\hat d_i \cdot \hat d_j| > 0.98$ — the mesh is sliced at stations of `tunnel_station` mm and each cross section is decomposed into loops with a nesting depth (0 = outer boundary, 1 = hole). Stations whose section contains a depth-1 loop are marked, and the longest contiguous run is a bore candidate:

$$\text{depth} = (\text{run length}) \cdot \text{step}, \qquad \text{width} = \operatorname{median}_{k \in \text{run}} 2\sqrt{A_{\text{hole},k} / \pi}$$

Detection is by cross section and **not** by genus, which is what makes blind pockets work.

### The aspect gate — bore or feature?

A candidate is accepted when

$$\frac{\text{depth}}{\text{width}} \ge \texttt{tunnel\_min\_aspect}$$

This is the whole pipe-versus-picture-frame distinction, and it separates cleanly:

| shape | depth / width | verdict |
|---|---|---|
| 60 mm pipe, ⌀12 bore | 5.0 | bore — follow the axis |
| 60 mm pipe, ⌀18 bore (thin wall) | 3.3 | bore |
| 40×40×60 block, ⌀15 bore | 4.0 | bore |
| 90° elbow, ⌀12 bore | 2.75 | bore |
| torus R25 r6 | 0.32 | feature — keep the loop |
| ⌀50/⌀30 washer, 8 thick | 0.27 | feature |
| 60×60×6 plate, 30 mm window | 0.18 | feature |

The default of 1.2 sits with roughly a 2× margin below the cheapest bore and a 3.5× margin above the most bore-like feature. `TunnelFill::On` skips the gate entirely (every hole is a bore); `TunnelFill::Off` skips the whole stage and restores pre-2026-08 behaviour.

### Tracing — chord-normal centreline march

From the seed (the hole centroid at the run midpoint) the centreline is walked in both directions. Each step places the next plane by a short fixed-point iteration:

$$c_0 = p + h\,\hat d, \qquad \hat n_i = \widehat{c_i - p}, \qquad c_{i+1} = \text{centroid}\big(\Pi(c_i, \hat n_i) \cap \mathcal{M}\big)_{\text{outer}}$$

then $p \leftarrow c$, $\hat d \leftarrow \widehat{c - p}$. Taking the plane normal from the **chord** rather than from a fixed axis is what lets the trace follow a bend: on an arc of radius $R$ the chord lags the tangent by only $h/2R$, while the accumulated points follow the arc exactly.

Note that the trace continues past the end of the bore as long as there is still material — which is exactly what a flanged pipe needs: the axis carries on down through the solid flange to the bed.

#### Four controls keep the march on the centreline

A real intake manifold breaks a naive version of the above in four distinct ways, each of which
ended the trace early and each of which needs its own control. Measured on `NAIntake1of2.stl`.

1. **The anticipation is a turn rate, not a per-step angle.** The prediction $c_0 = p + h \hat d$
   assumes local straightness, so it is pre-rotated by the turn observed over the previous
   advance. That correction is applied over whatever step the *next* attempt uses, and a failed
   step halves the step — so carrying an angle across unscaled over-rotates by exactly the factor
   the step shrank, and refining makes the prediction worse. Six halvings from 9.45 mm to 0.15 mm
   all failed at the same corner. Storing rad/mm and multiplying by the current step fixes it.
2. **The step shrinks through a bend.** No pre-rotation tracks an arc turning 27°/step, which is
   what this part does where its runner meets the plenum. $h$ is capped at
   $\texttt{kTurnBudget} / \dot\theta$ with a 12° budget. Away from a bend $\dot\theta \approx 0$,
   the cap is inactive, and the march runs at the coarse step exactly as before. The step budget
   is therefore sized from the march's **floor**, not from $h$, or a curvature-limited march
   exhausts it part way along and reports that as the end of the bore.
3. **Leaving the outer contour is not the end of the road.** Where a bore opens into a wider
   volume the outer contour's centroid lurches sideways within one step, so a prediction made
   from the old heading lands in air while the part continues. The march re-centres onto the
   nearest depth-0 loop instead of stopping, bounded at $1.5h$ — a correction larger than that is
   not a lurch but a jump to another limb, and it must stop. This is worth ~30% of the part.
4. **The fixed-point iteration is damped.** Taking the section centroid outright is a Newton step
   with no control on overshoot: through a widening section it lands past the true centre, the
   next section is cut at a skew, and it rings — a 14 mm spike with a 119° turn in the middle of
   an otherwise exact spine. Half steps converge to the same fixed point without ringing (worst
   turn over the whole spine: 15.5°), paid for by raising the iteration budget from 3 to 6.
5. **The heading carries inertia.** The direction update is blended $0.7\,\hat d + 0.3\,\hat c$,
   not half and half. A section centroid is an unbiased estimate of the local axis only while the
   plane is *transverse* to it; once the heading tilts the centroid biases toward whatever the
   skewed plane over-samples, and at 0.5 that bias compounds — the intake march left the axis at
   $z = 43$ and drove into the bed 11 mm below it, taking the last 30 mm of the part with it.
   0.3 costs nothing in bend-following: the 90° elbow still tracks its ideal arc to 0.01 mm.

Marching stops when the section is empty, when a re-centre would have to jump further than $1.5h$,
when one accepted advance turns more than 100° (a safety net for a jump to another feature, set
well above any turn a real centreline makes — measuring it at 60° cost more coverage than the
spikes it caught), when forward progress stalls, or at the step cap.

### Splicing — `splice_tunnel_spines`

Each traced point carries the outer contour's reach as a **corridor radius**. Every existing skeleton node within the corridor is dropped (for a plain pipe, that is the entire mid-wall loop), the traced centreline is inserted as a node chain, and any edge left with one endpoint inside the corridor is reattached to the nearest spine node, so side branches survive.

**The corridor is capped longitudinally as well as laterally.** Distance to a polyline degenerates
to distance to an endpoint once the query is past the end, so a lateral test on its own makes the
corridor a capsule with a hemispherical cap of radius $r$ — and $r$ is the outer contour's full
reach, 49–55 mm on a real intake. Where a trace stopped short, that cap deleted the wall skeleton
covering exactly the geometry the trace had failed to reach, so the region ended with *no*
skeleton rather than falling back to the one it already had. `point_polyline_dist` now also
reports overhang past a terminal vertex, and a node is swallowed only within one node spacing of
the spine's ends.

**Ordering matters here.** Splicing runs *after* resampling, smoothing and junction collapse — those stages exist to tame MCF noise, and Laplacian smoothing pulls a curve toward its chord, so smoothing a freshly measured arc would only flatten it. A second `resample_skeleton_uniform` runs afterwards to bring the sub-millimetre march spacing up to the segment height.

---

## 3. DAG Construction — Multi-root Dijkstra

### Theory

The undirected skeleton $G = (V_G, E_G)$ is oriented into a directed acyclic graph $D = (V_G, A)$ using multi-source Dijkstra from a set of physical root nodes $R \subset V_G$.

**Root selection:** $R$ is the set of degree-1 nodes whose $z$-coordinate lies in the bottom $p\%$ of the skeleton's $z$-range:

$$z_{\text{thresh}} = z_{\min}(G) + p \cdot (z_{\max}(G) - z_{\min}(G)), \quad p = 0.10$$

$$R = \{ v \in V_G : \deg(v) = 1,\; z(v) \leq z_{\text{thresh}} \}$$

**Multi-source Dijkstra:** Initialize $d(r) = 0$ for all $r \in R$, $d(v) = \infty$ otherwise. Relax edges by physical 3D length:

$$d(v) \leftarrow \min_{u \in \mathcal{N}(v)} \left( d(u) + \|\mathbf{p}_v - \mathbf{p}_u\| \right)$$

**Edge orientation:** For each undirected edge $\{u, v\} \in E_G$:

$$\{u, v\} \rightarrow (u \to v) \iff d(u) < d(v), \quad \text{tie-break: } u < v$$

This produces a DAG where every edge points away from the nearest root in physical distance. Multi-parent nodes (DAG in-degree $\geq 2$) arise where multiple root paths converge.

### Structures

| Symbol | Implementation | Meaning |
|--------|----------------|---------|
| $R$ | `dag.pred[i].empty()` | Root nodes (no DAG predecessors) |
| $d(v)$ | `dag.dist[v]` | Dijkstra distance from nearest root |
| $\text{succ}(v)$ | `dag.succ[v]` | DAG successors of $v$ |
| $\text{pred}(v)$ | `dag.pred[v]` | DAG predecessors of $v$ |

### Topological property

Every edge $(u, v) \in A$ satisfies $d(u) < d(v)$. Therefore $D$ is acyclic and admits a topological sort. Nodes with $|\text{pred}(v)| \geq 2$ are exactly the points where distinct path families from different roots merge — these are **convergence nodes**.

---

## 4. Convergence Detection

### Definition

A **convergence node** is any node $c \in V_G$ with DAG in-degree $\geq 2$:

$$\mathcal{C} = \{ c \in V_G : |\text{pred}(c)| \geq 2 \}$$

This is exact and topology-derived: $c \in \mathcal{C}$ iff two or more distinct branches from different roots both lead into $c$ in the Dijkstra DAG.

### Implementation

```cpp
find_convergence_nodes(dag)
  → { i : dag.pred[i].size() >= 2 }
```

This replaces an earlier wavefront BFS approach. The DAG already encodes multi-path convergence; reading `pred` directly is both simpler and correct by construction.

---

## 5. Skeleton Tangents

### Purpose

Each DAG edge $(u, v)$ requires a **cut plane normal** — a unit vector describing the local print direction at that edge. A naïve edge tangent works for simple chains but breaks at convergence nodes where branches merge at an angle.

### 5a. Raw Tangent

For edge $(u, v)$:

$$\hat{t}_{\text{raw}}(u, v) = \frac{\mathbf{p}_v - \mathbf{p}_u}{\|\mathbf{p}_v - \mathbf{p}_u\|}$$

### 5b. Convergence Branch Collection

A **convergence branch** is a directed path $[v_0, v_1, \ldots, v_k]$ where either $v_0$ or $v_k$ (or both) are convergence nodes and the interior nodes are not. The exit filter (using DAG distance) prevents including edges whose orientation contradicts the branch direction.

For branch endpoint $v_0 \in \mathcal{C}$ (a_conv) or $v_k \in \mathcal{C}$ (b_conv), the branch contributes to the bisector at that node.

### 5c. Bisector Computation

For each convergence node $c$, collect the **incoming direction** from each branch that terminates at $c$. For a branch with path $[v_0, \ldots, v_k = c]$, the incoming direction is computed from the penultimate step:

$$\mathbf{d}_i = \frac{\mathbf{p}_c - \mathbf{p}_{v_{k-1}}}{\|\mathbf{p}_c - \mathbf{p}_{v_{k-1}}\|}$$

The bisector is the normalized sum of all incoming directions:

$$\mathbf{b}_c = \frac{\sum_i \mathbf{d}_i}{\left\|\sum_i \mathbf{d}_i\right\|}$$

If $\mathbf{b}_c \cdot \hat{z} < 0$, flip: $\mathbf{b}_c \leftarrow -\mathbf{b}_c$ (ensures the cut plane faces upward toward the print direction).

### 5d. Tangent Smoothing with Hard Pins

For each convergence branch, apply W-blend smoothing along the interior edges, with the convergence-adjacent edge **hard-pinned** to the bisector:

**Hard-pin:** Edge immediately adjacent to $c$ is set to $\mathbf{b}_c$ unconditionally.

**W-blend** for interior edge $i$ (0 = tip end, $k$ = convergence end, normalized position $s_i = i/k$):

$$\hat{t}_{\text{smooth}}(i) = \text{normalize}\left( (1 - w_i)\,\hat{t}_{\text{raw}}(i) + w_i\,\mathbf{b}_c \right)$$

$$w_i = W \cdot s_i^2$$

where $W \in [0,1]$ is the **tangent blend weight** parameter.

**Root enforcement:** Edges adjacent to root nodes $r \in R$ that are not also adjacent to a convergence node are set to $\hat{z} = (0,0,1)$ (horizontal cut at the base).

### 5e. Cut Plane Construction

For edge $(u, v)$ with smoothed tangent $\hat{t}$, the cut plane is:

$$\Pi: \quad \mathbf{n} = \mathbf{W} \cdot \hat{t}, \quad \mathbf{c} = \tfrac{1}{2}(\mathbf{W}\mathbf{p}_u + \mathbf{W}\mathbf{p}_v)$$

where $\mathbf{W}$ is the object-to-world transform. The rotation matrix aligns $\hat{z}$ with $\mathbf{n}$:

$$R = \text{FromTwoVectors}(\hat{z}, \mathbf{n})$$

---

## 6. Cut Ordering — BFS Zigzag

### Theory

Cuts must be ordered so each cut operates on a known, intact mesh piece. The natural order is **root-first**: process edges closer to the bed before edges further away, ensuring the parent piece exists when we cut it.

### Algorithm

Seed one BFS queue per DAG root (every $r \in R$, equivalently every $v$ with $\text{pred}(v) = \emptyset$). Round-robin across queues, each round popping the front node $u$, emitting edges $(u, v)$ for each unvisited $v \in \text{succ}(u)$, and pushing $v$:

```
for each root r: Q_r ← {r}, visited[r] ← true
repeat until all queues empty:
    for each queue Q_r:
        u ← Q_r.dequeue()
        for v ∈ succ(u):
            if not visited[v]:
                emit edge (u, v)
                visited[v] ← true
                Q_r.enqueue(v)
```

The round-robin interleaves parallel branches (zigzag), preventing one long branch from being fully cut before others begin.

### Arm Subtree Assignment

After cutting edge $(u, v)$, the mesh splits into a **trunk** piece (containing $u$) and an **arm** piece (containing $v$). The arm subtree — the set of skeleton nodes that now belong to the arm object — is found by BFS from $v$ via $\text{succ}$ (DAG successors only):

$$\text{arm}(v) = \{ v \} \cup \bigcup_{w \in \text{succ}(v)} \text{arm}(w)$$

Using `dag.succ` (not undirected adjacency) prevents the BFS from crossing through convergence nodes into sibling branches, which would corrupt the node-to-object mapping for all subsequent cuts.

Each skeleton node $w$ is mapped to an ObjectID `node_to_obj[w]`. After the cut, nodes in $\text{arm}(v)$ are reassigned to the new arm object; all others with the current active ObjectID are reassigned to the trunk object. Subsequent cuts look up the active object by `node_to_obj[par]`.

---

## Pipeline Summary

```
mesh (V, F)
    │
    ▼  build_prepared_mesh    repair, outward-orient, keep largest shell (drops sealed voids)
    ▼  MCF contraction (quality/speed tradeoff α, min edge factor β)  or voxel thinning
SkeletonGraph G  (raw, dense, noisy)
    │
    ├─ resample_uniform     even spacing δ along each branch (arc length)
    ├─ smooth_skeleton      Laplacian, k iterations, weight α       ─┐ MCF noise only:
    ├─ collapse_junctions   merge adjacent junction clusters         │ must run BEFORE
    └─ prune_short_leaves   iteratively remove leaves with edge < δ ─┘ bore splicing
    │
    ▼  find_tunnel_spines   hole persistence per direction → aspect gate → chord-normal march
    ▼  splice_tunnel_spines drop wall nodes inside the corridor, insert the axis, reattach branches
    ▼  snap_to_bed          downward-running tips near the bed → bed plane, footprint-checked
    ▼  resample_uniform     march spacing (sub-mm) → segment height δ
SkeletonGraph G  (clean)
    │
    ▼  multi-source Dijkstra from R (bottom 10% Z leaves)
SkeletonDAG D    (oriented, dist labels)
    │
    ├─ C = { v : |pred(v)| ≥ 2 }        convergence nodes
    └─ bisectors b_c for each c ∈ C     normalized sum of incoming directions
    │
    ▼  compute_skeleton_tangents (W-blend + hard-pins + root enforcement)
SkeletonTangents  (one smoothed unit vector per directed edge)
    │
    ▼  build_cut_edges (root-first BFS zigzag via dag.succ)
ordered edge list (par, child)
    │
    ▼  perform_skeleton_cut (cut plane Π per edge, arm BFS via dag.succ)
N mesh segments, each with a local print axis
```

---

## Parameter Reference

| Parameter | Symbol | Default | Effect |
|-----------|--------|---------|--------|
| Quality/speed | $\alpha$ (MCF $W_L$) | 0.1 | Higher = faster contraction, coarser skeleton |
| Min edge factor | $\beta$ | 0.002 | Minimum edge length as fraction of bbox diagonal |
| Resample interval | $\delta$ | 5.0 | Uniform branch node spacing (mm); cuts land at edge midpoints, so this is also the segment height |
| Smooth iterations | $k$ | 10 | Laplacian smoothing passes |
| Smooth alpha | $\alpha_s$ | 0.5 | Laplacian step size $\in (0,1)$ |
| Tangent blend W | $W$ | 0.5 | Convergence blend strength; 1.0 = full bisector at midpoint |
| Root bottom pct | $p$ | 0.10 | Z-fraction for root leaf detection |
| `tunnel_fill` | — | `Threshold` | `Off` / `Threshold` / `On` — see §2.5 |
| `tunnel_min_aspect` | — | 1.2 | Bore depth / width required by `Threshold` |
| `tunnel_station` | — | auto | Slice + march step, mm. 0 → bbox diagonal / 120 |
| `bed_snap_band` | — | auto | Bed-snap band, mm. 0 → max(5, 0.15·Δz) |
| `prune_leaves` | — | 0 (off) | Remove leaf edges shorter than this, mm |

---

## 8. Headless harness

`--export-skeleton <dir>` runs `build_skeleton()` on every loaded object and writes, per object,
`<name>_skeleton.obj` (nodes as vertices, edges as `l` lines — opens in any mesh viewer) and
`<name>_skeleton.json` (nodes, edges, roots, convergence nodes, ordered cut edges, the cut plane
normal per cut, traced bores, genus, and the cycle count before and after splicing).
`--skeleton-params "key=value;..."` overrides any field of `SkeletonParams`.

Use absolute forward-slash paths. Git Bash mangles `\\` inside a quoted Windows path, and the
checker sets the subprocess cwd to the dump directory, so a relative `--exe` does not resolve.
The dump filename comes from `ModelObject::name`, which for an STL loaded by absolute path is the
*whole path* — hence the `path(...).stem()` in the CLI handler; without it the write fails silently
while the pipeline logs a perfectly clean run.

```sh
python tools/skeleton_test_meshes.py --out sandboxes/skeleton_tests   # 15 watertight meshes
python tools/skeleton_check.py \
  --exe C:/Unlayered-Slicer/build/src/RelWithDebInfo/unlayered-slicer.exe \
  --dir sandboxes/skeleton_tests                                      # pass/fail matrix
```

`tools/skeleton_test_meshes.py` builds the corpus parametrically (solids of revolution, a swept
elbow, polygon prisms) and validates every mesh — half-edge pairing, positive volume, expected
genus — before writing it, so a corpus failure can never be mistaken for a pipeline failure.
`tools/skeleton_check.py` asserts the properties that matter per shape and prints the measured
value for each check, so a failure says what the geometry actually did.

Redirect stdout: only a WIN32 no-console shim is built, so `[tunnel]`, `[snap_to_bed]` and
`[mcf]` diagnostics are invisible unless you pipe to a file (the checker does this per mesh).

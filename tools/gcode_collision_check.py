#!/usr/bin/env python3
"""Post-slice hotend collision check for multiplanar G-code (standalone cross-check).

The slicer runs the same model itself after export (libslic3r/GCode/HotendCollisionCheck.cpp, printer
settings nonplanar_hotend_cone_height/_angle); this script re-derives the result from an exported
file with numpy only, for verifying that implementation or checking foreign G-code.

The hotend is modelled as a cone with its apex at the nozzle tip, its axis along the tool axis and
a 90-degree opening (45-degree half-angle), `--cone-height` mm tall; optionally a cylinder of
`--body-radius` above it up to `--body-height` (heater block / shroud). Material deposited in an
EARLIER episode (a maximal run of moves sharing one tool axis -- one chunk or cut-plane segment
under By object / By group sequencing) that lies inside that cone while a later episode is being
printed is a collision.

Frame: RepRapFirmware compensates the rotary axes, so the emitted X/Y/Z are the nozzle tip in the
BED frame and deposited material stays where it was laid; the tool axis per episode comes from the
`; TOOL_AXIS:x,y,z` tag the slicer writes at every orientation change (falls back to B/C words via
tool_axis = (cos b cos c, cos b sin c, -sin b), the fork's convention, B=-90 being level).

Method (numpy only): per episode, rotate tips and earlier material into the tool frame (height a
along the axis, lateral u/v). Tips -- sampled along every G0/G1, extruding or not -- are binned into
1 mm cells per 0.2 mm height band; each band gets a distance-to-nearest-tip field by iterated
octagonal dilation (within ~8% of Euclidean, slightly conservative). A material voxel (1 mm, sampled
every 0.5 mm along extrusions) at height h above a band collides when its distance to that band's
tips is <= h + margin, for eps < h <= cone height.

    python tools/gcode_collision_check.py plate_1.gcode [--cone-height 40] [--body-radius 0]
        [--body-height 80] [--margin 0.5] [--eps 0.3] [--examples 10]
Exit 1 if any collision.
"""
import argparse
import math
import re
import sys
from collections import defaultdict

import numpy as np

WORD = re.compile(r"\b([XYZEFBC])(-?(?:\d+\.?\d*|\.\d+))")
AXIS_TAG = re.compile(r"TOOL_AXIS:(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)")


def rotation_to_z(n):
    """3x3 rotation R with R @ n == (0,0,1) (Rodrigues; identity when n is already +Z)."""
    n = n / np.linalg.norm(n)
    z = np.array([0.0, 0.0, 1.0])
    v = np.cross(n, z)
    s = np.linalg.norm(v)
    c = float(n @ z)
    if s < 1e-12:
        return np.eye(3) if c > 0 else np.diag([1.0, -1.0, -1.0])
    k = v / s
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + math.sin(math.acos(c)) * K + (1 - c) * (K @ K)


def parse(path):
    """Return episodes: list of dicts {axis, tips (N,3), tip_lines (N,), mat (M,3), mat_lines (M,)}."""
    pos = np.zeros(3)
    have = [False, False, False]
    relative = False          # G91 (the machine end G-code lifts in relative mode)
    axis = np.array([0.0, 0.0, 1.0])
    episodes = []
    cur = None

    def new_episode(ax):
        nonlocal cur
        cur = {"axis": ax.copy(), "tips": [], "tip_lines": [], "mat": [], "mat_lines": [], "first_line": None}
        episodes.append(cur)

    new_episode(axis)
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln, line in enumerate(f, 1):
            if line.startswith(";"):
                m = AXIS_TAG.search(line)
                if m:
                    ax = np.array([float(m.group(1)), float(m.group(2)), float(m.group(3))])
                    if np.linalg.norm(ax - cur["axis"]) > 1e-6:
                        new_episode(ax)
                continue
            if line.startswith("G90"):
                relative = False
                continue
            if line.startswith("G91"):
                relative = True
                continue
            if not line.startswith(("G0 ", "G1 ")):
                continue
            words = dict(WORD.findall(line.split(";")[0]))
            if relative:
                for i, k in enumerate("XYZ"):
                    if k in words:
                        words[k] = str(pos[i] + float(words[k]))
            # B/C words are not interpreted: the `; TOOL_AXIS` tag (same emitter, same convention) is
            # authoritative for the episode's axis, and moves before a tag belong to the previous axis.
            new = pos.copy()
            for i, k in enumerate("XYZ"):
                if k in words:
                    new[i] = float(words[k])
                    have[i] = True
            if not all(have):
                pos = new
                continue
            extruding = "E" in words and float(words["E"]) > 0 and ("X" in words or "Y" in words)
            d = new - pos
            length = float(np.linalg.norm(d))
            if length > 1e-9:
                # tip samples every 1 mm (both ends), material every 0.5 mm along extrusions
                nt = max(2, int(length) + 1)
                t = np.linspace(0.0, 1.0, nt)
                pts = pos[None, :] + t[:, None] * d[None, :]
                cur["tips"].append(pts)
                cur["tip_lines"].append(np.full(nt, ln))
                if extruding:
                    nm = max(2, int(length / 0.5) + 1)
                    tm = np.linspace(0.0, 1.0, nm)
                    mpts = pos[None, :] + tm[:, None] * d[None, :]
                    cur["mat"].append(mpts)
                    cur["mat_lines"].append(np.full(nm, ln))
                if cur["first_line"] is None:
                    cur["first_line"] = ln
            pos = new
    out = []
    for e in episodes:
        if not e["tips"]:
            continue
        e["tips"] = np.vstack(e["tips"]); e["tip_lines"] = np.concatenate(e["tip_lines"])
        if e["mat"]:
            e["mat"] = np.vstack(e["mat"]); e["mat_lines"] = np.concatenate(e["mat_lines"])
        else:
            e["mat"] = np.zeros((0, 3)); e["mat_lines"] = np.zeros(0, dtype=int)
        out.append(e)
    return out


def distance_field(occ, max_r):
    """Octagonal iterated dilation: D[cell] = steps until reached by an occupied cell (max_r+1 = never)."""
    D = np.full(occ.shape, max_r + 1, dtype=np.int32)
    D[occ] = 0
    cur = occ.copy()
    for r in range(1, max_r + 1):
        nxt = cur.copy()
        # 4-neighbour step
        nxt[1:, :] |= cur[:-1, :]; nxt[:-1, :] |= cur[1:, :]
        nxt[:, 1:] |= cur[:, :-1]; nxt[:, :-1] |= cur[:, 1:]
        if r % 2 == 0:  # alternate with diagonal step -> octagonal metric
            nxt[1:, 1:] |= cur[:-1, :-1]; nxt[:-1, :-1] |= cur[1:, 1:]
            nxt[1:, :-1] |= cur[:-1, 1:]; nxt[:-1, 1:] |= cur[1:, :-1]
        newly = nxt & ~cur
        if not newly.any():
            break
        D[newly] = r
        cur = nxt
    return D


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gcode")
    ap.add_argument("--cone-height", type=float, default=40.0, help="cone height along the tool axis, mm")
    ap.add_argument("--body-radius", type=float, default=0.0, help="cylinder radius above the cone (0 = none), mm")
    ap.add_argument("--body-height", type=float, default=80.0, help="top of the cylinder above the tip, mm")
    ap.add_argument("--margin", type=float, default=0.5, help="lateral tolerance (line width/2 + nozzle), mm")
    ap.add_argument("--eps", type=float, default=0.3, help="ignore material within this height above the tip (the current layer), mm")
    ap.add_argument("--examples", type=int, default=10)
    a = ap.parse_args()

    eps_list = parse(a.gcode)
    print(f"{a.gcode}: {len(eps_list)} tool-axis episode(s)")
    for i, e in enumerate(eps_list):
        print(f"  episode {i}: axis={np.round(e['axis'], 4).tolist()} from line {e['first_line']}: {len(e['tips'])} tip samples, {len(e['mat'])} material samples")

    band = 0.2
    H = a.cone_height
    Hbody = max(a.body_height, H) if a.body_radius > 0 else H
    total = 0
    pair_counts = defaultdict(int)
    worst = 0.0
    examples = []
    for i, e in enumerate(eps_list):
        if i == 0:
            continue
        prev_mat = [p["mat"] for p in eps_list[:i] if len(p["mat"])]
        prev_lines = [p["mat_lines"] for p in eps_list[:i] if len(p["mat"])]
        prev_ids = [np.full(len(p["mat"]), j) for j, p in enumerate(eps_list[:i]) if len(p["mat"])]
        if not prev_mat:
            continue
        M = np.vstack(prev_mat); ML = np.concatenate(prev_lines); MI = np.concatenate(prev_ids)
        R = rotation_to_z(e["axis"])
        T = e["tips"] @ R.T          # tool frame: columns u, v, a
        Mt = M @ R.T
        # 1 mm lateral grid covering both sets
        u0 = math.floor(min(T[:, 0].min(), Mt[:, 0].min())) - 1
        v0 = math.floor(min(T[:, 1].min(), Mt[:, 1].min())) - 1
        nu = math.ceil(max(T[:, 0].max(), Mt[:, 0].max())) - u0 + 2
        nv = math.ceil(max(T[:, 1].max(), Mt[:, 1].max())) - v0 + 2
        tu = np.floor(T[:, 0]).astype(int) - u0; tv = np.floor(T[:, 1]).astype(int) - v0
        ta = np.floor(T[:, 2] / band).astype(int)
        mu = np.floor(Mt[:, 0]).astype(int) - u0; mv = np.floor(Mt[:, 1]).astype(int) - v0
        ma = Mt[:, 2]
        # material voxel dedupe (1 mm, plus height quantised to the band) to keep the loops small
        key = np.stack([mu, mv, np.floor(ma / band).astype(int)], axis=1)
        _, first_idx = np.unique(key, axis=0, return_index=True)
        mu, mv, ma, ML, MI = mu[first_idx], mv[first_idx], ma[first_idx], ML[first_idx], MI[first_idx]
        max_r = int(math.ceil(max(H, a.body_radius if a.body_radius > 0 else 0) + a.margin)) + 1
        bands = np.unique(ta)
        for b in bands:
            sel = ta == b
            occ = np.zeros((nu, nv), dtype=bool)
            occ[tu[sel], tv[sel]] = True
            a_t = (b + 0.5) * band                   # representative tip height of this band
            h = ma - a_t                              # material height above the tips of this band
            cand = (h > a.eps) & (h <= Hbody)
            if not cand.any():
                continue
            D = distance_field(occ, max_r)
            d = D[mu[cand], mv[cand]].astype(float)
            hc = h[cand]
            cone_hit = (hc <= H) & (d <= hc + a.margin)
            body_hit = np.zeros_like(cone_hit)
            if a.body_radius > 0:
                body_hit = (hc > H) & (d <= a.body_radius + a.margin)
            hit = cone_hit | body_hit
            if not hit.any():
                continue
            idx = np.nonzero(cand)[0][hit]
            hc_hit, d_hit = hc[hit], d[hit]      # index space of the hits, not of the candidates
            pen = np.where(hc_hit <= H, hc_hit + a.margin - d_hit, a.body_radius + a.margin - d_hit)
            total += int(hit.sum())
            worst = max(worst, float(pen.max()))
            # example: the deepest hit in this band, with a tip line from the nearest occupied cell
            k = int(np.argmax(pen)); q = idx[k]
            tip_sel = np.nonzero(sel)[0]
            du = tu[tip_sel] - mu[q]; dv = tv[tip_sel] - mv[q]
            nearest = tip_sel[int(np.argmin(du * du + dv * dv))]
            src = int(MI[q])
            pair_counts[(src, i)] += int(hit.sum())
            if len(examples) < a.examples * 4:
                examples.append((float(pen[k]), i, src, int(e["tip_lines"][nearest]), e["tips"][nearest], int(ML[q]), None, float(hc_hit[k]), float(d_hit[k])))
    print()
    if total == 0:
        print(f"NO COLLISIONS: no material from an earlier episode enters the {int(math.degrees(2*math.atan(1.0)))}-degree cone "
              f"(height {H} mm{', body r=' + str(a.body_radius) + ' mm to ' + str(a.body_height) + ' mm' if a.body_radius > 0 else ''}) of a later episode.")
        return 0
    print(f"COLLISIONS: {total} material voxel/band hits, worst penetration {worst:.1f} mm (lateral distance inside the cone surface)")
    for (src, dst), n in sorted(pair_counts.items(), key=lambda kv: -kv[1]):
        print(f"  material of episode {src} (axis {np.round(eps_list[src]['axis'], 3).tolist()}) hit while printing episode {dst} (axis {np.round(eps_list[dst]['axis'], 3).tolist()}): {n} hits")
    examples.sort(key=lambda t: -t[0])
    print(f"  worst examples (tip G-code line -> offending material G-code line):")
    for pen, dst, src, tline, tip, mline, _, hgt, dist in examples[: a.examples]:
        print(f"    tip line {tline} at ({tip[0]:.1f}, {tip[1]:.1f}, {tip[2]:.1f}) [episode {dst}] vs material line {mline} [episode {src}]: "
              f"{hgt:.1f} mm above tip along the axis, {dist:.0f} mm off-axis -> {pen:.1f} mm inside the cone")
    return 1


if __name__ == "__main__":
    sys.exit(main())

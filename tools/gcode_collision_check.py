#!/usr/bin/env python3
"""Post-slice hotend collision check for multiplanar G-code (standalone cross-check).

The slicer runs the same model itself after export (libslic3r/GCode/HotendCollisionCheck.cpp, printer
settings nonplanar_hotend_cone_height and nonplanar_hotend_angle_xpos/_xneg/_ypos/_yneg); this script
re-derives the result from an exported file with numpy only, for verifying that implementation or
checking foreign G-code.

The hotend is modelled as a four-sided pyramid with its apex at the nozzle tip and its axis along the
tool axis, clipped by a sphere of radius `--height` about the tip (a spherical cap, not a flat top,
so a shallow side cannot make it huge). Each side has its own angle, measured up from the bed plane with the
head vertical (`--angles +X,-X,+Y,-Y`, default 45 each = the old 90-degree cone): at a height h a side
stands h / tan(angle) out from the nozzle axis. Optionally a square prism of half-width `--body-radius`
sits above it up to `--body-height` (heater block / shroud). Material deposited in an EARLIER episode
(a maximal run of moves sharing one tool axis -- one chunk or cut-plane segment under By object / By
group sequencing) that lies inside that envelope while a later episode is being printed is a collision.

Frame: RepRapFirmware compensates the rotary axes, so the emitted X/Y/Z are the nozzle tip in the
BED frame and deposited material stays where it was laid; the tool axis per episode comes from the
`; TOOL_AXIS:x,y,z` tag the slicer writes at every orientation change. G2/G3 arcs (I/J or R, XY
plane) are followed as polylines, like GCodeProcessor does - the start G-code's purge arc is one. The pyramid's sides turn with
the head (head_frame(): B leans the head about machine Y, C turns the table, so the head's +Y stays
level and its +X is the side B leans it toward). `--upright` is a virtual 5-axis (3-axis) printer,
whose nozzle never tilts: the pyramid stays vertical with its sides along machine X / Y.

Method (numpy only): per episode, rotate tips and earlier material into the head frame (height a
along the axis, lateral u/v along the head's +X/+Y). Tips -- sampled along every G0/G1, extruding or
not -- are binned into 1 mm cells per 0.2 mm height band, and each band gets a summed-area table of
its tip cells. A material voxel (1 mm, sampled every 0.5 mm along extrusions) at height h above a band
collides when any tip of that band lies in the rectangle the four sides reach at that height (plus
the margin), for eps < h <= height -- one O(1) table lookup per voxel.

    python tools/gcode_collision_check.py plate_1.gcode [--height 40] [--angles 45,45,45,45]
        [--upright] [--body-radius 0] [--body-height 80] [--margin 0.5] [--eps 0.3] [--examples 10]
Exit 1 if any collision.
"""
import argparse
import math
import re
import sys
from collections import defaultdict

import numpy as np

WORD = re.compile(r"\b([XYZEFBCIJR])(-?(?:\d+\.?\d*|\.\d+))")
AXIS_TAG = re.compile(r"TOOL_AXIS:(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)")
SIDES = ("+X", "-X", "+Y", "-Y")


def head_frame(n):
    """Rows: the head's +X, +Y and nozzle axis in the bed frame -- tool_head_frame() in PrintConfig.cpp."""
    n = n / np.linalg.norm(n)
    if math.hypot(n[0], n[1]) < 1e-6:
        return np.eye(3) if n[2] > 0 else np.diag([1.0, -1.0, -1.0])
    c = math.atan2(n[1], n[0])
    y = np.array([-math.sin(c), math.cos(c), 0.0])
    return np.array([np.cross(y, n), y, n])


def arc_polyline(start, end, words, clockwise):
    """G2 (clockwise) / G3 arc from `start` to `end` in the XY plane, centre from I/J (offsets
    from the start) or R (negative R = the long way round), as points about 1 mm apart."""
    sx, sy, ex, ey = start[0], start[1], end[0], end[1]
    if "I" in words or "J" in words:
        cx, cy = sx + float(words.get("I", 0)), sy + float(words.get("J", 0))
    elif "R" in words:
        r = float(words["R"])
        mx, my, dx, dy = (sx + ex) / 2, (sy + ey) / 2, ex - sx, ey - sy
        chord = math.hypot(dx, dy)
        if chord < 1e-9 or abs(r) < chord / 2:
            return [start, end]
        h = math.sqrt(r * r - chord * chord / 4)
        # the centre sits left of the chord for a short counter-clockwise arc
        side = (1 if not clockwise else -1) * (1 if r > 0 else -1)
        cx, cy = mx - side * h * dy / chord, my + side * h * dx / chord
    else:
        return [start, end]
    rad = math.hypot(sx - cx, sy - cy)
    a0, a1 = math.atan2(sy - cy, sx - cx), math.atan2(ey - cy, ex - cx)
    sweep = a1 - a0
    if clockwise and sweep >= 0: sweep -= 2 * math.pi
    if not clockwise and sweep <= 0: sweep += 2 * math.pi
    n = max(2, int(abs(sweep) * rad) + 1)
    pts = []
    for k in range(n + 1):
        t = k / n
        a = a0 + sweep * t
        pts.append(np.array([cx + rad * math.cos(a), cy + rad * math.sin(a), start[2] + (end[2] - start[2]) * t]))
    pts[-1] = end.copy()
    return pts


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
            if not line.startswith(("G0 ", "G1 ", "G2 ", "G3 ")):
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
            # G2/G3 (the start G-code's purge arc, arc fitting) become a polyline in XY with Z
            # ramped along it, as GCodeProcessor does; G0/G1 are a single segment.
            path = arc_polyline(pos, new, words, line.startswith("G2")) if line.startswith(("G2", "G3")) else [pos, new]
            for a0, a1 in zip(path[:-1], path[1:]):
                d = a1 - a0
                length = float(np.linalg.norm(d))
                if length <= 1e-9:
                    continue
                # tip samples every 1 mm (both ends), material every 0.5 mm along extrusions
                nt = max(2, int(length) + 1)
                t = np.linspace(0.0, 1.0, nt)
                pts = a0[None, :] + t[:, None] * d[None, :]
                cur["tips"].append(pts)
                cur["tip_lines"].append(np.full(nt, ln))
                if extruding:
                    nm = max(2, int(length / 0.5) + 1)
                    tm = np.linspace(0.0, 1.0, nm)
                    mpts = a0[None, :] + tm[:, None] * d[None, :]
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


def summed_area(occ):
    """S[u, v] = occupied cells in [0, u) x [0, v)."""
    S = np.zeros((occ.shape[0] + 1, occ.shape[1] + 1), dtype=np.int32)
    S[1:, 1:] = np.cumsum(np.cumsum(occ, axis=0, dtype=np.int32), axis=1, dtype=np.int32)
    return S


def count(S, ul, uh, vl, vh):
    """Occupied cells in the inclusive rectangles [ul, uh] x [vl, vh] (arrays), clipped to the grid."""
    nu, nv = S.shape[0] - 1, S.shape[1] - 1
    ul = np.clip(ul, 0, nu); vl = np.clip(vl, 0, nv)
    uh = np.clip(uh + 1, 0, nu); vh = np.clip(vh + 1, 0, nv)
    empty = (ul >= uh) | (vl >= vh)
    c = S[uh, vh] - S[ul, vh] - S[uh, vl] + S[ul, vl]
    return np.where(empty, 0, c)


def tip_rect(mu, mv, reach):
    """Tip cells that reach material cell (mu, mv): u - tu in [-xneg, xpos], v - tv in [-yneg, ypos]."""
    xp, xn, yp, yn = reach
    return (np.ceil(mu - xp).astype(int), np.floor(mu + xn).astype(int),
            np.ceil(mv - yp).astype(int), np.floor(mv + yn).astype(int))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gcode")
    ap.add_argument("--height", "--cone-height", dest="height", type=float, default=40.0, help="envelope height along the tool axis, mm")
    ap.add_argument("--angles", default="45,45,45,45", help="side angles from the bed plane, +X,-X,+Y,-Y, degrees")
    ap.add_argument("--upright", action="store_true", help="virtual 5-axis: the nozzle never tilts")
    ap.add_argument("--body-radius", type=float, default=0.0, help="half-width of a square prism above the pyramid (0 = none), mm")
    ap.add_argument("--body-height", type=float, default=80.0, help="top of the prism above the tip, mm")
    ap.add_argument("--margin", type=float, default=0.5, help="lateral tolerance (line width/2 + nozzle), mm")
    ap.add_argument("--eps", type=float, default=0.3, help="ignore material within this height above the tip (the current layer), mm")
    ap.add_argument("--examples", type=int, default=10)
    a = ap.parse_args()

    angles = [float(x) for x in a.angles.split(",")]
    if len(angles) != 4:
        ap.error("--angles takes four values: +X,-X,+Y,-Y")
    spread = np.array([math.cos(math.radians(min(max(x, 1.0), 90.0))) / math.sin(math.radians(min(max(x, 1.0), 90.0))) for x in angles])
    spread = np.maximum(spread, 0.0)

    eps_list = parse(a.gcode)
    print(f"{a.gcode}: {len(eps_list)} tool-axis episode(s)")
    for i, e in enumerate(eps_list):
        print(f"  episode {i}: axis={np.round(e['axis'], 4).tolist()} from line {e['first_line']}: {len(e['tips'])} tip samples, {len(e['mat'])} material samples")

    band = 0.2
    H = a.height
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
        R = np.eye(3) if a.upright else head_frame(e["axis"])
        T = e["tips"] @ R.T          # head frame: columns u, v, a
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
        for b in np.unique(ta):
            sel = ta == b
            a_t = (b + 0.5) * band                   # representative tip height of this band
            h = ma - a_t                              # material height above the tips of this band
            cand = (h > a.eps) & (h <= Hbody)
            if not cand.any():
                continue
            occ = np.zeros((nu, nv), dtype=bool)
            occ[tu[sel], tv[sel]] = True
            S = summed_area(occ)
            hc, cu, cv = h[cand], mu[cand], mv[cand]
            # per-side reach at each candidate's height: the pyramid below `height` -- clipped by the
            # sphere of radius `height` about the tip, as the slicer does, so no face reaches past the
            # sphere's half-chord there -- and the prism above
            body = hc > H
            chord = np.sqrt(np.maximum(H * H - hc * hc, 0.0))
            reach = [np.where(body, a.body_radius, np.minimum(hc * spread[s], chord)) + a.margin for s in range(4)]
            hit = count(S, *tip_rect(cu, cv, reach)) > 0
            # the rectangle is the cap's bounding square: confirm against the true distance from a tip
            tip_u, tip_v = tu[sel], tv[sel]
            for k in np.nonzero(hit & ~body)[0]:
                du = cu[k] - tip_u; dv = cv[k] - tip_v
                in_rect = (du <= reach[0][k]) & (-du <= reach[1][k]) & (dv <= reach[2][k]) & (-dv <= reach[3][k])
                lim = chord[k] + a.margin
                if not np.any(in_rect & (du * du + dv * dv <= lim * lim)):
                    hit[k] = False
            if not hit.any():
                continue
            idx = np.nonzero(cand)[0][hit]
            reach_hit = [r[hit] for r in reach]
            hu, hv = cu[hit], cv[hit]
            # depth: the largest uniform inset of all four sides that still leaves a tip inside
            lo = np.zeros(len(idx)); hi = np.max(np.stack(reach_hit), axis=0)
            for _ in range(12):
                mid = 0.5 * (lo + hi)
                ok = count(S, *tip_rect(hu, hv, [r - mid for r in reach_hit])) > 0
                lo = np.where(ok, mid, lo); hi = np.where(ok, hi, mid)
            pen = lo
            total += int(hit.sum())
            worst = max(worst, float(pen.max()))
            # example: the deepest hit in this band, with the tip it lies deepest inside and its side
            k = int(np.argmax(pen)); q = idx[k]
            tip_sel = np.nonzero(sel)[0]
            du = mu[q] - tu[tip_sel]; dv = mv[q] - tv[tip_sel]
            rk = [r[k] for r in reach_hit]
            slack = np.min(np.stack([rk[0] - du, rk[1] + du, rk[2] - dv, rk[3] + dv]), axis=0)
            best = int(np.argmax(slack)); nearest = tip_sel[best]
            fill = [du[best] / rk[0], -du[best] / rk[1], dv[best] / rk[2], -dv[best] / rk[3]]
            side = SIDES[int(np.argmax(fill))]
            src = int(MI[q])
            pair_counts[(src, i)] += int(hit.sum())
            if len(examples) < a.examples * 4:
                examples.append((float(pen[k]), i, src, int(e["tip_lines"][nearest]), e["tips"][nearest], int(ML[q]), side, float(h[q])))
    print()
    shape = (f"pyramid (sides +X {angles[0]:g}, -X {angles[1]:g}, +Y {angles[2]:g}, -Y {angles[3]:g} deg from the bed, "
             f"capped by a {H} mm sphere about the tip{', body half-width ' + str(a.body_radius) + ' mm to ' + str(a.body_height) + ' mm' if a.body_radius > 0 else ''}"
             f"{', upright' if a.upright else ''})")
    if total == 0:
        print(f"NO COLLISIONS: no material from an earlier episode enters the {shape} of a later episode.")
        return 0
    print(f"COLLISIONS in the {shape}: {total} material voxel/band hits, worst penetration {worst:.1f} mm (lateral distance inside the nearest side)")
    for (src, dst), n in sorted(pair_counts.items(), key=lambda kv: -kv[1]):
        print(f"  material of episode {src} (axis {np.round(eps_list[src]['axis'], 3).tolist()}) hit while printing episode {dst} (axis {np.round(eps_list[dst]['axis'], 3).tolist()}): {n} hits")
    examples.sort(key=lambda t: -t[0])
    print(f"  worst examples (tip G-code line -> offending material G-code line):")
    for pen, dst, src, tline, tip, mline, side, hgt in examples[: a.examples]:
        print(f"    tip line {tline} at ({tip[0]:.1f}, {tip[1]:.1f}, {tip[2]:.1f}) [episode {dst}] vs material line {mline} [episode {src}]: "
              f"{hgt:.1f} mm above tip along the axis on the {side} side -> {pen:.1f} mm inside the envelope")
    return 1


if __name__ == "__main__":
    sys.exit(main())

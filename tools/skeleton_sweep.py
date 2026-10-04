#!/usr/bin/env python3
"""Measure what each skeleton generator control actually changes.

Runs --export-skeleton over a grid of single-parameter variations and reports, per parameter,
how much the resulting skeleton moved versus the default. The point is to answer "does this
slider do anything" with a number instead of an opinion, so dead controls can be removed.

Metrics per run:
  nodes     node count
  dpos      mean distance from the default run's node set (nearest-node, mm) - shape change
  dnorm     mean angle between cut plane normals and the default's (deg)    - tangent change
"""
import argparse
import json
import math
import os
import subprocess
import sys

# Each entry: (label, param key, [values to try]). First value is not assumed to be the default.
SWEEPS = [
    ("Quality/speed",     "quality_speed",     ["0.01", "0.1", "1.0"]),
    ("Min edge factor",   "min_edge_factor",   ["0.0002", "0.002", "0.02"]),
    ("Resample edge len", "resample",          ["2", "5", "10"]),
    ("Smooth iters",      "smooth_iterations", ["0", "10", "50"]),
    ("Smooth alpha",      "smooth_alpha",      ["0.1", "0.5", "1.0"]),
    ("Junction collapse", "collapse_junctions", ["0", "1"]),
    ("Collapse to spine", "collapse_to_spine", ["0", "1"]),
    ("Prune leaves",      "prune_leaves",      ["0", "2", "5"]),
    ("Tangent blend W",   "tangent_w",         ["0", "0.5", "1.0"]),
    ("Correct convergence", "correct_convergence", ["0", "1"]),
    ("Bore aspect min",   "tunnel_min_aspect", ["0.3", "1.2", "3.0"]),
    ("Voxel resolution",  "voxel_resolution",  ["32", "64", "128"]),  # method=1 only
]


def run(exe, datadir, outdir, stl, params):
    stem = os.path.splitext(os.path.basename(stl))[0]
    js = os.path.join(outdir, stem + "_skeleton.json")
    for p in (js, os.path.join(outdir, stem + "_skeleton.obj")):
        if os.path.exists(p):
            os.remove(p)
    cmd = [exe, "--datadir", datadir, "--export-skeleton", outdir]
    if params:
        cmd += ["--skeleton-params", params]
    cmd.append(stl)
    with open(os.path.join(outdir, "sweep_log.txt"), "w") as fh:
        rc = subprocess.call(cmd, stdout=fh, stderr=subprocess.STDOUT, timeout=1800)
    if rc != 0 or not os.path.exists(js):
        return None
    with open(js) as fh:
        return json.load(fh)


def mean_nearest(a, b):
    """Mean distance from each node of a to the closest node of b (mm)."""
    if not a or not b:
        return float("nan")
    tot = 0.0
    for p in a:
        best = min((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 for q in b)
        tot += math.sqrt(best)
    return tot / len(a)


def mean_normal_angle(a, b):
    """Mean angle between corresponding cut normals (deg). Only compares the common prefix."""
    n = min(len(a), len(b))
    if n == 0:
        return float("nan")
    tot = 0.0
    for i in range(n):
        d = sum(x * y for x, y in zip(a[i], b[i]))
        tot += math.degrees(math.acos(max(-1.0, min(1.0, abs(d)))))
    return tot / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--dir", default="sandboxes/skeleton_tests")
    ap.add_argument("--datadir", default=os.path.join(os.environ.get("APPDATA", ""), "Unlayered Slicer"))
    ap.add_argument("--meshes", default="pipe_vertical,tee_solid,torus")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    outdir = os.path.abspath(os.path.join(args.dir, "sweep"))
    os.makedirs(outdir, exist_ok=True)
    meshes = [m.strip() for m in args.meshes.split(",") if m.strip()]

    print("%-20s %-8s | %s" % ("control", "value", "  ".join("%-22s" % m for m in meshes)))
    print("-" * (30 + 24 * len(meshes)))

    # Baselines (defaults, method 0) per mesh.
    base = {}
    for m in meshes:
        stl = os.path.abspath(os.path.join(args.dir, m + ".stl"))
        base[m] = run(exe, args.datadir, outdir, stl, "")
        if base[m] is None:
            print("baseline FAILED for", m)
            return 1

    verdicts = []
    for label, key, values in SWEEPS:
        rows = []
        spread_pos = {m: 0.0 for m in meshes}
        spread_nodes = {m: set() for m in meshes}
        spread_norm = {m: 0.0 for m in meshes}
        n_fail = 0
        n_run = 0
        for v in values:
            cells = []
            for m in meshes:
                stl = os.path.abspath(os.path.join(args.dir, m + ".stl"))
                params = "%s=%s" % (key, v)
                if key == "voxel_resolution":
                    params = "method=1;" + params
                d = run(exe, args.datadir, outdir, stl, params)
                n_run += 1
                if d is None:
                    # A failed run must NOT read as "changed nothing" — that is how a rejected
                    # parameter name got reported as a dead control and nearly had a working
                    # slider deleted on the strength of it.
                    n_fail += 1
                    cells.append("%-22s" % "FAIL")
                    continue
                dpos = mean_nearest(d["nodes"], base[m]["nodes"])
                dnorm = mean_normal_angle(d.get("cut_normals", []), base[m].get("cut_normals", []))
                cells.append("%-22s" % ("n=%-4d dpos=%-6.2f da=%-5.1f" % (len(d["nodes"]), dpos, dnorm)))
                spread_pos[m] = max(spread_pos[m], 0.0 if dpos != dpos else dpos)
                spread_norm[m] = max(spread_norm[m], 0.0 if dnorm != dnorm else dnorm)
                spread_nodes[m].add(len(d["nodes"]))
            rows.append("%-20s %-8s | %s" % (label if v == values[0] else "", v, "  ".join(cells)))
        print("\n".join(rows))

        worst_pos = max(spread_pos.values())
        worst_norm = max(spread_norm.values())
        node_varies = any(len(s) > 1 for s in spread_nodes.values())
        if n_fail:
            effect = "UNTESTED - %d/%d runs FAILED" % (n_fail, n_run)
        elif worst_pos < 0.05 and worst_norm < 0.5 and not node_varies:
            # Geometry within 0.05mm, normals within 0.5deg, node count fixed.
            effect = "NO MEASURABLE EFFECT"
        else:
            effect = "active"
        verdicts.append((label, key, worst_pos, worst_norm, node_varies, effect))

    print("\n\n=== verdict ===")
    print("%-20s %-10s %-10s %-10s %s" % ("control", "max dpos", "max dangle", "n varies", "effect"))
    for label, key, wp, wn, nv, effect in verdicts:
        print("%-20s %-10.3f %-10.2f %-10s %s"
              % (label, wp, wn, "yes" if nv else "no", effect))
    bad = [l for l, _, _, _, _, e in verdicts if e.startswith("UNTESTED")]
    if bad:
        print("\n!! %d control(s) could not be tested at all: %s" % (len(bad), ", ".join(bad)))
        print("   Fix the parameter name/values and re-run before drawing any conclusion.")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Run the skeleton pipeline over the generated corpus and check it behaves.

    python tools/skeleton_check.py --exe build/src/Release/unlayered-slicer.exe \
                                  --dir sandboxes/skeleton_tests

Drives `unlayered-slicer.exe --export-skeleton`, reads each `<name>_skeleton.json`, and
checks the properties that matter for automated multiplanar:

  * a bore is traced and the skeleton follows the axis, not the wall  (pipes, bores)
  * a wide shallow hole is NOT treated as a bore                      (torus, washer, window)
  * the skeleton reaches the bed when — and only when — it should     (bed snapping)
  * solid parts are unaffected                                       (regressions)

Every check prints its measured value, so a failure says what the geometry actually did
rather than just "expected true".
"""

import argparse
import json
import math
import os
import subprocess
import sys
import time

import numpy as np

TOL_AXIS = 1.0   # mm a node may stray from a straight bore axis
TOL_BED = 0.6    # mm the lowest node may sit above the bed


# ─────────────────────────────────────────────────────────────────────────────────
# measurements over one dump
# ─────────────────────────────────────────────────────────────────────────────────


class Dump:
    def __init__(self, path):
        with open(path) as fh:
            self.d = json.load(fh)
        self.nodes = np.asarray(self.d["nodes"], dtype=float).reshape(-1, 3)
        self.edges = [tuple(e) for e in self.d["edges"]]
        self.normals = np.asarray(self.d.get("cut_normals") or [], dtype=float).reshape(-1, 3)

    def __getitem__(self, k):
        return self.d[k]

    def get(self, k, dflt=None):
        return self.d.get(k, dflt)

    # ── derived ──

    def xy_radius(self):
        """Max distance of any node from the Z axis (x=y=0)."""
        return float(np.max(np.linalg.norm(self.nodes[:, :2], axis=1))) if len(self.nodes) else 0.0

    def axis_dev(self, axis, origin=(0.0, 0.0, 0.0)):
        """Max distance of any node from the line through `origin` along `axis`."""
        if not len(self.nodes):
            return 0.0
        a = np.asarray(axis, dtype=float)
        a = a / np.linalg.norm(a)
        rel = self.nodes - np.asarray(origin, dtype=float)
        return float(np.max(np.linalg.norm(rel - np.outer(rel @ a, a), axis=1)))

    def span(self, i):
        if not len(self.nodes):
            return (0.0, 0.0)
        return (float(np.min(self.nodes[:, i])), float(np.max(self.nodes[:, i])))

    def graph_length(self):
        return float(sum(np.linalg.norm(self.nodes[a] - self.nodes[b]) for a, b in self.edges))

    def degree(self):
        deg = np.zeros(len(self.nodes), dtype=int)
        for a, b in self.edges:
            deg[a] += 1
            deg[b] += 1
        return deg

    def root_z(self):
        r = self.d.get("roots") or []
        return min(float(self.nodes[i][2]) for i in r) if r else float("inf")

    def max_dist_to_polyline(self, poly):
        """Max distance from any skeleton node to a reference polyline (mm)."""
        P = np.asarray(poly, dtype=float)
        worst = 0.0
        for p in self.nodes:
            seg_d = []
            for i in range(len(P) - 1):
                ab = P[i + 1] - P[i]
                L2 = float(ab @ ab)
                t = 0.0 if L2 < 1e-12 else max(0.0, min(1.0, float((p - P[i]) @ ab) / L2))
                seg_d.append(np.linalg.norm(P[i] + t * ab - p))
            worst = max(worst, min(seg_d) if seg_d else float("inf"))
        return float(worst)

    def normals_align(self, axis):
        """Worst |1 - |n.axis|| over the cut normals (0 = every cut is square to the axis)."""
        if not len(self.normals):
            return 0.0
        a = np.asarray(axis, dtype=float)
        a = a / np.linalg.norm(a)
        return float(np.max(1.0 - np.abs(self.normals @ a)))


def elbow_reference(n=200):
    """Ideal centreline of pipe_elbow: quarter arc, R30, inlet at the origin heading +Z."""
    phis = np.linspace(math.pi, math.pi / 2.0, n)
    return [(30.0 + 30.0 * math.cos(p), 0.0, 30.0 * math.sin(p)) for p in phis]


# ─────────────────────────────────────────────────────────────────────────────────
# expectations
# ─────────────────────────────────────────────────────────────────────────────────
# Each entry is name -> list of (label, fn(Dump) -> (ok, measured_str)).


def bore_traced(min_n=1):
    def f(d):
        n = len(d.get("tunnels") or [])
        return n >= min_n, f"{n} bore(s)"
    return ("bore traced", f)


def no_bore():
    def f(d):
        n = len(d.get("tunnels") or [])
        return n == 0, f"{n} bore(s)"
    return ("no bore (hole is a feature)", f)


def cycles(expect):
    def f(d):
        c = d["cycles_final"]
        ok = (c == 0) if expect == 0 else (c >= 1)
        return ok, f"raw {d['cycles_raw']} -> final {c}"
    return (f"cycles {'== 0' if expect == 0 else '>= 1'}", f)


def on_axis(axis=(0, 0, 1), origin=(0, 0, 0), tol=TOL_AXIS):
    def f(d):
        v = d.axis_dev(axis, origin)
        return v <= tol, f"max deviation {v:.2f}mm"
    return (f"follows the {axis} axis through {origin} (<= {tol}mm)", f)


def reaches_bed(tol=TOL_BED):
    def f(d):
        lo = d.span(2)[0]
        return lo <= d["bed_z"] + tol, f"lowest node z={lo:.2f} (bed {d['bed_z']:.2f})"
    return (f"reaches the bed (<= {tol}mm)", f)


def spans_z(lo_max, hi_min):
    def f(d):
        lo, hi = d.span(2)
        return lo <= lo_max and hi >= hi_min, f"z span {lo:.2f}..{hi:.2f}"
    return (f"spans z (<= {lo_max}, >= {hi_min})", f)


def spans_x(lo_max, hi_min):
    def f(d):
        lo, hi = d.span(0)
        return lo <= lo_max and hi >= hi_min, f"x span {lo:.2f}..{hi:.2f}"
    return (f"spans x (<= {lo_max}, >= {hi_min})", f)


def root_on_bed(tol=TOL_BED):
    def f(d):
        z = d.root_z()
        return z <= d["bed_z"] + tol, f"lowest root z={z:.2f}"
    return ("has a root on the bed", f)


def no_bed_snap():
    def f(d):
        s = d["snapped_nodes"]
        return s == 0, f"{s} tip(s) snapped"
    return ("no bed snap (part is on its side)", f)


def near_polyline(poly, tol):
    def f(d):
        v = d.max_dist_to_polyline(poly)
        return v <= tol, f"max deviation {v:.2f}mm"
    return (f"follows the reference centreline (<= {tol}mm)", f)


def cuts_square_to(axis, tol=0.05):
    def f(d):
        v = d.normals_align(axis)
        return v <= tol, f"worst 1-|n.a| = {v:.3f}"
    return ("cut planes square to the axis", f)


def has_nodes(n=4):
    def f(d):
        k = len(d.nodes)
        return k >= n, f"{k} nodes"
    return (f"produced a skeleton (>= {n} nodes)", f)


EXPECT = {
    "pipe_vertical":       [bore_traced(), cycles(0), on_axis(), spans_z(0.6, 59.4),
                            reaches_bed(), root_on_bed(), cuts_square_to((0, 0, 1))],
    "pipe_thin_wall":      [bore_traced(), cycles(0), on_axis(), spans_z(0.6, 59.4), reaches_bed()],
    # axis lies along X at z = OD/2 = 10 after the part is laid on the bed
    "pipe_horizontal":     [bore_traced(), cycles(0), on_axis((1, 0, 0), (0, 0, 10), 1.0),
                            spans_x(0.6, 59.4), no_bed_snap()],
    "pipe_elbow":          [bore_traced(), cycles(0), near_polyline(elbow_reference(), 2.5),
                            reaches_bed(), root_on_bed()],
    "pipe_flange_blind":   [bore_traced(), cycles(0), on_axis(), spans_z(0.6, 59.0),
                            reaches_bed(), root_on_bed()],
    "pipe_flange_through": [bore_traced(), cycles(0), on_axis(), spans_z(0.6, 59.0),
                            reaches_bed(), root_on_bed()],
    "pipe_reducer":        [bore_traced(), cycles(0), on_axis(), spans_z(0.6, 59.0), reaches_bed()],
    "block_bored":         [bore_traced(), cycles(0), on_axis(), spans_z(0.6, 59.4), reaches_bed()],
    "torus":               [no_bore(), cycles(1), has_nodes()],
    "washer":              [no_bore(), cycles(1), has_nodes()],
    "plate_windowed":      [no_bore(), cycles(1), has_nodes()],
    "cylinder_solid":      [no_bore(), cycles(0), on_axis(), reaches_bed(), root_on_bed()],
    "cone_solid":          [no_bore(), cycles(0), on_axis(), reaches_bed()],
    # A sealed box is not a multiplanar target; all that is asserted is that the inner shell
    # is ignored (no bore, no cycle) and a skeleton comes out. Whether its MCF blob reaches
    # the bed depends on the blob's tip directions, and bed snapping is deliberately bounded
    # to tips near the bed rather than dragging a mid-height tip the whole way down.
    "box_hollow_closed":   [no_bore(), cycles(0), has_nodes(2)],
    "tee_solid":           [no_bore(), has_nodes(8)],
}


# ─────────────────────────────────────────────────────────────────────────────────


# Meshes that are also checked on a rotated instance. Cheap ones that cover the shapes whose
# bed behaviour differs: an upright bore, a bend, and a plain solid.
ROTATED = ("pipe_vertical", "pipe_elbow", "cylinder_solid", "pipe_horizontal")


def join_params(a, b):
    return ";".join(x for x in (a, b) if x)


def rotated_root_on_bed(d, deg_x):
    """The DAG root must sit on the world bed once the instance rotation is applied.

    Nodes are stored in the object's local frame, so the check re-applies the same transform the
    CLI used: rotate about X, then drop so the lowest node sits at 0.
    """
    import math
    a = math.radians(deg_x)
    ca, sa = math.cos(a), math.sin(a)
    w = [(x, y * ca - z * sa, y * sa + z * ca) for (x, y, z) in d.nodes]
    roots = d.d.get("roots") or []
    if not w or not roots:
        return ("rotated root on the world bed", False, "no nodes or no root")
    zmin = min(p[2] for p in w)
    rz = w[roots[0]][2] - zmin
    return ("rotated root on the world bed (<= 1.0mm)", rz <= 1.0,
            f"root sits {rz:.2f}mm above the world bed")


def run_one(exe, datadir, stl, outdir, params, log_dir, timeout):
    cmd = [exe, "--datadir", datadir, "--export-skeleton", outdir]
    if params:
        cmd += ["--skeleton-params", params]
    cmd += [stl]
    log = os.path.join(log_dir, os.path.basename(stl) + ".log")
    t0 = time.time()
    with open(log, "wb") as fh:
        try:
            rc = subprocess.call(cmd, stdout=fh, stderr=subprocess.STDOUT, timeout=timeout,
                                 cwd=log_dir)
        except subprocess.TimeoutExpired:
            return None, time.time() - t0, log, "TIMEOUT"
    return rc, time.time() - t0, log, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="build/src/Release/unlayered-slicer.exe")
    ap.add_argument("--dir", default="sandboxes/skeleton_tests")
    ap.add_argument("--out", default=None, help="dump directory (default <dir>/out)")
    ap.add_argument("--datadir", default=os.path.expandvars(r"%APPDATA%\Unlayered Slicer"))
    ap.add_argument("--params", default="", help="passed through as --skeleton-params")
    ap.add_argument("--only", default="", help="comma-separated subset of mesh names")
    ap.add_argument("--timeout", type=int, default=300)
    a = ap.parse_args()

    outdir = a.out or os.path.join(a.dir, "out")
    os.makedirs(outdir, exist_ok=True)
    # The CLI is run with cwd set to the dump dir (so its 00000.log lands there), which would
    # break a relative --exe.
    a.exe = os.path.abspath(a.exe)

    names = sorted(EXPECT) if not a.only else [n.strip() for n in a.only.split(",")]
    if a.params:
        print(f"skeleton params: {a.params}")
    print(f"exe: {a.exe}\ndumps: {outdir}\n")

    n_pass = n_fail = 0
    rows = []
    for name in names:
        stl = os.path.join(a.dir, name + ".stl")
        if not os.path.exists(stl):
            print(f"  {name:22s} SKIP (no {stl})")
            continue

        rc, secs, log, err = run_one(a.exe, a.datadir, os.path.abspath(stl), os.path.abspath(outdir),
                                     a.params, os.path.abspath(outdir), a.timeout)
        jpath = os.path.join(outdir, name + "_skeleton.json")
        if err or not os.path.exists(jpath):
            rows.append((name, secs, [("ran", False, err or f"rc={rc}, no dump (see {log})")]))
            n_fail += 1
            continue

        d = Dump(jpath)
        results = []
        ok_all = True
        for label, fn in EXPECT[name]:
            try:
                ok, measured = fn(d)
            except Exception as exc:  # a malformed dump should read as a failure, not a crash
                ok, measured = False, f"check raised {exc!r}"
            results.append((label, ok, measured))
            ok_all = ok_all and ok
        n_pass += int(ok_all)
        n_fail += int(not ok_all)
        rows.append((name, secs, results))

        # ── the same mesh on a ROTATED instance ──
        # Every generated mesh sits on z=0 with an identity instance, so nothing here exercised
        # the instance transform — which is exactly how a bug that put the DAG root at the TOP of
        # a rotated part passed a clean 15/15. The pipeline runs in world space, so the check is
        # frame-independent: whatever the rotation, the root must come out at the world bed.
        if name in ROTATED:
            rc, rsecs, rlog, rerr = run_one(a.exe, a.datadir, os.path.abspath(stl),
                                            os.path.abspath(outdir),
                                            join_params(a.params, "test_rot=-90,0,0"),
                                            os.path.abspath(outdir), a.timeout)
            rname = name + " [rotated -90x]"
            if rerr or not os.path.exists(jpath):
                rows.append((rname, rsecs, [("ran", False, rerr or f"rc={rc}, no dump")]))
                n_fail += 1
            else:
                rd = Dump(jpath)
                rres = [rotated_root_on_bed(rd, -90.0)]
                rok = all(x[1] for x in rres)
                n_pass += int(rok)
                n_fail += int(not rok)
                rows.append((rname, rsecs, rres))

    for name, secs, results in rows:
        bad = [r for r in results if not r[1]]
        print(f"{'PASS' if not bad else 'FAIL'}  {name:22s} {secs:6.1f}s")
        for label, ok, measured in results:
            print(f"        {'ok  ' if ok else 'FAIL'} {label:44s} {measured}")

    print(f"\n{n_pass} passed, {n_fail} failed")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())

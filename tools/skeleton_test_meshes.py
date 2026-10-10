#!/usr/bin/env python3
"""Generate the STL corpus used to exercise the skeleton / automated-multiplanar pipeline.

Every mesh is watertight, consistently outward-oriented, and sits on z = 0 so that
"the bed" is the plane z = 0 in the object's own frame.

    python tools/skeleton_test_meshes.py --out sandboxes/skeleton_tests

Each mesh is validated (every half-edge paired exactly once, positive signed volume)
before it is written. The genus printed per mesh comes from the Euler characteristic, so
it tells you up front whether a case is a through-hole (genus >= 1) or a blind pocket
(genus 0) — the blind-pocket cases are the reason tunnel detection keys off cross-section
hole persistence rather than off genus.
"""

import argparse
import math
import os
import struct
import sys

import numpy as np

# ─────────────────────────────────────────────────────────────────────────────────
# mesh container + validation
# ─────────────────────────────────────────────────────────────────────────────────


class Mesh:
    def __init__(self):
        self.v = []
        self.f = []

    def add_v(self, p):
        self.v.append((float(p[0]), float(p[1]), float(p[2])))
        return len(self.v) - 1

    def add_tri(self, a, b, c):
        if a != b and b != c and a != c:
            self.f.append((a, b, c))

    def add_quad(self, a, b, c, d):
        self.add_tri(a, b, c)
        self.add_tri(a, c, d)

    def merge(self, other, flip=False):
        base = len(self.v)
        self.v.extend(other.v)
        for (a, b, c) in other.f:
            self.f.append((c + base, b + base, a + base) if flip else (a + base, b + base, c + base))

    # ── validation ──────────────────────────────────────────────────────────────

    def signed_volume(self):
        V = np.asarray(self.v)
        F = np.asarray(self.f, dtype=np.int64)
        a, b, c = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
        return float(np.sum(np.einsum("ij,ij->i", a, np.cross(b, c))) / 6.0)

    def edge_report(self):
        """(unpaired, doubled) — a closed, consistently oriented surface has both 0."""
        half = {}
        for (a, b, c) in self.f:
            for e in ((a, b), (b, c), (c, a)):
                half[e] = half.get(e, 0) + 1
        unpaired = doubled = 0
        for (a, b), n in half.items():
            if n > 1:
                doubled += 1
            if half.get((b, a), 0) != n:
                unpaired += 1
        return unpaired, doubled

    def genus(self):
        nf = len(self.f)
        used = {i for t in self.f for i in t}
        ne = len({(min(a, b), max(a, b)) for (x, y, z) in self.f for (a, b) in ((x, y), (y, z), (z, x))})
        chi = len(used) - ne + nf
        return (2 - chi) / 2.0

    def flip_all(self):
        self.f = [(c, b, a) for (a, b, c) in self.f]

    def subdivide(self, levels=1):
        """Midpoint-split every triangle into 4. Geometry is unchanged (faces are planar).

        MCF contraction needs vertices to work with: on a 28-triangle T-prism it produced a
        16 mm stub for a 60x50 part. Real STLs are dense, so coarse prisms get subdivided
        rather than left to misrepresent the pipeline.
        """
        for _ in range(levels):
            mid = {}

            def midpoint(a, b):
                k = (min(a, b), max(a, b))
                if k not in mid:
                    pa, pb = self.v[a], self.v[b]
                    mid[k] = self.add_v(((pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2, (pa[2] + pb[2]) / 2))
                return mid[k]

            out = []
            for (a, b, c) in self.f:
                ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
                out += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
            self.f = out
        return self

    def translate(self, d):
        self.v = [(x + d[0], y + d[1], z + d[2]) for (x, y, z) in self.v]

    def rotate_y90(self):
        """+Z -> +X (lays a vertical part on its side)."""
        self.v = [(z, y, -x) for (x, y, z) in self.v]

    def drop_to_bed(self):
        self.translate((0.0, 0.0, -min(p[2] for p in self.v)))

    def finalize(self, name, expect_genus=None):
        if self.signed_volume() < 0:
            self.flip_all()
        self.drop_to_bed()
        unpaired, doubled = self.edge_report()
        vol = self.signed_volume()
        g = self.genus()
        ok = unpaired == 0 and doubled == 0 and vol > 0
        if expect_genus is not None and abs(g - expect_genus) > 1e-6:
            ok = False
        print(f"  {name:24s} {len(self.v):6d} v {len(self.f):6d} f  vol={vol:9.1f}  genus={g:4.1f}  "
              + ("OK" if ok else f"BAD (unpaired={unpaired} doubled={doubled} expect_genus={expect_genus})"))
        if not ok:
            raise SystemExit(f"{name}: generated mesh failed validation")
        return g

    def write_stl(self, path):
        V = np.asarray(self.v, dtype=np.float64)
        F = np.asarray(self.f, dtype=np.int64)
        a, b, c = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n, axis=1, keepdims=True)
        n = np.divide(n, ln, out=np.zeros_like(n), where=ln > 0)
        with open(path, "wb") as fh:
            fh.write(b"\0" * 80)
            fh.write(struct.pack("<I", len(F)))
            for i in range(len(F)):
                fh.write(struct.pack("<12fH", *n[i], *a[i], *b[i], *c[i], 0))


# ─────────────────────────────────────────────────────────────────────────────────
# 2D helpers
# ─────────────────────────────────────────────────────────────────────────────────


def signed_area(pts):
    s = 0.0
    for i in range(len(pts)):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % len(pts)]
        s += x1 * y2 - x2 * y1
    return s / 2.0


def ear_clip(pts):
    """Triangulate a simple CCW polygon. Returns index triples into pts."""
    n = len(pts)
    idx = list(range(n)) if signed_area(pts) > 0 else list(range(n))[::-1]

    def cross(o, a, b):
        return (pts[a][0] - pts[o][0]) * (pts[b][1] - pts[o][1]) - (pts[a][1] - pts[o][1]) * (pts[b][0] - pts[o][0])

    def inside(a, b, c, p):
        d1 = cross(a, b, p)
        d2 = cross(b, c, p)
        d3 = cross(c, a, p)
        return (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0)

    tris, guard = [], 0
    while len(idx) > 3 and guard < 10000:
        guard += 1
        for i in range(len(idx)):
            a, b, c = idx[i - 1], idx[i], idx[(i + 1) % len(idx)]
            if cross(a, b, c) <= 0:
                continue  # reflex
            if any(inside(a, b, c, p) for p in idx if p not in (a, b, c)):
                continue
            tris.append((a, b, c))
            idx.pop(i)
            break
        else:
            break
    if len(idx) == 3:
        tris.append((idx[0], idx[1], idx[2]))
    return tris


def circle_ring(r, n, cw=False):
    a = np.arange(n) * (2.0 * math.pi / n)
    pts = [(r * math.cos(t), r * math.sin(t)) for t in a]
    return pts[::-1] if cw else pts


def square_ring(side, n, cw=False):
    """n points on a square boundary, sampled at the same angles as circle_ring(.., n).

    Matching the angular sampling of the hole lets the cap between an outer square and
    an inner circle be a clean 1:1 quad strip.
    """
    h = side / 2.0
    pts = []
    for t in np.arange(n) * (2.0 * math.pi / n):
        c, s = math.cos(t), math.sin(t)
        k = h / max(abs(c), abs(s))
        pts.append((k * c, k * s))
    return pts[::-1] if cw else pts


# ─────────────────────────────────────────────────────────────────────────────────
# builders
# ─────────────────────────────────────────────────────────────────────────────────


def revolve(profile, n=72):
    """Revolve a closed CCW (r, z) profile about the Z axis.

    A profile that touches r = 0 collapses to a pole there and yields a genus-0 solid
    (cylinder, cone, cup); one that stays off the axis yields a genus-1 solid of
    revolution (pipe, washer, torus).
    """
    assert signed_area(profile) > 0, "profile must be CCW in the (r, z) plane"
    m = Mesh()
    thetas = np.arange(n) * (2.0 * math.pi / n)
    ct, st = np.cos(thetas), np.sin(thetas)

    rings = []
    for (r, z) in profile:
        if abs(r) < 1e-12:
            rings.append([m.add_v((0.0, 0.0, z))] * n)
        else:
            rings.append([m.add_v((r * ct[i], r * st[i], z)) for i in range(n)])

    k = len(profile)
    for s in range(k):
        s2 = (s + 1) % k
        for i in range(n):
            j = (i + 1) % n
            # winding chosen so the normal is dP/dtheta x dP/ds — outward for a CCW profile
            m.add_quad(rings[s][i], rings[s][j], rings[s2][j], rings[s2][i])
    return m


def sweep_annulus(centers, tangents, r_out, r_in, n=48):
    """Sweep an annular section along a path; annular caps close both ends."""
    m = Mesh()
    thetas = np.arange(n) * (2.0 * math.pi / n)

    frames, ref = [], None
    for t in tangents:  # parallel transport so the section does not spin along the path
        t = np.asarray(t, dtype=float)
        t = t / np.linalg.norm(t)
        seed = np.array([0.0, 1.0, 0.0]) if ref is None else ref
        ref = seed - np.dot(seed, t) * t
        if np.linalg.norm(ref) < 1e-9:
            ref = np.array([1.0, 0.0, 0.0]) - t[0] * t
        ref = ref / np.linalg.norm(ref)
        frames.append((t, ref, np.cross(t, ref)))

    outer, inner = [], []
    for (c, (t, u, w)) in zip(centers, frames):
        c = np.asarray(c, dtype=float)
        outer.append([m.add_v(c + r_out * (math.cos(a) * u + math.sin(a) * w)) for a in thetas])
        inner.append([m.add_v(c + r_in * (math.cos(a) * u + math.sin(a) * w)) for a in thetas])

    for s in range(len(centers) - 1):
        for i in range(n):
            j = (i + 1) % n
            m.add_quad(outer[s][i], outer[s][j], outer[s + 1][j], outer[s + 1][i])
            m.add_quad(inner[s][j], inner[s][i], inner[s + 1][i], inner[s + 1][j])
    for i in range(n):  # annular end caps
        j = (i + 1) % n
        m.add_quad(inner[0][i], inner[0][j], outer[0][j], outer[0][i])
        m.add_quad(outer[-1][i], outer[-1][j], inner[-1][j], inner[-1][i])
    return m


def prism(outer, holes, h, axis="z"):
    """Extrude a 2D polygon by h.

    Every loop — outer and holes alike — is given CCW and with the SAME vertex count, so
    that index i of a hole and index i of the outer loop sit at the same angle about the
    centroid and the cap between them is a clean 1:1 quad strip. Hole side walls get the
    reversed winding so their normals face into the hole.
    """
    m = Mesh()

    def place(p, t):
        return (p[0], p[1], t) if axis == "z" else (p[0], t, p[1])

    loops = [list(outer)] + [list(hl) for hl in holes]
    bot = [[m.add_v(place(p, 0.0)) for p in loop] for loop in loops]
    top = [[m.add_v(place(p, h)) for p in loop] for loop in loops]

    for li, loop in enumerate(loops):
        k = len(loop)
        for i in range(k):
            j = (i + 1) % k
            if li == 0:
                m.add_quad(bot[li][i], top[li][i], top[li][j], bot[li][j])
            else:
                m.add_quad(bot[li][j], top[li][j], top[li][i], bot[li][i])

    if holes:
        for li in range(1, len(loops)):
            k = len(loops[li])
            assert k == len(outer), "hole loop must match the outer loop vertex count"
            for i in range(k):
                j = (i + 1) % k
                m.add_quad(bot[0][i], bot[0][j], bot[li][j], bot[li][i])
                m.add_quad(top[0][j], top[0][i], top[li][i], top[li][j])
    else:
        # Boundary half-edges of a cap must run opposite to the side wall's, or the two
        # would agree and every rim edge would be doubled.
        for (a, b, c) in ear_clip(list(outer)):
            m.add_tri(bot[0][a], bot[0][b], bot[0][c])
            m.add_tri(top[0][a], top[0][c], top[0][b])
    return m


def box(lo, hi):
    m = Mesh()
    idx = {}
    for xi in (0, 1):
        for yi in (0, 1):
            for zi in (0, 1):
                idx[(xi, yi, zi)] = m.add_v((lo[0] if xi == 0 else hi[0],
                                             lo[1] if yi == 0 else hi[1],
                                             lo[2] if zi == 0 else hi[2]))
    for q in ([(0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)],
              [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)],
              [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)],
              [(0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)],
              [(0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)],
              [(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)]):
        m.add_quad(*[idx[c] for c in q])
    return m


# ─────────────────────────────────────────────────────────────────────────────────
# the corpus:  name -> (mesh, expected genus, note)
# ─────────────────────────────────────────────────────────────────────────────────


def build_corpus():
    out = {}

    # straight pipes, bore through — the headline case
    for name, r_in in (("pipe_vertical", 6.0), ("pipe_thin_wall", 9.0)):
        out[name] = (revolve([(r_in, 0), (10, 0), (10, 60), (r_in, 60)]), 1,
                     f"OD20 ID{2 * r_in:g} H60 pipe, bore through")

    # the same pipe lying on the bed, axis along X at z = 10
    lying = revolve([(6, 0), (10, 0), (10, 60), (6, 60)])
    lying.rotate_y90()
    out["pipe_horizontal"] = (lying, 1, "OD20 ID12 L60 pipe on its side, axis along X at z=10")

    # 90 deg swept elbow: inlet flat on the bed pointing +Z, outlet pointing +X
    R, steps = 30.0, 40
    phis = np.linspace(math.pi, math.pi / 2.0, steps)
    out["pipe_elbow"] = (sweep_annulus([(R + R * math.cos(p), 0.0, R * math.sin(p)) for p in phis],
                                       [(math.sin(p), 0.0, -math.cos(p)) for p in phis], 10.0, 6.0), 1,
                         "90deg elbow OD20 ID12 bend R30, inlet on the bed")

    # U-bend standing on its bend: no leg anywhere -- both ends point up -- so the root belongs
    # under the bottom of the U, with the cuts fanning out both ways.
    R, cz = 30.0, 40.0  # bend radius; bend centre height, so the centreline bottoms out at z = 10
    legs = np.linspace(80.0, cz, 9)[:-1]
    arc = np.linspace(math.pi, 2.0 * math.pi, 37)
    centers = ([(-R, 0.0, z) for z in legs] +
               [(R * math.cos(p), 0.0, cz + R * math.sin(p)) for p in arc] +
               [(R, 0.0, z) for z in legs[::-1][1:]])
    tangents = ([(0.0, 0.0, -1.0)] * len(legs) +
                [(-math.sin(p), 0.0, math.cos(p)) for p in arc] +
                [(0.0, 0.0, 1.0)] * (len(legs) - 1))
    out["pipe_u"] = (sweep_annulus(centers, tangents, 10.0, 6.0), 1,
                     "U-bend OD20 ID12 R30 standing on its bend, both ends up at z=80")

    # 120 deg bend lying flat on the bed: the arc's middle is the root, not either end.
    arc = np.linspace(-math.pi / 3.0, math.pi / 3.0, 49)
    out["pipe_bend_flat"] = (sweep_annulus([(40.0 * math.cos(t), 40.0 * math.sin(t), 10.0) for t in arc],
                                           [(-math.sin(t), math.cos(t), 0.0) for t in arc], 10.0, 6.0), 1,
                             "120deg bend OD20 ID12 R40 lying flat, axis at z=10, middle at (40,0)")

    # blind bore (a cup): genus 0, so genus alone cannot be the trigger
    out["pipe_flange_blind"] = (revolve([(0, 0), (20, 0), (20, 8), (10, 8), (10, 60), (6, 60), (6, 8), (0, 8)]), 0,
                                "pipe on a solid D40 flange, bore blind above z=8")

    # same flange, bore through to the bed — bed snap has to land in the bore
    out["pipe_flange_through"] = (revolve([(6, 0), (20, 0), (20, 8), (10, 8), (10, 60), (6, 60)]), 1,
                                  "pipe on a D40 flange, bore through to the bed")

    # coaxial reducer: OD24/ID16 up to z=30, then OD18/ID10
    out["pipe_reducer"] = (revolve([(8, 0), (12, 0), (12, 30), (9, 30), (9, 60), (5, 60), (5, 30), (8, 30)]), 1,
                           "coaxial reducer OD24/ID16 -> OD18/ID10 at z=30")

    # torus: through-hole, but only one tube diameter deep — must keep its loop
    nr = 48
    out["torus"] = (revolve([(25.0 + 6.0 * math.cos(a), 6.0 * math.sin(a))
                             for a in np.arange(nr) * (2 * math.pi / nr)]), 1,
                    "torus R25 r6 — wide shallow hole, loop must be kept")

    # washer: same story, flat
    out["washer"] = (revolve([(15, 0), (25, 0), (25, 8), (15, 8)]), 1, "OD50 ID30 H8 washer, loop must be kept")

    # bored block: hole persists 4x its width -> treat as a bore
    out["block_bored"] = (prism(square_ring(40, 48), [circle_ring(7.5, 48)], 60.0), 1,
                          "40x40x60 block with a D15 bore along Z")

    # windowed plate: hole 5x wider than deep -> loop must be kept
    out["plate_windowed"] = (prism(square_ring(60, 48), [square_ring(30, 48)], 6.0), 1,
                             "60x60x6 plate with a 30mm window, loop must be kept")

    # solid regressions
    out["cylinder_solid"] = (revolve([(0, 0), (10, 0), (10, 60), (0, 60)]), 0, "solid D20 H60 cylinder")
    out["cone_solid"] = (revolve([(0, 0), (14, 0), (0, 60)]), 0, "solid cone D28 H60")

    # Sealed cavity: two shells; keep_largest_connected_components drops the inner one. The
    # outer shell is subdivided so it unambiguously has the most faces — with a plain 12-tri
    # box on each side the "largest" component is a tie and which shell survives is arbitrary.
    hb = prism(square_ring(40, 48), [], 40.0)
    hb.merge(box((-15, -15, 5), (15, 15, 35)), flip=True)
    out["box_hollow_closed"] = (hb, None, "40mm box with a sealed 30mm cavity (two shells)")

    # branching solid, no bore — convergence regression; must not crash or hang
    tee = [(-30, 0), (30, 0), (30, 12), (6, 12), (6, 50), (-6, 50), (-6, 12), (-30, 12)]
    out["tee_solid"] = (prism(tee, [], 16.0, axis="y").subdivide(3), 0,
                        "solid T prism (branching, no bore)")

    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="sandboxes/skeleton_tests")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    print(f"writing corpus to {a.out}")
    notes = {}
    for name, (mesh, expect_g, note) in build_corpus().items():
        g = mesh.finalize(name, expect_g)
        mesh.write_stl(os.path.join(a.out, name + ".stl"))
        notes[name] = (note, g)

    with open(os.path.join(a.out, "README.txt"), "w") as fh:
        fh.write("Skeleton pipeline test corpus (tools/skeleton_test_meshes.py)\n")
        fh.write("Watertight, outward-oriented, sitting on z=0.\n\n")
        for name, (note, g) in notes.items():
            fh.write(f"{name + '.stl':28s} genus {g:g}  {note}\n")
    print(f"\n{len(notes)} meshes written")
    return 0


if __name__ == "__main__":
    sys.exit(main())

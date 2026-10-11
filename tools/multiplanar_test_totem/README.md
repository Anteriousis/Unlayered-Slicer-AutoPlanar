# Multiplanar Calibration Totem (MCT-1)

A parametric test part for multiplanar (tilted flat layer) printing. One 76 × 76 × 28 mm print shows, side
by side, how a printer and slicer handle tilt, overhangs, curved tops, transitions and branched shapes. It
is designed to compare **different machine types** on the same part: a fixed-nozzle 3-axis printer, a
5-axis printer and a belt printer each tilt a different subset of its features, and the boundary between
tilted and planar regions is the measurement.

Generate it (and every module as its own STL, plus a manifest of every angle):

```
pip install trimesh manifold3d matplotlib
python3 make_totem.py --out mct1 --preview
```

## Features and what to look for

| Feature | Where | What it tests | What to look at |
|---|---|---|---|
| Tilt ladders | four rows, one per side | the per-side tilt limit | the first rung that prints with planar steps instead of a smooth tilted top; compare the four sides |
| Overhang shelves | 8 shelves on the tower, 15–55° | support threshold vs available tilt | which shelves needed support planar, which lost it when tilted, sag on the underside |
| Dome cap | top of the tower | curved-surface finish | terrace bands, planar vs tilted |
| Flat reference | base top, corner cube (+X−Y) | "keep planar" decisions; dimensional check | should stay planar; cube 10.00 mm |
| Arch | corner −X+Y | crown bridge/overhang, curved spine | sag at the crown; skeleton-cut behaviour |
| Fork | corner +X+Y | branched geometry | branch surfaces and joints |
| Seam witness | corner −X−Y | seam position across a transition | whether the seam jumps between planar and tilted sections |

Default rung angles (top slope from horizontal): **2, 3, 4, 5, 7, 10, 15, 25°** on every side. The ramps rise
away from the tower, so the tilted layers on the +X ladder climb toward +X, and so on. Reading the four
ladders together maps the printer's limit for each side of its hotend.

Default shelf undersides (from horizontal): **15, 20, 25, 30, 35, 40, 45, 55°**, two per tower face, in
order +X (low, high), +Y, −X, −Y.

## Re-making it for another printer

Every angle is a parameter. Pick rungs that bracket the expected limit:

| Printer | Suggested `--tilts` |
|---|---|
| fixed-nozzle 3-axis with a small limit (e.g. 5°) | `1,2,3,4,5,6,8,10` |
| 3-axis with a slim, high-clearance hotend | `2,4,6,8,10,12,15,20` |
| 5-axis / rotary | `5,10,15,20,30,40,50,60` |
| belt printer | default, printed with the ladders along and across the belt |

`--scale` scales the whole part, `--only tilt_ladders,shelves` keeps a subset (the base is always kept),
and `--overhangs` changes the shelf angles.

## Recording results

`MCT-1_manifest.json` lists every rung and shelf with its side and angle. For each, note: tilted or planar
(from the slicer preview), visible steps (yes/no), support needed (yes/no), defects. The rung where a
side switches from tilted to planar, together with any defect on the rung below it, is that side's
practical limit. A preview or a clean collision check is not a physical result: only the printed part is.

The part is original geometry made for this purpose; it is not derived from any existing benchmark model.

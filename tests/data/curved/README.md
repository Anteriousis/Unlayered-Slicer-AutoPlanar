# Regional native slicing fixture

`MCT-1_Centauri_5deg.stl` is the 76 x 76 x 28 mm Centauri variant of the
repository's parametric MCT-1 totem, generated with rung angles
`1,2,3,4,5,6,8,10` degrees and shelf undersides `15,20,25,30,35,40,45,55`
degrees. It contains the ladders, dome/tower, shelves, arch, fork, seam witness
and reference cube as one closed connected solid, with 22,146 facets.

Generator: `tools/multiplanar_test_totem/make_totem.py` (repository license).
The retained binary makes the regression exercise the same model used in the
Centauri trial, independently of changes in boolean-library tessellation.

SHA256: `a9c8045012446a2bfc48275c035a23bfaa23dce0ca1b18bfb025a85c45eaaf3d`.

The native regression slices with synthetic printer bounds, 15% infill and
normal supports, checks that some local roof paths are curved, and retains
planar support paths. Its G-code is a temporary software-test artifact and
must not be printed. This model includes slopes above the Centauri's configured
nonplanar angle; those features are not a command to increase the angle limit.

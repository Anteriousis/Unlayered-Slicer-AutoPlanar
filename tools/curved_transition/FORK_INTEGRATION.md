# Fork main integration

The fork's original `main` contains the older 0.1.2 source. The original review
requests target `release/auto-planes` and `codex/claude-autoplanar-continuation`,
so merging them into those branches would not update `main`.

The updated fork PR #3 targets `main` and combines:

- Current upstream 0.1.4 source and its build/profile fixes.
- The reviewed auto-plane suggestions, painted-seam preservation and corrected
  MCT-1 geometry from the original PRs #1 and #2.
- Continuous transitions, automatic regional selection and progressive travel
  at engine commit `523c5bfa303dcf2e8450c21d776bb0135701ecb1`.
  The earlier `902d2d6e` engine and `4b0ef6cd` integration passed native and
  packaged checks. The updated engine/integration require their own exact-source
  build, native suite and packaged geometry/time comparisons.
- The independent Centauri 0.4/0.2 experimental catalog proposal at
  `3aa086faaa3827c4b996ca3b0c185bc6b95c0342`.

The integration preserves the original fork commits as ancestors. Its current
implementation/test files are the corrected upstream port, not a second copy of
the earlier engine. Old contribution patches are already included through the
ported commits; their build and release scaffolding is superseded by current
upstream scaffolding. No branch rewrite or direct update to `main` is required.

The focused workflow verifies the exact integrated source with a Windows x64
build, native tests, calibration geometry and Elegoo catalog checks. It runs on
the integration PR's branch and on `main` after a merge. Publishing remains
manual. Passing this workflow is Windows software validation, not a claim that
every platform's release packaging or all-printer custom firmware scripts were
tested by this integration.

The supplied comparison and temple exercise one material and one fixed active
nozzle. Ordinary presets leave curved modes off. Experimental Centauri presets
are shipped as selectable catalog entries; automatic/manual curved mode is
selected separately. No Bambu clearance geometry or new physical print is
claimed. Installed 0.2 mm tip geometry remains unverified. Review
[ENGINE.md](ENGINE.md) and
[the Centauri evidence](../../resources/profiles/Elegoo/NONPLANAR_CENTAURI.md)
for supported conditions and the limits of the measurements.

The three official upstream contributions remain separate. The combined fork
request prepares this experimental fork's main branch and does not merge any
official upstream request.

Progressive travel preserves the configured clearance and motion limits. It
uses a conservative bound on actual physical deposition across the plate,
including tilted bead-edge rise, rather than future unprinted layers. An XY
detour stays raised until its final approach. Unknown positions retain the full
final-plate fallback. Empty/repeated layer changes preserve an existing staging
height without upward rounding creep. See ENGINE.md for the process scope.

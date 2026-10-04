## Implementation
- [x] Regression test first: the #618 fin (0.1 thick, voxel 0.01, Draw radius 0.45 on the ridge) on a lattice through its faces and one between them, plus a sphere pole and a torus inner equator. All four failed before the fix (11.5, 1.4, 0.9 and 4.3 deg).
- [x] Measure the three hypotheses: walk vs ball footprint, lattice through vs between the faces, per-face entry counts and vote sums.
- [x] `WorkItemReader::normal_at` returns the item's area with its normal; `SculptWorkset::areas` carries it; `resolve_frame` weighs each vote by weight times area.
- [x] Fixed mesh: `class_normal` returns the class area off the cross products it already takes, the cross-level faces included (`CrossLevelNeighborhood::normal_contribution`).
- [x] Adaptive surface: `DynamicSurface::vertex_area`, and an adaptive subcase in the regression test (mutating its reader back to the equal vote fails it at 1.4 deg).
- [x] Re-baseline the arm64 macOS golden table (30 of 80 rows: the six frame verbs on five fixtures; one moved count, sphere/flatten 8 -> 10). Let the parity case write its table when hashes differ, so CI carries the x86-64 Linux and MSVC tables out as artifacts, and adopt those.
- [x] Document the rule in `docs/07` and record the cause in the roadmap's follow-up list.
- [x] Unit suite, dynamic and multires shared-brush parity, A/B of the stamp benchmarks.

No ABI change.

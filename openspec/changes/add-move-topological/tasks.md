# Tasks: add-move-topological

- [x] 1.1 Geodesic distance from an anchor through the material, on a local grid
- [x] 1.2 The grid is sized to the drag's reach, since the warp is the identity outside it
- [x] 1.3 Dilate outward, so space just off the surface still has a weight
- [x] 1.4 `field::move_topological`: sample the source through the weighted displacement
- [x] 1.5 The declared Lipschitz is MEASURED, as flatten's is
- [x] 1.6 Python bindings
- [x] 1.7 Tests: the neighbouring finger stays put where a Euclidean grab drags it;
      the grabbed part still moves; distance is along the material, not through
      the gap; a drag reaching nothing changes nothing; degenerate input refused
- [x] 1.8 Docs and an example

Found while building — all four by measuring an alternative that reads better:

- [x] 1.9 The weight has to extend into free space by at least the DISPLACEMENT.
      An output point at p takes its material from p - w*d, so a point up to |d|
      outside the original surface still needs a weight. A fixed three-cell
      shell against a drag of 0.25 left the grabbed finger 0.044 wide where it
      started at 0.204 — torn off at its own boundary.
- [x] 1.10 That shell must grow only ALONG the drag. Grown in every direction it
      put a weight in the gap between the fingers, where sampling p - w*d reached
      across into the other finger and deposited a slab of it in mid-air: a third
      run of material 0.16 wide belonging to neither.
- [x] 1.11 ...and it must carry a CONSTANT value rather than a growing distance.
      Grown as a true distance the extension is Euclidean from the reached set,
      which hands a large weight to the gap and drags the far finger anyway —
      its edge moved +0.158 to +0.088, the exact failure this exists to avoid.
      Constant keeps the translation rigid; the cost is a step where the shell
      ends, which the trilinear sampling softens.
- [x] 1.12 Nearest-cell sampling of the geodesic field is unusable: it makes the
      warp piecewise constant, declaring a Lipschitz of 14.66 and a step scale of
      0.039 — geometry correct, and the raymarcher renders an empty frame.
      Trilinear, with unreached filled to the radius so "no weight" is a finite
      value to blend toward, brings it to 7.76 and 0.074.
- [x] 1.13 A plain bug both suites caught: `outside_value` defaulted to zero, so
      a sample point outside the solved box read distance 0 — FULL weight — and
      an anchor placed away from the material warped the whole document by the
      displacement. Set before any return path.

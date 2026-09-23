## 1. Reproduce

- [x] 1.1 Sample a lattice minus a sphere (every blend profile, rounding, in a group, as a layer, every compile entry point, resumed by `compile_document_append`) and count material samples outside `tape.bounds` on main: 5,824 of 5,824 for the hard case, 19 failing assertions (`tests/unit/test_lattice_bounds.cpp`)
- [x] 1.2 Open the issue (#640)

## 2. Fix

- [x] 2.1 Report the folded material extent as is; drop the one-cell `reported_bound` fallback
- [x] 2.2 `compile_item` takes `item_material_extent`, keeping it byte-identical to a one-item layer
- [x] 2.3 Guard pyclay's three `Volume` region fallbacks against an infinite box
- [x] 2.4 Rewrite `fold bounds: an unconfined infinite grid ...` to assert the unbounded box

## 3. Prove

- [x] 3.1 Revert the fix: 20 assertions fail across the two files; restore it: all pass
- [x] 3.2 Mutate `compile_item` alone back to the one-cell expand: the entry-point case fails
- [x] 3.3 Bricks planned on documents with no lattice: the nine gallery documents, 19 tapes, bit-identical bounds and 3,188 bricks before and after, with a lattice control proving which library each probe linked (8 bricks on main, unbounded with the fix)
- [x] 3.4 Random documents with infinite grids (subtract, intersect, every blend profile, nested and inline groups, layer compositions and layer transforms, resumed appends, every compile entry point): zero material samples outside `tape.bounds` with the fix, over a million on main; 3,324 tapes of documents with no infinite grid report bit-identical bounds and the same brick plan on both
- [x] 3.5 `SdfSmoothTransaction::begin` refuses an unconfined lattice layer instead of laying its working lattice over one cell (fails on main)

## 4. Document

- [x] 4.1 `clay_tape_info` header, `docs/05`, the scene-model requirement

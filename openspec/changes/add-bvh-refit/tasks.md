# Tasks: add-bvh-refit

## 1. Make the summary combinable — this decides whether a refit is a refit

- [x] 1.1 `summarize()` loops `n.span`, so a bottom-up refit that re-summarised
      each ancestor would rescan the whole mesh at the root. Store the total
      area and the area-weighted centroid SUM on the node, both of which
      `summarize` already computes and throws away, so a parent combines in O(1)
- [x] 1.2 Keep the degenerate branch honest: `summarize` falls back to the box
      centre when the total area is zero, which breaks the weighted identity. A
      zero-area subtree has to be handled explicitly rather than inherited

## 2. The three indices a refit needs

- [x] 2.1 `source` to slot. `Tri::source` names the mesh triangle but the build
      reorders `tris_` with `nth_element`, so finding one means scanning today
- [x] 2.2 Slot to leaf, so a changed triangle reaches the node holding it
- [x] 2.3 A parent link on `Node`, to walk a leaf to the root
- [x] 2.4 Record what this costs per triangle and per node, since the tree is
      resident for a whole session on the platform where memory ends sessions

## 3. Refit

- [x] 3.1 `Bvh::refit(mesh, changed, count)`: update the named triangles' stored
      positions, mark their leaves, mark ancestors, recompute bottom-up
- [x] 3.2 Order the recomputation so a parent is never recomputed before its
      children. Nodes are appended in DFS pre-order, so a parent's index is
      always lower than its children's — descending index order is the whole
      of the ordering problem, with no queue and no sort
- [x] 3.3 `Bvh::refit(mesh)` for a global deformation
- [x] 3.4 Refuse a mesh whose triangle count differs, changing nothing
- [x] 3.5 `Bvh::quality()`, and NO automatic rebuild — the spec says why

## 4. Reach it from the sculptor

- [x] 4.1 `MeshSculptor::refit_bvh()`, taking the triangles from the classes the
      last stamp moved via `Adjacency::triangles_of`
- [x] 4.2 `refresh_bvh()` stays as the rebuild, and the header says when each is
      the right call

## 5. Prove it

- [x] 5.1 The spec delta's scenarios
- [x] 5.2 EQUIVALENCE against a fresh build after a displacement — raycast,
      closest point, unsigned distance, winding number. Bit-identity is NOT the
      claim: boxes are exact (a union of unions is the same union, and min/max
      do not round) but summaries are not (float addition is not associative),
      so the test asserts exact boxes and tolerance summaries and says which
- [x] 5.3 CONSERVATIVENESS as its own test, walking every node and checking it
      contains the triangles beneath it. This is the property that makes refit
      safe, and it is the one a wrong ordering breaks silently
- [x] 5.4 Degenerate meshes: zero-area triangles, a single triangle, a mesh
      whose indices were dropped at build time
- [x] 5.5 A refit naming a superset matches one naming exactly the moved set
- [x] 5.6 Benchmark: refit against rebuild at several mesh sizes, asserting the
      refit is flat in mesh size for a fixed brush

## 6. Say it

- [x] 6.1 The header's advice currently tells a caller not to refresh per stamp
      because it is too expensive. After this it is not, and the sentence has to
      change rather than sit there
- [x] 6.2 `MeshSculptor::refresh`'s docstring is WRONG and was reported in
      `reference/host_loop.py`: it says a stale tree "reports the surface as it
      was when the tree was built". Measured, the hit follows the moved surface
      but through stale bounds, so it drifts OFF the ray. Fixed in sculpt.h,
      clay.h and pyclay. The figures: 4.4e-2 -> 1.5e-8 in this change's own
      test, 6.9e-4 -> 3.1e-9 for `reference/host_loop.py`'s smaller stamp —
      two setups, and the first draft quoted them as one pair
- [x] 6.3 C ABI and pyclay, so `check_binding_parity` stays clean

## 7. Found by an adversarial review of this change

Five reviewers over the ordering, the index maps, the numerics, the sculptor
integration and the written claims; every finding then handed to an agent told
to refute it with a compiled probe. 26 raised, 10 refuted, 16 confirmed.

- [x] 7.1 **`refit_bvh()` named a SUBSET, which is the one thing `Bvh::refit`
      forbids.** It derived its set from `region_`, which every stamp
      overwrites, so a stroke refitted its FINAL dab and left every earlier
      one's ancestors holding pre-stroke bounds. Found independently by three
      lenses. Reproduced on a 2,400-triangle sphere: a 15-stamp stroke left 175
      triangles at pre-stroke positions, worst distance query off by 0.153 —
      and `refit()` returned true and the conservativeness check passed
      throughout, because the tree was consistent with its own stale copies.
      The sculptor now ACCUMULATES a dirty class set between fits and drains
      it, with the mark array reset through the list so the cost stays
      proportional to the stroke. Regression: a 12-stamp walking stroke plus
      one refit, verified to fail without the fix
- [x] 7.2 **The whole-mesh paths had no dirty set at all.** `apply_lattice` and
      `apply_deformer` move every vertex; a refit derived from a brush region
      would fit a handful of triangles and leave the rest. Both now mark
      everything, and a refit after one refits the whole tree
- [x] 7.3 **`bounds_contain_their_triangles()` could not see 7.1.** It compared
      the tree against its OWN stored triangles, so an under-named refit leaves
      it true. It now takes the mesh optionally and checks the stored triangles
      against the mesh's current positions, which is the half that catches a
      caller error rather than an implementation one
- [x] 7.4 **`quality()` returned 0.0 — the BEST score — for a degenerate root**,
      which reads as "queries got cheaper" for a tree nothing was measured
      about. NaN now, so every threshold comparison is false instead
- [x] 7.5 **`quality()` accumulated float areas** and overflows on a large mesh.
      Accumulated in double
- [x] 7.6 **The C ABI entry points skipped `resolve_sculptor`**, the guard every
      sibling makes — so a sculptor whose mesh had left its document, or whose
      counts had changed, went straight through
- [x] 7.7 **`quality` was a getter that silently built a tree.** 1.3 s on a
      2M-vertex mesh, with the GIL held in Python. `has_bvh()` now gates it
- [x] 7.8 **"Comparable across meshes, one threshold serves" was false.** It is
      invariant to a uniform SCALE, not to shape or triangle distribution. The
      claim is gone from all four sites; compare a tree against itself
- [x] 7.9 **`sculpt.h` advised a rebuild on a rising `quality()`** — which this
      change's own spec forbids, having measured that a rebuild helps in one
      deformation of five
- [x] 7.10 **The spec delta reinstated the exact sentence task 6.2 calls wrong**
      ("reports the surface as it was"). Corrected
- [x] 7.11 The benchmark gate held a RATIO at one mesh size and claimed a SLOPE.
      A second size was added: the same 800-triangle dab on 522k triangles
      against 130k costs 0.026 ms against 0.020 ms — 1.3x for 4x the mesh
- [x] 7.12 Quoted figures that did not reproduce: the gate's "0.0002x" (~0.0006x),
      and a drift pair that came from two different setups

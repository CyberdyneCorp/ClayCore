## Why

`tape.bounds` is where the field can hold material: what meshing marches, what a
raycast clips against, and what `clay_tape_info` tells a host to plan bricks
over. For an item repeated on an INFINITE GRID it reported one cell, although the
copies fill space (#640).

#637 closed half of this: `item_material_extent` takes an infinite grid as
unbounded, so it never narrows an intersect. It left the other half on purpose —
where the unbounded extent reached the result unconfined (the lattice alone, in a
union, or as the left operand of a subtract) `Compiler::reported_bound` fell back
to the plain union of item bounds, which is the grid's one cell, "so narrowing
never turns a document the mesher accepted into an unbounded scene". That keeps
the mesher quiet by handing it the wrong region. #637's reviewer: "a lattice minus
a sphere is still only covered by one cell, on main and on this branch".

Measured on main (9616286c), a 0.4 half-size box on a 1.5 grid minus a sphere of
radius 1, sampled on 32^3 points over five cells either way:

| fixture | material samples | outside `tape.bounds` |
|---|---|---|
| lattice − sphere, hard | 5,824 | **5,824** |
| lattice − sphere, quadratic / cubic / circular / chamfer | 5,800–5,824 | 5,616–5,824 |
| rounded lattice − sphere | 10,392 | 10,184 |
| lattice alone / ∪ sphere | 5,832 / 5,856 | 5,824 / 5,824 |
| in a group, subtracting group, subtracting layer | 5,824 | 5,824 |
| `compile_layer`, `_part`, `_prefix`, `compile_item` | 5,824–5,832 | 5,824 |
| resumed by `compile_document_append` (root, group) | 5,824 | 5,824 |

Cell zero is carved out entirely by the sphere, so for the hard case the box
covered no material at all.

## What changes

- **An unconfined infinite grid reports an infinite `tape.bounds`**, the answer a
  plane already gives. `reported_bound` is gone: every entry point reports the
  folded material extent as is. `compile_item` takes the item's material extent
  too, so it stays byte-identical to the single-item layer it stands in for.
- **What the infinite box costs a caller**, all of which were already written
  for a plane: `clay_mesh` / the quad mesher / `clay_voxel_rasterize` / the bakes
  refuse with "unbounded scene; pass a region"; `clay_sdf_smooth_begin` refuses
  the layer, as it refuses a plane's, rather than laying its working lattice over
  one cell; raycast and pick skip the clip and pick's per-ray local tape;
  `advised_params` returns no advice; the prefix cache declines and the layer
  walks in full. pyclay's `Volume.from_document`, `moved_topologically_from` and
  `flattened_from` derived a default region without an infinite check and now
  refuse with "pass bounds=".
- **Not a finite cap.** Any finite box is wrong for a field that holds material in
  every cell; the only honest finite answer is the caller's own region, which
  every region-taking entry point already accepts.
- The header (`clay_tape_info`), `docs/05` and the scene-model requirement say it.

## What does not change

A document with no infinite grid: the nine gallery documents compile 19 tapes
(document and per layer) with bit-identical bounds before and after, and plan the
same 3,188 bricks of 0.4 over them. The confined forms stay finite and narrow — a
finite shape minus a lattice keeps the shape's box, a lattice intersected with a
box is the box.

No ABI entry point or layout changes; an existing output (`clay_tape_info`'s
bounds) now reports ±FLT_MAX for a document it used to report one cell for.

## Impact

- `src/scene/tape_build.cpp`, comments in `include/clay/scene/{tape,bounds}.h`
- `bindings/python/pyclay_module.cpp` (three region fallbacks)
- `bindings/c/clay.h` (documentation only), `docs/05-claycore-library.md`
- `openspec/specs/scene-model` — "Visible SDF layers fold under a per-layer operator"

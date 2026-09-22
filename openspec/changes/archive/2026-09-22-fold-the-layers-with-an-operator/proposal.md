## Why

`scene::Layer` carries a name, a transform, protection and visibility, and no
operator. Visible SDF layers hard-union, unconditionally, and the compiler says
so in eight places. So a layer is organisation and nothing else: an artist who
wants one shape to cut another must put both in one layer and lose the ability to
hide, reorder or transform the cutter as a thing.

Giving a layer an operator turns the stack into a procedural modelling stack —
`A - B + C` — where hiding the cutter restores the uncut geometry and reordering
is a modelling decision rather than a cosmetic one.

## What Changes

- `LayerComposition` on an SDF layer — op, blend, `blend_k`, rounding — using the
  **existing item-level enums**. A layer boolean is the same operation an item
  boolean is and SHALL NOT get a second vocabulary or a second evaluator.
- The document compile stops unioning and **folds**: the first visible SDF layer
  initialises the accumulator and every later one combines with what is below.
- **The first visible layer's operator is not applied.** `Subtract(empty, A)` and
  `Intersect(empty, A)` are the two ways a stack can open with nothing on screen
  and no error, and an artist who reorders their base to the top would hit both.
- Bounds per operator, which is the correctness half: a subtract cannot create
  material outside its left operand, an intersect is bounded by the intersection,
  and defaulting everything to union bounds means missing ray hits and incomplete
  brick plans rather than a slow frame.
- Exactness and Lipschitz folded exactly as the item-level combine folds them,
  with a parity fixture: **layer A then layer B(Subtract) must equal one layer
  holding A then B(Subtract)** in distance, colour, bounds and safe step.
- Composition changes are undoable through the existing layer-property history,
  persisted, and default to union so every existing document renders as it does.
- Non-SDF layers REFUSE the setter rather than storing state that does nothing.

## The blast radius, named up front

This is why the guide puts it last, and the audit agrees. The inter-layer union
is not only in `compile_document`:

| Site | What assumes it |
|---|---|
| `tape.h:147` | "Whole document: visible SDF layers chained by hard union" |
| `tape.h:200` | the resumable checkpoint: the trailing union an append must be emitted BEFORE |
| `tape.h:372` | `compile_document_part`: "The union to fold them with is a HARD Add ... Anything else is a different field" |
| `tape.h:390` | `compile_document_except`, for previewing one layer beside the rest |
| `tape_build.cpp:1281` | "a hard Add is exact and adds no extent, so the prefix's are the chain's" |
| `clay_c.cpp:1502,1541` | the brick refill's multi-layer split, which folds the two halves itself |

A layer fold that is not a hard Add breaks the multi-layer brick resume unless
each of these is taught the operator — and the failure is SILENT, because a
refill that folds with the wrong operator returns a field that never existed
rather than an error. `design.md` decides what each site does; the first
decision it must make is whether the resumable split stays available at all when
the layer above is not a union.

## Capabilities

### Modified Capabilities
- `scene-model`: visible SDF layers fold under a per-layer operator rather than
  unioning, with the bounds and exactness that makes it correct.
- `c-abi`: the composition setter and getter.

## Impact

- `include/clay/scene/document.h`, `src/scene/tape_build.cpp`,
  `include/clay/scene/tape.h`, `cull_index`, `io/`, `session/`, `bindings/`.
- The six sites above, each decided rather than discovered.
- ABI grows; document format gains a per-layer block that defaults to union.

## Task 3.1: bounds per operator, measured

`tape.bounds` is folded combine by combine through `scene::combine_extent` at
every level a combine happens (item, group, layer, and a resume unwinding the
same stack). A subtract keeps its left operand's extent, an intersect the
overlap. It is the box `clay_document_mesh`, `clay_voxel_rasterize`, the SDF
export's default region and a raycast's clip all read, and what `clay_tape_info`
hands a host to plan bricks over.

The probe meshes the SAME compiled tape over two regions, the new `tape.bounds`
and the plain union of item bounds (what `tape.bounds` was before this task), so
the field is identical and only the region differs. Voxel 0.02, bricks counted
at an edge of 0.16 (8 voxels), mesh time the median of 200, Apple M2 Max,
Release build.

| Fixture | Cells marched | Bricks over the box | Mesh ms (median, n=200) |
|---|---|---|---|
| r 0.5 sphere carved by an r 2.0 cutter, as ITEMS | 9,640,000 -> 125,000 (77.1x) | 20,956 -> 512 (40.9x) | 42.18 -> 5.26 (8.0x) |
| the same, cutter as a composed LAYER | 9,640,000 -> 125,000 (77.1x) | 20,956 -> 512 (40.9x) | 40.14 -> 5.21 (7.7x) |
| two unit spheres 1.2 apart, intersected | 1,600,000 -> 400,000 (4.0x) | 4,116 -> 1,176 (3.5x) | 14.30 -> 9.63 (1.5x) |
| the same two spheres, unioned (control) | 1,600,000 -> 1,600,000 | 4,116 -> 4,116 | 37.97 -> 38.00 |

The time ratio is well below the cell ratio because the mesher already skips
empty space cheaply; the brick count is the figure a host planning over
`clay_tape_info`'s box pays in full. Triangle counts agree to 0.1% (67,476
against 67,548 for the carve): the lattice origin moves with the region's
minimum, so the two regions sample the same surface at different phases. With
`resolution` rather than `voxel_size`, the same narrowing buys detail instead of
speed: the carve is meshed at a 77^(1/3) ≈ 4.3x finer voxel for the same cell
count. A document with neither a subtract nor an intersect, and no smooth group,
keeps its box to the cell.

## What building it found

- **The gate had to come first, and it is why the task was blocked.** Narrowing
  one side of a layer-versus-item comparison breaks the parity 3.3 stands on, and
  the only parity test never crossed the C ABI. `test_c_layer_group_parity.cpp`
  builds every document through `clay.h`; making
  `clay_document_set_layer_composition` write the op onto each root item fails 10
  of its 11 compositions and the order case. The hard union is the one it cannot
  see, correctly: flattening a hard union changes nothing.
- **"A flat chain subtracts twice" is only true under a smooth blend.** The first
  draft asserted the flat spelling A, B(Subtract), C(Subtract) differs from the
  group for every composition and it failed for the hard ones:
  max(max(a, -c1), -c2) IS max(a, -min(c1, c2)). The teeth are asserted where a
  blend or mode radius makes the forms different, and the mutation is what shows
  the gate reaches the binding.
- **The group ring could be closed here, and had to be.** compile_group recorded
  that a smooth group's own ring was missing from `tape.bounds`, and that it could
  not be added because a resume unwinds frames that carried no extent. Narrowing
  needs exactly that extent (an intersecting group is bounded by the chain OUTSIDE
  it), so each frame now carries `outer_bound`, and the ring came for free through
  the same rule. The C ABI gate therefore compares extents for EVERY composition,
  where before it could only compare them for the ones with no support.
- **Intersect dilates its right operand by the ring although the kernel does not
  need it.** -smin(-a, -b) >= max(a, b) for every profile, so the overlap of the
  UNDILATED boxes is sound. But an item's geometry bound carries its own combine's
  support whatever the op, and the first build reported a tighter box for a
  smooth-intersecting LAYER than for the same intersect spelled on an ITEM; the
  parity test caught it. Parity costs the ring's width on an intersect.
- **The plain union still has readers.** A transition's field info bounds
  |d1 - d2| by the diagonal of the region both surfaces live in, and read
  `tape.bounds` mid-compile. Narrowing that box would have raised a safe step
  nobody asked to change, so the old union survives as the compiler's `reach_`
  and in `TapeCheckpoint::reach` for an append, and `compile_layer_suffix` keeps
  reporting the appended items' union, which is what its consumer culls against.
- **A disjoint intersect keeps its left operand's box rather than an empty one.**
  The field has no material there, but an empty box on a non-empty tape reads as
  "unbounded" to every consumer and `clay_document_mesh` refuses it. The same
  harmless direction is kept for an empty intersecting LAYER.
- **Not narrowed, deliberately:** paint, relief, incise and the morphs. Paint and
  incise are provably inside their left operand too (the distance is `a`, or
  `a + k w`), but the task named subtract and intersect and each extra row is one
  more thing a too-tight bug can hide in; they are left as wide as they were.

# Proposal: sculpt at more than one resolution

## Why

`VoxelGrid` takes its cell size in the constructor and never lets go of it:

```cpp
explicit VoxelGrid(float voxel_size = 0.1f) : voxel_size_(voxel_size) {}
float voxel_size() const { return voxel_size_; }
```

There is no resample, no resize, no subdivide, and no adaptive refinement
anywhere in `voxel/`, `mesh/` or `brick/`. The brick cache is a sparse *narrow
band* — it stores only the bricks the surface crosses, which is a memory
optimisation over one uniform lattice, not a level-of-detail hierarchy.

So a sculptor has to choose the finest detail in the model before making the
first stroke, and then pay for it everywhere. A helmet whose cable ribs need
2 mm cells is a helmet whose entire skull is at 2 mm. Worse, the choice is
final: there is no way to add detail to a region later, and no way to work
coarse while the forms are still moving. The only recourse is to rebuild at a
finer size and redo the work.

This removes the loop that sculpting is actually made of — **block out coarse,
subdivide, refine, repeat**. Every package has it (ZBrush subdivision levels,
Mudbox, Blender's multires) because form and detail are different activities:
big shapes want to be cheap to push around, and small ones want resolution
only where they are.

It is also the ceiling on the examples. `35_hard_surface_helmet.py`'s vent
louvers and cable ribs are as coarse as they are for this reason, and no brush
that could be added would lift it.

## Why this ranks above the other sculpting gaps

The three sculpting proposals already open — SDF verbs, SDF masking, exposed
groups — are all additive: each bolts on beside what exists. Multi-resolution
is not. It changes what a grid *is*, which means it touches storage,
serialisation, every verb's footprint walk, meshing, and the C ABI's notion of
a grid handle.

That is an argument for doing it **first**, not later. Retrofitting a
resolution hierarchy under verbs, a file format and an ABI that all assume one
uniform cell size is materially harder than building them on top of one.

## Approach — and the decision that has to be made first

There are two shapes this can take and they are not interchangeable. The
proposal exists to force that choice before code is written.

**A. Discrete levels (the ZBrush model).** The grid holds a stack; level *n+1*
has half the cell size. A stroke lands on the active level; moving down
averages, moving up interpolates and replays finer levels as offsets. Detail is
preserved across level changes, which is the property that makes the workflow
work. Costs: the stack multiplies memory, and every verb must state which level
it acts on.

**B. Adaptive refinement (the octree model).** One structure, refined only
where the surface needs it. Memory follows the surface rather than the bounding
box. Costs: neighbour lookups stop being O(1) array indexing, which every verb's
footprint walk currently assumes, and the mask/dither machinery is written
against a uniform lattice.

**A is the better fit for this codebase**, and the reason is the dither. Every
falloff brush resolves sub-unit strength by hashing the *cell coordinate*, and
that hash is what makes strokes reproducible across platforms and backends. A
uniform lattice per level keeps that property exactly as it is; an adaptive
structure changes what a "cell coordinate" means and puts the reproducibility
guarantee — which the parity suite enforces — in question.

This proposal therefore recommends A, and asks for the decision to be recorded
either way before implementation.

## Open questions

- **Serialisation.** `.clayspace` stores one grid per voxel layer. Does it
  store every level, or the coarsest plus offsets? The format is chunked and
  backward-open, so a new chunk is available; the size cost is the question.
- **Which level a verb acts on**, and whether a verb may act across levels.
- **Whether meshing picks a level** or always meshes the finest.
- **How the brick cache interacts.** It is already a sparse narrow band around
  one lattice; with levels, whether it caches per level or only the finest is a
  real design question rather than a detail.
- **What happens to a mask across levels.** Masks are addressed in world units
  precisely so they survive a resolution change — that property should make
  this easier, and should be confirmed rather than assumed.

## Impact

`voxel-engine` gains the level concept and every verb gains a statement about
which level it acts on. `brick-cache` and `meshing` need to say how they choose.
`file-io` gains persistence. `c-abi` and `python-bindings` gain the surface.
This is the largest change proposed so far and it is not additive: existing
behaviour must be defined as "the single-level case" rather than left implicit.

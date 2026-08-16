# Design: sculpt at more than one resolution

## The decision the proposal asked for

**Discrete levels (A), not adaptive refinement (B).** Recorded here because the
proposal exists to force the choice before code is written.

The reason the proposal gives is the dither, and it holds up against the code.
`cell_threshold` (`src/voxel/sculpt.cpp`) mixes `c.x`, `c.y` and `c.z` as
integers with no floating point until the final divide, and `passes()` compares
a falloff weight against it. That is the only thing standing between a soft
brush and a different result per platform, and the parity suite enforces the
guarantee. It is reachable from exactly one place — `for_each_brush_cell`,
which every verb goes through — so the property is cheap to keep and easy to
lose.

A stack of uniform lattices keeps it untouched: each level has its own integer
cell coordinates and the hash sees the same kind of key it always did. An
adaptive structure would have to define what a cell coordinate is for a cell
whose size depends on where it is, and would additionally break the O(1)
neighbour indexing that `Region`/`snapshot` and every verb's footprint walk
assume. Two load-bearing properties for one structural change was not a trade
worth taking.

## Storage

`VoxelGrid` holds `std::vector<Level>`. Level 0 is the coarsest and level k has
half the cell size of level k-1. One level is **active**, and every existing
method acts on it — the level is grid state rather than a parameter. That is
what keeps the C ABI purely additive: no existing signature changed, and a
caller that never mentions a level has a one-level grid behaving exactly as
before.

Each level above 0 also carries a `detail` map: exactly the cells whose value
differs from the cell above them in the coarser level. It is maintained eagerly
on every write, so it is always the canonical difference rather than a cache
that can go stale.

## Propagation, and why detail survives

A write at the active level does three things:

1. writes the cell,
2. **down**: recomputes each coarser ancestor by averaging its eight children
   (occupied when at least four are, coloured by the commonest), then restates
   every child's offset against the parent that just moved,
3. **up**: rewrites each finer descendant as *its stored offset if it has one,
   otherwise its parent's value*.

Step 3 is the whole feature. A broad stroke at a coarse level reaches every
finer level, and the cells that were sculpted finer keep their own value
because they are offsets rather than derived. An offset the coarse form has
caught up with is dropped, so the map stays the size of the detail.

The consequence to know about: a coarse *erase* under fine detail leaves the
detail standing, because an offset is not something a coarser level can address.
That is the stated requirement ("detail survives a coarse edit") rather than an
oversight, and it matches what a multires workflow does elsewhere — you go to
the level the detail lives on to remove it.

Cost: a write costs 8^d cell writes for d levels finer than the active one.
Editing coarse with a deep stack is therefore not free, but it is the same work
a full resync would do and it is charged to the edit that caused it.

## Serialisation

`VoxelGrid::serialize()` opens with the **coarsest** level in exactly the layout
it always had, then appends a tagged tail carrying only the per-level offsets —
everything else is reproducible by subdividing. Two properties fall out:

- A one-level grid writes no tail, so its bytes are byte-for-byte what they
  were. This is pinned by a golden hash taken from a pre-change build.
- A reader that predates the tail stops after the chunk records and opens the
  document at the coarsest level rather than failing, which is what the
  backward-open rule asks for. No new `.clayspace` chunk was needed to get
  that, and putting it in the grid stream means a standalone grid serialised
  through the C ABI keeps its levels too — a sibling chunk would have lost them.

`.clayspace` minor moves to 6 (with `kSceneMinor`, which a static_assert binds
to it) because the container's content changed, even though the scene payload
did not.

## What the other capabilities do

- **Meshing** picks a level explicitly: `mesh_greedy(level)`. The no-argument
  form meshes the active level, which for a one-level grid is what it always
  did.
- **Brick cache**: nothing to do. It is an SDF-side structure keyed on a
  `BrickConfig` lattice and it never sees a `VoxelGrid` — the rule is that it
  caches the lattice it was configured with, and a voxel level change neither
  dirties nor invalidates it. Its `build_mip`/`find_mip` LOD is a separate
  mechanism for a separate representation and stays that way.
- **Masks** need no change, and the proposal's assumption is confirmed:
  `MaskField` is addressed in world units on its own lattice, and voxel edits
  sample it through `mask_at(mask, cell, voxel_size)` in the one shared
  footprint walk. Since the walk is handed the active level's cell size, the
  same world region is selected at every level. Tested rather than assumed.

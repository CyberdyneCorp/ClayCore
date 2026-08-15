# Tasks: add-multi-resolution

- [x] 1.1 DECIDE and record: discrete levels vs adaptive refinement. The proposal recommends levels because the brush dither hashes a cell coordinate, and platform-reproducible strokes are enforced by the parity suite — recorded in `design.md`, with the reasoning checked against `cell_threshold`
- [x] 1.2 Level stack on `VoxelGrid`: add a level, drop one, query the count and the active one
- [x] 1.3 Down-sample (average) and up-sample (interpolate) between adjacent levels, preserving finer detail as offsets so a round trip through a coarser level is not destructive
- [x] 1.4 Every verb states which level it acts on; the shared footprint walk carries it, so no verb can forget — carried as grid state, which is what keeps the ABI additive
- [x] 1.5 Meshing chooses a level explicitly rather than implicitly
- [x] 1.6 Brick cache: decide and state whether it caches per level or only the finest — it never sees a `VoxelGrid`, so it caches the lattice it was configured with; stated in `design.md` and in the delta spec
- [x] 1.7 `.clayspace`: backward-open so an older reader opens the document at the coarsest level rather than failing — done as a tagged tail inside the existing `VOXL` stream rather than a new chunk, so a standalone grid keeps its levels too
- [x] 1.8 Both bindings, C ABI additive; a grid built with one level behaves exactly as today
- [x] 1.9 Tests: a stroke at a coarse level survives a trip to fine and back; detail added at fine survives an edit at coarse; masks addressed in world units still select the same region at every level; the dither stays reproducible per level; single-level grids are bit-identical to today (pinned by a golden hash from a pre-change build)
- [x] 1.10 Example: block out coarse, subdivide, add detail only where it is needed, and report the memory each level costs
- [x] 1.11 Bound what a level tail may cost the reader. Storing only offsets makes the tail small however deep the stack it names, so a 220-byte file claiming the maximum depth over a 64-cell coarsest level asked for 8^15 cells and `deserialize` did not return. The declared depth is now charged against the content the file supplied before any level is built — regression test in `test_voxel_levels.cpp`

## Not done

- **Up-sampling does not interpolate — and WILL NOT.** Occupancy is binary, so
  subdivision splits a cell into eight children with the same index. There is
  nothing to interpolate except by inventing a smoothed boundary, and that is a
  remesh rather than an up-sample: it would move the surface, breaking "adding a
  level cannot move the surface", which is documented and tested.

  The complaint behind it — a subdivided surface is blockier than one authored
  at the fine level — is answered at DISPLAY time instead: `mesh_smooth` runs
  surface nets over the same occupancy and rounds corners without erasing thin
  features (#108). That is the right place for it, because it changes the
  picture without changing what the grid stores.
- ~~**Only whole-grid subdivision.**~~ **Answered by `refine-a-region`.** A level
  can now be refined over a region: outside it the level has no storage and
  reads its parent's value, so the lattice stays uniform and complete and only
  what is STORED changes. Refinement is at chunk granularity, and a write
  outside the region refines what it touched. The original cost stood as
  written — measured at exactly 8x per level over the occupied volume, on the
  same three-primitive form `docs/09` uses.
- **No level-aware SDF bridges — and DELIBERATELY.** `rasterize_tape` and
  `sample_step_field` act on the active level, so rasterising into a stack fills
  one level and averages/replays into the others.

  Sampling each level at its own cell centres is more faithful and costs the
  invariant this stack is built on: a coarse level is the DOWNSAMPLE of the fine
  one. Break it and `drop_level` starts changing the solid, and the detail map —
  exactly the cells that differ from their parent — grows toward every cell,
  taking the serialised size with it.

  The gap is narrower than it reads, too: setting the active level and
  rasterising already gives THAT level its true sampling. What is unavailable is
  every level true at once, which is also the case that costs the most storage.
- **No brick-cache work.** Stated rather than built, because the cache does not
  touch `VoxelGrid` at all.
- [ ] 1.1 DECIDE and record: discrete levels vs adaptive refinement. The proposal recommends levels because the brush dither hashes a cell coordinate, and platform-reproducible strokes are enforced by the parity suite
- [ ] 1.2 Level stack on `VoxelGrid`: add a level, drop one, query the count and the active one
- [ ] 1.3 Down-sample (average) and up-sample (interpolate) between adjacent levels, preserving finer detail as offsets so a round trip through a coarser level is not destructive
- [ ] 1.4 Every verb states which level it acts on; the shared footprint walk carries it, so no verb can forget
- [ ] 1.5 Meshing chooses a level explicitly rather than implicitly
- [ ] 1.6 Brick cache: decide and state whether it caches per level or only the finest
- [ ] 1.7 `.clayspace`: a new chunk, backward-open so an older reader opens the document at the coarsest level rather than failing
- [ ] 1.8 Both bindings, C ABI additive; a grid built with one level behaves exactly as today
- [ ] 1.9 Tests: a stroke at a coarse level survives a trip to fine and back; detail added at fine survives an edit at coarse; masks addressed in world units still select the same region at every level; the dither stays reproducible per level; single-level grids are bit-identical to today
- [ ] 1.10 Example: block out coarse, subdivide, add detail only where it is needed, and report the memory each level costs
- [ ] 0.1 SEQUENCING (see ROADMAP, "What can run in parallel"): runs FIRST and alone among the VoxelGrid changes; add-sculpt-layers and add-representation-round-trip both wait on it
- [ ] 0.2 This change takes `.clayspace` minor **6**. The minors are assigned in the
      roadmap rather than taken first-come, because three open changes each add a chunk and
      two bumping independently yields a document claiming one minor while carrying one
      feature. Bump `kClaySpaceMinor` and `kSceneMinor` together — a static_assert binds them


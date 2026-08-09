# Tasks: add-multi-resolution

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

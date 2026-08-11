# Tasks: emit-subset-boundary-straddlers

## 1. Mesher (issue #66)

- [x] 1.1 Generalize `march_tet` / `march_cells` over the emission sink, so a shell cell can be marched into a collector without touching the mesh builder
- [x] 1.2 Shell pass in `mesh_bricks`: enumerate the one-cell ring around the requested set, owned by unrequested surface bricks, deduplicated and in deterministic (lexicographic) order
- [x] 1.3 Keep the shell triangles with at least one corner inside a requested brick's closed box; attribute each to the lexicographically lowest requested key owning a corner, using the builder's own interpolation arithmetic so classification agrees with the emitted vertex
- [x] 1.4 Emit each key's straddlers inside its range, after its own cells, through the same welding builder — ranges still partition, seam vertices still weld
- [x] 1.5 The whole-surface path (`keys == nullptr`) skips the shell pass entirely and stays byte-identical

## 2. Docs

- [x] 2.1 `clay.h`: the subset returns every whole-mesh triangle with a corner in a requested brick; the attribution rule; the per-brick dedupe a host owes
- [x] 2.2 `clay_brick_mesh_range`: a key's ranges carry its own cells' triangles first, then its attributed straddlers
- [x] 2.3 `include/clay/mesh/marching.h`: the same contract on `mesh_bricks`

## 3. Tests

- [x] 3.1 Regression, the issue's measurement: one relief dab, subset(dirty keys) vs whole — triangles with all three corners inside match, triangles with at least one corner inside match (N-missing becomes 0-missing, 0 spurious), and the subset invents nothing
- [x] 3.2 Regression, the claim the straddlers exist for: a per-brick host loop (replace each dirty key's share from the subset's ranges, over several dabs) equals a full rebuild as a set of triangles
- [x] 3.3 Update the union-of-singles and adjacent-pair tests to compare as sets: a straddler touching two requested keys is deliberately in both single-brick meshes

# meshing — march a level of the brick cache

Delta for `mesh-brick-cache-lod` (#93).

## ADDED Requirements

### Requirement: The brick mesher marches a level
The brick mesher SHALL take the LEVEL to march, defaulting to the full-resolution one so that every existing call site is unaffected. A mip is the cache's own lattice with every second point kept, so a level SHALL change exactly two things — the lattice spacing doubles per level, and the keys are the coarse block keys — and SHALL change nothing about the marching, the seam welding, the per-key ranges or the straddler attribution.

Straddler collection SHALL test the neighbouring bricks AT THE SAME LEVEL, so a subset stays a filter of the whole mesh at that level rather than borrowing cells from another one.

A level the cache cannot hold SHALL produce an empty mesh rather than the nearest level it can.

Field attributes — colours and gradient normals — SHALL be applied at the full-resolution level only. They are evaluated through per-brick culled tapes whose exactness rests on a vertex sitting on the field's surface, and a coarse vertex sits on the mip's; the attributes would be silently approximate. Face normals are derived from the triangles and are unaffected.

#### Scenario: The coarse mesh is the same surface, coarser
- **WHEN** a filled cache whose mips are built is meshed at level 0 and at level 1
- **THEN** both meshes describe the same surface — bounds agreeing to within one coarse cell — and the coarse one carries substantially fewer triangles

#### Scenario: A level changes no other behaviour
- **WHEN** the whole-cache and key-subset paths are meshed at level 0 after the level parameter exists
- **THEN** the output is identical, vertex for vertex and index for index, to what the mesher produced before it existed

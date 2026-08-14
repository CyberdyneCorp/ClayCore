# voxel-engine — a sculpt exports as quads

Delta for `quad-mesh-export`.

## ADDED Requirements

### Requirement: A voxel grid meshes to quads in two modes
`VoxelGrid` SHALL provide a quad mesher offering two modes, because a voxel sculpt is two different subjects depending on what the user made.

**Dual mode** SHALL be the lattice dual over the same occupancy field `mesh_smooth` builds — the rounded form, quads meeting four to a vertex on average — generalised to a lattice cell size other than the grid's voxel size by sampling that occupancy TRILINEARLY.

At the grid's own voxel size with no blur the sampler reads exactly the values `mesh_smooth` reads, so dual mode SHALL return `mesh_smooth`'s mesh vertex for vertex and index for index, differing only by the quad array. This identity is what keeps the two on one code path rather than two that drift.

A cell size COARSER than a voxel low-passes the occupancy and CAN drop a one-voxel-thick feature entirely — the same failure `blur` already carries, for the same reason, and stated in the same place. A cell size FINER than a voxel resamples the same step field: it adds quads and no detail, so the count search SHALL clamp there and report that it clamped.

**Faces mode** SHALL emit one planar, axis-aligned quad per exposed voxel face — the greedy sweep with merging switched off. It SHALL NOT change `mesh_greedy`, whose merged output and its existing per-chunk guarantees are untouched.

Faces mode SHALL WELD its corners, keyed by lattice corner AND palette index. Today's `emit_quad` pushes four fresh vertices per face, so a greedy mesh arrives in a DCC as disconnected rectangles; welding makes it a connected quad grid within a colour region, and splits it at a colour boundary so per-face palette colour survives — which is why those vertices were duplicated in the first place.

Faces mode SHALL NOT emit vertex normals. A welded corner is shared by faces pointing three ways and has no single normal; averaging would round a cube and duplicating would undo the weld. The quads are planar, so a consumer derives the face normal from the face. A caller who needs per-face normals uses `mesh_greedy` and gets triangles, as today.

Faces mode has no cell size — its lattice is the grid. Its count lever SHALL be the multi-resolution LEVEL the grid already carries, so its granularity is roughly a factor of four per step, and a requested target SHALL pick the nearest level. This SHALL be stated, because a caller who asks for fifty thousand quads and receives twelve thousand needs to know a level was chosen rather than a bug hit.

The faces search SHALL be a WALK of that stack and not a search over a cell size that is then rounded to a level: it meshes the coarsest level first, stops at the first level whose count reaches or passes the target, and returns the nearer of the two levels it landed between. The count rises with every level, so no later level can be nearer. The walk SHALL mesh each level at most once, and it starts at the coarsest, so it costs one mesh for every level up to and including the one that stops it: a target met at level k costs k+1 meshes, which is what `iterations` SHALL report. A caller budgeting for this SHALL be told to price the stack's length and not the bracketing pair, because the levels below the bracket were meshed to reach it. `max_iterations` SHALL NOT apply to it: a stack is its own bound, and because each level holds about a quarter of the next one's faces, walking the whole stack costs about a third more than meshing its finest level alone — which a target above every level buys in any case.

With NO target the count search SHALL be the plain quad mesher with a report attached, in both modes and for every input the plain mesher takes. A level the grid does not have SHALL therefore come back as an EMPTY mesh and a zeroed report, as the plain mesher already answers it, rather than be clamped onto the finest level — clamping would hand a caller who passed a stale level a full-resolution mesh and a report naming a level they did not ask for.

`clamped` in faces mode SHALL mean THE STACK RAN OUT — the target is below what the COARSEST LEVEL THAT YIELDS ANYTHING gives, or above what the finest yields. The qualifier is normative, not prose: the stack is not a strict mip, a sculpt made only at a fine level leaves the coarse levels EMPTY, and an empty level meshes to zero quads, which is below every target without being a level the caller can be handed. Reading the coarse end off level 0 would therefore report a grid whose every usable level overshoots as if it bracketed the target. A level whose count EQUALS the target SHALL NOT be reported as clamped — nothing overshot and nothing ran out. A target falling BETWEEN two levels SHALL NOT be reported as clamped even when the level returned is far from it; that is what `within_tolerance` false says. The documented meaning of the flag and the search that sets it SHALL be the same thing, because this feature's premise is that a caller learns the behaviour from the documentation rather than from the mesh.

#### Scenario: A faces target inside the stack brackets it and is not clamped
- **WHEN** a multi-level grid is asked in faces mode for a count between two levels' counts
- **THEN** both bracketing levels are meshed, the nearer one is returned and named in the report, and `clamped` is false
- **AND** `iterations` is the stopping level's index plus one, counting every level meshed from the coarsest, rather than the two levels of the bracket

#### Scenario: A faces target outside the stack reports the stack running out
- **WHEN** a multi-level grid is asked in faces mode for a count below what its coarsest level yields, or above what its finest level yields
- **THEN** the end level is returned and `clamped` is true, and a stack longer than `max_iterations` still reaches its end

#### Scenario: An empty coarse level is not the coarse end of the stack
- **WHEN** a grid sculpted only at a fine level — its coarse levels empty — is asked in faces mode for a count below what its first non-empty level yields
- **THEN** that level's mesh is returned and `clamped` is true, because no level of this grid is nearer
- **AND** the same grid asked for exactly that level's count reports `clamped` false and `within_tolerance` true

#### Scenario: An untargeted fit at a level the grid does not have is empty
- **WHEN** a grid is quad-meshed with no target at a level index beyond its stack, in either mode
- **THEN** the mesh is empty and the report is zeroed, matching what the plain quad mesher returns for the same options

#### Scenario: Dual mode at the grid's own resolution is the smooth mesh
- **WHEN** a sculpt is quad-meshed in dual mode at the grid's voxel size with no blur
- **THEN** the positions and triangle indices are identical to `mesh_smooth`'s, and the mesh additionally carries one quad per two triangles

#### Scenario: Faces mode is one quad per exposed face
- **WHEN** a solid block of voxels is quad-meshed in faces mode
- **THEN** the output carries exactly one quad per exposed voxel face, every quad is planar and axis-aligned, and the quads cover exactly the surface `mesh_greedy` covers over the same cells

#### Scenario: Faces mode welds within a colour and splits across one
- **WHEN** a two-colour slab is quad-meshed in faces mode
- **THEN** faces of the same palette index sharing a lattice corner share one vertex, and the corner on the boundary between two palette indices appears once per colour

#### Scenario: Greedy meshing is untouched
- **WHEN** `mesh_greedy` and `mesh_greedy_chunks` run after faces mode exists
- **THEN** their output is identical, vertex for vertex and index for index, to what they produced before it

#### Scenario: A cell finer than a voxel is clamped
- **WHEN** dual mode is asked for a cell size below the grid's voxel size
- **THEN** it meshes at the voxel size and reports that it clamped, rather than spending the quads on detail the grid does not hold

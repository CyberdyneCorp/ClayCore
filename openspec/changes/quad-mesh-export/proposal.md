# Proposal: keep the quads the meshers already make

## Why

**Two meshers in this tree build quads and then throw them away.**

- `VoxelGrid::mesh_greedy` merges runs of voxel faces into axis-aligned
  rectangles and `emit_quad` splits each one into two triangles
  (`src/voxel/grid.cpp`). The rectangle is the whole point of greedy meshing
  and it survives exactly long enough to be triangulated.
- `detail::dual_grid_mesh` — behind `mesh_lattice_nets` / `mesh_tape_nets`, and
  behind `VoxelGrid::mesh_smooth` for voxels — emits **one quad per
  sign-changing lattice edge** and then writes six indices for it
  (`src/mesh/dual_grid.h`).

`mesh::Mesh` has nowhere to put them: `indices` is documented as a triangle
list and `triangle_count()` divides by three, so the four corners the mesher
had in hand are unrecoverable by the time anything downstream sees the mesh.
The exporters follow: OBJ writes `f a b c`, PLY declares `element face
triangle_count`, FBX writes three-index polygons — all three formats carry
polygons natively and all three are handed triangles because that is all they
are given.

The user-facing consequence is that a sculpt exported from this library lands
in a DCC as a triangle soup. Quads are what a subdivision surface, a
sculpting app's dynamic topology and most modelling tools want, and the
information needed to give them quads is computed and discarded on every
export.

## What

`mesh::Mesh` gains an **optional quad index array parallel to `indices`**, two
meshers learn to fill it on request, three exporters learn to write it, and a
quad count becomes something a caller can aim at.

1. **Storage.** `Mesh::quads` — four indices per face, empty on every mesh
   produced today. When it is non-empty, `indices` still holds exactly the
   triangulation of those same quads, so **every existing consumer — decimation,
   BVH, validation, all four exporters, the C ABI, the mesh stream — keeps
   working byte-identically and needs no change at all.** The quad array is an
   addition to what a mesh carries, never a replacement for what it already
   carried.
2. **The mesher.** The **lattice-dual** path (surface nets) is the quad source,
   for the SDF side and the voxel side alike, because its quads meet four to a
   vertex and it has no T-junctions. Greedy meshing gets an **unmerged**
   quad mode for the voxel-art case, where the boxes are the intent.
3. **The count.** The lattice cell size is the only lever, so a requested quad
   count becomes a short search over cell size that reports what it actually
   landed on. It lands NEAR a target, never on it.
4. **The formats.** OBJ, PLY and FBX write quads. **glTF/GLB keeps
   triangulating, because glTF 2.0 has no quad primitive mode** — that is
   stated in the header, the spec and the example rather than discovered in
   Blender.
5. **Both sources.** A voxel sculpt and an SDF document both quad-mesh, through
   `clay_voxel_mesh_quads` and `clay_document_mesh_quads` and their Python
   counterparts.

## What this is NOT — read this before using it

**This produces a REGULAR QUAD GRID DERIVED FROM A LATTICE. It is not
field-aligned retopology.**

The quads follow the sampling lattice, not the form. There are no edge loops
running around a limb or a mouth, no poles placed where the surface wants
them, no denser rows where curvature asks for them, and nothing here is
animation-ready — deforming this mesh will pinch wherever the topology
disagrees with the shape, which is everywhere.

This is the input a retopology pass REPLACES, not the output one produces. A
user reaching for this expecting ZRemesher, QuadRemesher or Instant Meshes
output must find that out from the header, the spec, the docstring and the
example — not from the result. Every one of those places states it.

What it IS good for: getting quads into a DCC that prefers them, subdividing a
sculpt, and exporting a voxel model as the box faces it actually is.

## What else this is not

**Not a change to any existing call.** No mesher starts producing quads on its
own — every quad path is a new entry point. `mesh_greedy`, `mesh_smooth`,
`mesh_tape`, `mesh_tape_nets` and `clay_document_mesh` return exactly the bytes
they return today, which is the constraint the storage decision is built
around.

**Not a quad importer.** OBJ and PLY readers keep fan-triangulating what they
read; a quad file re-imported comes back as triangles. The readers already have
the face list in hand, so preserving it is a cheap follow-up — but it is a
second direction with its own budget and validation questions, and mixing it
in here would double the surface for a feature nobody has asked for yet.

**Not decimation.** Quadric edge collapse is a triangle operation and destroys
the pairing; a decimated mesh drops its quads. The cell-size search is the
count lever, and it is the only one.

**Not a manifold guarantee.** The dual mesher pinches where a cell is crossed
twice, exactly as its header already says, and a voxel grid can expose faces
that meet only along an edge. Marching cubes remains the watertight,
2-manifold triangle export path, and that stays true.

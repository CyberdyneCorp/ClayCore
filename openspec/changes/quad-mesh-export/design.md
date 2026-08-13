# Design: quad meshing and quad export

The decisions and what was rejected. Written before the code because three of
them are visible outside this repository once they ship: a struct every
consumer includes, a file format's bytes, and a contract about a number a
caller asks for and does not get exactly.

## 1. Which mesher gives clean quads: the lattice dual

Three candidates exist in the tree already.

| candidate | quad shape | topology | density | verdict |
|---|---|---|---|---|
| greedy MERGED rectangles (`mesh_greedy` today) | planar, axis-aligned, wildly varying size | **T-junctions**: one long quad abuts several short ones along a merged run | lowest | **rejected as the quad path** |
| greedy UNMERGED (one quad per exposed voxel face) | planar, axis-aligned, uniform | no T-junctions; non-manifold where voxels touch only along an edge | very high, ~area/voxel² | **taken as a second mode**, voxels only |
| **lattice dual** (surface nets, `dual_grid_mesh`) | **non-planar**, roughly uniform | four quads to a vertex, no T-junctions; **not manifold** at a twice-crossed cell | tunable by cell size | **taken as the default** |

**Why the merged rectangles lose.** A T-junction is a vertex sitting in the
middle of another face's edge. Subdivision cracks there, normals split there,
and most DCC cleanup tools will either weld it or leave a visible seam. Greedy
merging produces them by construction — that is what merging a run of faces
against a wall of shorter runs means. The result is quads, but it is not a
quad mesh anybody can subdivide, and "clean" in the ask means "survives the
next operation".

**Why the dual wins.** Every quad comes from one sign-changing lattice edge and
its four corners are the four cells around that edge, so interior vertices have
valence four everywhere and no vertex ever lands mid-edge. Quad count scales
directly and predictably with cell size, which is what makes decision 3
possible at all. The mesh is already computed this way — `dual_grid_mesh`
literally builds `std::uint32_t quad[4]` and then writes six indices from it.

**Its two real defects, stated rather than papered over.**

- **The quads are not planar.** Four cell vertices around an edge are placed
  independently; nothing makes them coplanar. Renderers and DCCs triangulate
  non-planar quads on their own terms, so the same face can shade differently
  in two applications. We do not attempt to planarise: moving vertices to
  flatten faces moves them off the surface, which trades a shading artifact for
  a geometric error.
- **It is not manifold.** A cell the surface crosses twice still gets one
  vertex, pinching the two sheets. `surface_nets.h` already says this and
  `mesh_smooth`'s docstring already says this; the quad path inherits it and
  repeats it. Marching cubes stays the watertight/2-manifold export path.

**Why unmerged greedy is kept as a mode.** For a voxel model the boxes ARE the
subject — hard-surface work, voxel art, anything where the merged rectangles'
irregularity is worse than the density. One quad per exposed face is uniform,
planar, axis-aligned, and trivially correct. It is dense, and that is the
honest trade.

**Rejected outright: quadrangulating a marching-cubes mesh.** Pairing triangles
(Blossom-quad) or a mixed-integer parameterisation would give better quads and
would be the beginning of actual retopology. It is a different feature with a
different order of magnitude of work, and shipping half of it under this name
would be exactly the confusion the honesty rule exists to prevent.

## 2. How `Mesh` carries quads: a parallel array, triangles untouched

```cpp
struct Mesh {
    // ... positions, normals, colors, uvs unchanged ...
    std::vector<std::uint32_t> indices;  // triangle list — unchanged
    std::vector<std::uint32_t> quads;    // empty, or 4 indices per quad
};
```

**The invariant, which is the whole design:** when `quads` is non-empty,
`indices` holds exactly the triangulation of those quads in order — quad `q`'s
corners `(a, b, c, d)` are triangles `(a, b, c)` and `(a, c, d)` at
`indices[6q .. 6q+5]` — over the same `positions`. So `indices.size() ==
quads.size() / 4 * 6`, and any consumer that ignores `quads` sees a complete,
correct triangle mesh, the same one it would have seen before this change.

| option | verdict |
|---|---|
| **parallel `quads` array, `indices` keeps the triangulation** | **taken** — no existing consumer changes, no exporter changes, no C accessor changes; quad-aware code opts in by reading one more array |
| a topology enum and `indices` reinterpreted as 4-tuples | rejected — `triangle_count()` silently returns nonsense, every exporter, the BVH, the mesh stream and every C consumer must branch or break. Fails the "byte-identical for existing callers" requirement on its first line |
| a separate `QuadMesh` type | rejected — duplicates five attribute arrays, four exporters, a C handle and a Python class, and every consumer needs a converter. The two representations of one mesh have to travel together anyway; two types make that the caller's bookkeeping |
| a general per-face vertex-count array (PLY's face-list shape) | rejected — nothing in this library produces an ngon. Every reader would branch per face, and a vector of 4s costs memory to say "still quads" |

**Cost.** Four extra `uint32` per quad against the six already stored for its
triangles: +67% index memory, paid only by a mesh that asked for quads. Nothing
else in the struct moves, and a mesh without quads costs one empty vector
header.

**Why not derive the quads from the triangles instead of storing them.**
Recovering pairs from a triangle list means matching shared diagonals and
guessing which pairing was intended; it is ambiguous on a regular grid and
wrong wherever the mesher's diagonal was not the shorter one. The producer
knows; storing it is four bytes a corner.

**The diagonal stays where it is.** `dual_grid_mesh` triangulates on the 0–2
diagonal today. A non-planar quad triangulates better on its SHORTER diagonal,
and we are not switching: that would change the triangles every existing caller
of `mesh_tape_nets` and `mesh_smooth` receives. The quad corners are recorded
in the order the mesher already uses.

**Staleness is the risk this design creates**, and it is named here so the
implementation treats it as a rule rather than a surprise: any operation that
rewrites `indices` MUST clear `quads`, or the invariant is a lie that survives
into a file. Decimation clears them. Transform does not touch indices and keeps
them. Concatenation carries them only when every input has them, rebased —
the same all-or-nothing rule `clay_mesh_concat` already applies to normals,
colours and uvs, and for the same reason. `mesh::quads_consistent()` exists so
tests and the stream reader can assert the invariant rather than trust it.

## 3. Targeting a quad count: a search over cell size, best-effort, reported

For the dual mesher the quad count is, to a good approximation, the number of
sign-changing lattice edges — surface area divided by the square of the cell
size:

```
quads ≈ A / cell²      →      cell' = cell · sqrt(actual / target)
```

That gives a secant step in log–log that converges in two or three meshes from
almost any start. The search meshes, measures, corrects, and repeats up to
`max_iterations` (default 4), keeping the best result seen.

**The contract: a HINT with a REPORTED ACTUAL, not a ceiling.**

| contract | verdict |
|---|---|
| **hint, best-effort, actual reported** | **taken** |
| hard ceiling ("never more than N") | rejected — see below |
| exact count | rejected — the lattice cannot produce one; only a retopologiser can |

A ceiling looks achievable (shrink the cell until you cross under N) and is
not, because **count is not monotonic in cell size**. A finer lattice can
resolve a thin feature the coarser one missed entirely and ADD area, so a
bisection that assumes monotonicity can step past its own bracket. Promising a
bound we cannot hold is worse than reporting the number we got. Where two
candidates are equally close to the target the search prefers the one that does
NOT exceed it, so the common reading of "about this many" usually holds without
being promised.

**The granularity limit, stated plainly.** Because count goes as `cell⁻²`, a 1%
change in cell size moves the count about 2%. Within the iteration cap the
search reliably lands inside roughly ±5–10% of a target and is not expected to
do better; the default tolerance is 10% and a tolerance below about 2% will
usually exhaust the cap and return `within_tolerance = false` with its best
attempt. There is no rounding trick that fixes this — the count is a property of
how many lattice edges the surface crosses.

**Both ends are clamped, and clamping is reported.**

- **Fine end:** the sample ceiling from the existing meshing requirement "A
  mesher prices the grid its resolution implies". The search never asks for a
  lattice the mesher would refuse; it stops at the ceiling and reports
  `clamped`.
- **Coarse end, voxels:** a cell finer than the grid's own voxel buys no
  detail, only quads. The search clamps there and reports it.
- **Coarse end, SDF:** the shape's own topology. Below some cell size a limb
  vanishes and the count collapses; the search returns its best and reports
  that it did not converge, rather than pretending the collapse was the answer.

**Cost.** Every iteration is a full mesh, including a full dense field
evaluation on the SDF path. `max_iterations` is therefore a cost knob and the
default is deliberately small. The header says so, because a caller who sets it
to 20 has asked for twenty meshes.

**Rejected: decimating to the target instead of re-meshing.** Quadric edge
collapse is a triangle operation; it breaks the quad pairing on its first
collapse. There is no quad decimator here and writing one is retopology again.

## 4. Which formats carry quads

| format | quads | why |
|---|---|---|
| **OBJ** | **yes** — `f a b c d`, with the same `v/vt/vn` corner spelling the triangle writer uses | native, universal |
| **PLY** | **yes** — `element face` counts quads, each row `4 a b c d` | the face list is already a variable-length list; the reader here already fan-triangulates one |
| **FBX** | **yes** — four indices per polygon in `PolygonVertexIndex`, last one's complement as the end marker | the writer already emits that structure with three |
| **glTF / GLB** | **NO — triangles, always** | glTF 2.0 defines no quad primitive mode. `mode: 4` is what a conforming file can carry, so the writer keeps writing the triangulation |
| `.clayspace` mesh stream | yes, as an appended tail — see below | a document that holds a quad mesh layer should not lose it on save |
| `MeshBufferView` / `clay_mesh_copy_vertices` | no | that is the GPU readback path and a GPU draws triangles |

**glTF is called out in three places** — `mesh_io.h`, the C header next to
`clay_mesh_save`, and the example — because "I exported GLB and got triangles"
is the single most likely bug report this feature can generate, and it is not a
bug.

**The mesh stream carries quads without a format version bump.** The reader in
`src/io/mesh_stream.cpp` already bounds the declared geometry against the bytes
present with `declared > body` — *"Bounded rather than equal, so a later minor
may append to the stream and this reader skips the tail instead of refusing the
file."* The quad section is exactly that appended tail: `u32 quad_count`
followed by `4 · quad_count` indices, after the triangle indices.

| option | verdict |
|---|---|
| **appended tail, no version change** | **taken** — an older build opens a document containing quad meshes and reads them as the triangles they already are. Nothing is lost that was not already redundant |
| a new attribute-mask bit | rejected — `load_mesh_stream` refuses an unknown mask bit outright, so this would make every older build refuse the whole document to avoid missing an optional decoration |
| bump `kClaySpaceMinor` / `kSceneMinor` | rejected for the same reason — this repository's minors are forward-REFUSED, and refusing a document over a recoverable, redundant section is a worse outcome than dropping the section |

A tail that is PRESENT but malformed — a count that does not fit the bytes, an
index past the vertices, or a quad list that is not the triangulation of the
triangles in the same chunk — refuses the stream as malformed. That matches
what the reader already does with an out-of-range triangle index: a chunk this
library did not write is not trusted to be half right.

## 5. The SDF path and the voxel path

**SDF.** `mesh_tape_quads` is `mesh_tape_nets` with the quads kept: same
lattice, same sampler, same attributes, same closing ring against out-of-range
positive samples. The cell size is the lever the target search moves.

**Voxels, dual mode.** The same lattice dual over the occupancy field
`mesh_smooth` already builds, generalised to a cell size other than the grid's
voxel size by sampling that occupancy **trilinearly**. At the grid's own voxel
size with no blur, the sampler reads exactly the values `mesh_smooth` reads, so
the output must be `mesh_smooth`'s mesh vertex for vertex and index for index,
plus the quad array — that identity is a required test, and the way the
implementation is kept to one code path. A cell COARSER than a voxel low-passes
the occupancy and can drop a one-voxel-thick feature entirely, the same failure
`blur` already has and for the same reason. A cell FINER than a voxel resamples
the same step field and adds quads without adding detail, which is why the
search clamps there.

**Voxels, faces mode.** One planar quad per exposed voxel face — greedy
meshing's sweep with the merge switched off, so `mesh_greedy`'s own output is
untouched and still gated by its existing tests. Two things differ from
`emit_quad` today, both forced by "clean":

- **Corners are welded.** `emit_quad` pushes four fresh vertices per face, so
  today's greedy mesh is a soup of disconnected rectangles that a DCC shows as
  unwelded shells. Faces mode keys vertices by `(lattice corner, palette
  index)`, so a quad grid is connected within a colour region and splits at a
  colour boundary — the compromise that keeps per-face palette colour, which is
  the reason those vertices were duplicated in the first place.
- **No vertex normals.** A welded corner is shared by faces pointing three
  different ways and has no single normal; averaging would round a cube, and
  duplicating for normals would undo the weld. The quads are planar and every
  consumer derives a face normal from them. A caller who needs the per-face
  normals uses `mesh_greedy` and gets triangles, exactly as today.

**Faces mode has no cell size.** Its lattice is the voxel grid. The count lever
is the multi-resolution LEVEL the grid already carries, so the granularity is a
factor of about four per step and a target picks the nearest level. Stated,
because a caller asking for 50,000 quads and getting 12,000 deserves to know it
was a level and not a bug.

**Faces mode is voxels only.** Asking a document for it is refused with
`CLAY_ERROR_INVALID_ARGUMENT` rather than quietly falling back to the dual: a
silent substitution of a smooth mesh for a boxy one is a change the caller can
see in the render and cannot see in the return code.

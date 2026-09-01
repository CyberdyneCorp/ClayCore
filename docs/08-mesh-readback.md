# Reading a mesh back — faces, vertices, normals, colours, UVs

Everything claycore meshes is reachable from outside the library: the vertex
positions, the triangle indices, and whatever attributes the mesh carries. This
document is the worked example in all three languages, plus the two rules that
cost people an afternoon — who owns the memory, and which attributes are
actually there.

If you want the field on your own GPU *without* triangles, you want
[06 — host GPU previews](06-host-gpu-previews.md) instead: the brick atlas and
the tape are the two routes that skip meshing entirely.

## What a claycore mesh is

One interchange type behind all three boundaries
(`include/clay/mesh/mesh_data.h`):

```cpp
struct Mesh {
    std::vector<kernel::cfloat3> positions;
    std::vector<kernel::cfloat3> normals;  // empty or positions.size()
    std::vector<kernel::cfloat3> colors;   // empty or positions.size()
    std::vector<kernel::cfloat2> uvs;      // empty or positions.size()
    std::vector<std::uint32_t> indices;    // triangle list
    std::vector<std::uint32_t> quads;      // empty, or 4 per quad
};
```

Four properties the rest of this document leans on:

- **`indices` is triangles, always.** A flat triangle list, three entries per
  face, each an index into `positions`. There are no n-gons and no face-size
  array to consult; a quad in an imported OBJ was triangulated on the way in.
  A mesh from a QUAD MESHER additionally carries `quads` — but `indices` still
  holds exactly the triangulation of those quads, so nothing that reads
  `indices` has to know. See [quads](#quads) below.
- **Vertices are welded and shared** on the SDF meshers. Two triangles meeting at
  an edge reference the same vertex, which is what makes the marching output
  watertight and 2-manifold by construction. It also means you cannot assume
  vertex `3*t+k` belongs to triangle `t`. (Greedy voxel meshing is the exception
  — see [below](#which-attributes-exist-when).)
- **Attributes are optional but never ragged.** An attribute is either empty or
  exactly as long as `positions`. No call in this library returns a mesh whose
  normals, colours or uvs are non-empty and a different length — the importers
  *drop* an attribute a file supplies for only some of its objects rather than
  padding it. So `normals.empty()` is a complete test; you never have to
  bounds-check per vertex.
- **Winding is outward**, toward the positive side of the field.

<a name="quads"></a>
### Quads

The quad meshers (`include/clay/mesh/quad_mesh.h`, `VoxelGrid::mesh_quads`)
fill `quads` with four indices per face and leave `indices` holding that quad
list's triangulation: quad `q = (a, b, c, d)` is triangles `(a, b, c)` and
`(a, c, d)` at `indices[6q .. 6q+5]`, over the same positions. So
`indices.size() == quads.size() / 4 * 6`, `quads.empty()` is the complete test
for "is this a quad mesh", and every consumer that predates quads — decimation,
the BVH, validation, the exporters, the C accessors, the mesh stream — sees the
same triangle mesh it always saw. `mesh::quads_consistent()` checks the
invariant; any operation that rewrites `indices` calls `mesh::drop_quads()`,
which is why a decimated quad mesh comes back as triangles.

**This is a lattice-derived quad grid, not retopology.** The quads follow the
sampling lattice, not the form: no edge loops around a limb or a mouth, no
poles placed at features, density does not follow curvature, and the result is
not animation-ready. It is the input a retopology pass replaces, not the output
one produces. The quads are also NOT planar, and the output is not manifold or
watertight — the marching mesher remains the path for that.

On export, **OBJ, PLY and FBX write the quads** (`f a b c d`, a four-index face
row, a four-index polygon). **GLB does not, and that is not a defect**: glTF
2.0 defines no quad primitive mode, so the writer keeps writing the
triangulation. The readers are unchanged too — a quad file this library wrote
re-imports as triangles.

#### Asking for a count

The lattice cell size is the only lever on how many quads come out, so a
requested count is a short search over it — `mesh::mesh_tape_quads_fit`,
`VoxelGrid::mesh_quads_fit`, `clay_quad_params.target_quads`,
`Document.mesh_quads(target=...)`.

**A target is approached, never hit, and what came out is reported.** A ceiling
is not promised either: the count is not monotonic in cell size, because a
finer lattice can resolve a thin feature the coarser one missed and so add
surface. Where two candidates tie, the one that does not exceed the target
wins.

The count goes as `cell⁻²`, so a 1% change in cell size moves it about 2%:
landing inside 5-10% is the expectation, the default tolerance is 10%, and a
much tighter one exhausts the iteration cap and comes back with the best
attempt and `within_tolerance` clear. **Every iteration is a whole mesh**,
including a whole dense field evaluation on the SDF path, so `max_iterations`
is a cost knob with a small default.

The search clamps rather than fails, and says which end it clamped at through
`clamped`: the fine end is the sample ceiling every dense mesher here prices
against, and for voxels the grid's own voxel size — below that a finer lattice
resamples the same step field and buys quads without buying detail. In the
voxel FACES mode there is no cell size at all, so the lever is the resolution
**level** and the granularity is a factor of about four per step: a caller who
asks for 50,000 and receives 12,000 chose a level. That search is a WALK from
the coarsest level, one mesh per level, stopping at the first level to reach
the target — so a target met at level `k` costs `k+1` meshes and reports that
in `iterations`. Budget a slider against the stack's length, not against the
two levels the target ends up between. `clamped` there names the ends of the
STACK: below what the coarsest level that yields anything gives, or above what
the finest gives. Coarse levels are often EMPTY — a stack is not a strict mip —
and an empty level is not one of those ends.

The report (`clay_mesh_quad_report`, `Mesh.quad_report`) describes a meshing
CALL, not a surface. A mesh loaded from a file, read back out of a document, or
concatenated was produced by no such call and is refused rather than answered
with zeroes.

`examples/44_quad_export.py` is the gallery version of all of this: the same
form at three target counts, with requested against actual printed for each.

## Where a mesh comes from

| | Python | C | C++ |
|---|---|---|---|
| mesh the field | `doc.mesh(...)` | `clay_document_mesh` | `mesh::mesh_tape(compile_document(doc), …)` |
| field + visible mesh layers | — | `clay_document_mesh_combined` | compose by hand |
| load a file (.obj/.ply/.fbx) | `clay.load_mesh(path)` | `clay_mesh_load` | `io::load_obj_file` / `load_ply_file` / `load_fbx_file` |
| a mesh layer's triangles | `doc.mesh_layer(name)` | `clay_document_mesh_layer`, `clay_document_mesh_layer_by_id` | `io::ClaySpaceDoc::mesh_layers` |
| a voxel grid | `grid.mesh()` | `clay_voxel_mesh` | `VoxelGrid::mesh_greedy()` |
| a brick subset (incremental) | — | `clay_brick_cache_mesh` | `mesh::mesh_bricks` |
| a coarse level of the bricks | — | `clay_brick_cache_mesh_lod` | `mesh::mesh_bricks(…, lod)` |
| triangles you own | `clay.Mesh.from_triangles` | `clay_mesh_from_triangles` | build a `mesh::Mesh` |

Whichever produced it, the readback below is identical — a mesh does not
remember where it came from.

## Python

```python
import pyclay as clay

doc = clay.Document()
layer = doc.add_sdf_layer("body")
layer.add(clay.Sphere(r=0.5))

mesh = doc.mesh(resolution=64)                 # or mesher="nets"

print(f"{len(mesh.positions)} vertices, {mesh.triangle_count} triangles")
print("positions", mesh.positions.shape, mesh.positions.dtype)   # (N, 3) float32
print("indices  ", mesh.indices.shape, mesh.indices.dtype)       # (T, 3) uint32
print("normals  ", mesh.normals.shape)                           # (N, 3), or (0, 3)
print("colors   ", mesh.colors.shape)
print("uvs      ", mesh.uvs.shape)                               # (0, 2) — none here

# Faces index into positions, so a triangle's corners are one fancy-index away.
faces = mesh.positions[mesh.indices]           # (T, 3, 3)
centroids = faces.mean(axis=1)                 # (T, 3)

print("bounds", mesh.bounds)
print("watertight", mesh.is_watertight(), "manifold", mesh.is_manifold())
```

```
57650 vertices, 115296 triangles
positions (57650, 3) float32
indices   (115296, 3) uint32
normals   (57650, 3)
colors    (57650, 3)
uvs       (0, 2)
bounds ((-0.5, -0.5, -0.5), (0.5, 0.5, 0.5))
watertight True manifold True
```

**`indices` is already `(T, 3)`.** Not flat. A `.reshape(-1, 3)` on it is a
harmless no-op, and enough example code carries one that it is worth saying.

**The arrays are zero-copy views, and they are read-only.** They point into the
mesh's own vectors, and they hold the mesh alive while you keep one — so a view
outliving the `Mesh` variable is fine, but writing through it is not:

```python
import numpy as np

mesh.positions.flags.writeable      # False
mesh.positions.base is not None     # True — a view, not a copy

moved = np.array(mesh.positions)    # copy before editing
moved[:, 1] += 0.25
lifted = clay.Mesh.from_triangles(moved, mesh.indices)
```

An absent attribute is an empty array of the right rank — `(0, 3)` for normals,
colours and positions, `(0, 2)` for uvs — never `None`. `len(mesh.uvs) == 0` is
the test.

## C

The full ABI surface is in `bindings/c/clay.h`; the meshing block starts at
`-- meshing (owner-handle pattern) --`.

```c
#include <clay.h>
#include <stdio.h>

int main(void) {
    clay_document* doc = clay_document_create();
    clay_layer_id layer = 0;
    clay_add_sdf_layer(doc, "body", &layer);

    float radius[1] = {0.5f};
    clay_item* sphere = clay_item_create(CLAY_PRIM_SPHERE, radius, 1);
    clay_node_id node = 0;
    clay_layer_add_item(doc, layer, sphere, &node);
    clay_item_destroy(sphere);

    clay_mesh_params params = {0};
    params.struct_size = (uint32_t)sizeof params;
    params.resolution = 64;    /* voxel_size stays 0, so resolution decides */
    params.mesher = CLAY_MESHER_MARCHING;

    clay_mesh* mesh = NULL;
    if (clay_document_mesh(doc, &params, &mesh) != CLAY_OK) {
        fprintf(stderr, "mesh failed: %s\n", clay_last_error());
        return 1;
    }

    const size_t vertex_count = clay_mesh_vertex_count(mesh);
    const size_t index_count  = clay_mesh_index_count(mesh);
    const float*    positions = clay_mesh_positions(mesh);  /* vertex_count * 3 */
    const float*    normals   = clay_mesh_normals(mesh);    /* or NULL */
    const float*    colors    = clay_mesh_colors(mesh);     /* or NULL */
    const float*    uvs       = clay_mesh_uvs(mesh);        /* or NULL, * 2 */
    const uint32_t* indices   = clay_mesh_indices(mesh);    /* index_count */

    printf("%zu vertices, %zu triangles\n", vertex_count, index_count / 3);
    printf("normals %s, colors %s, uvs %s\n", normals ? "yes" : "no",
           colors ? "yes" : "no", uvs ? "yes" : "no");

    /* Triangle 0, walked the way any consumer walks one. */
    for (size_t corner = 0; corner < 3; ++corner) {
        const uint32_t v = indices[corner];
        printf("  v%u  p=(%.3f, %.3f, %.3f)", v, positions[v * 3 + 0],
               positions[v * 3 + 1], positions[v * 3 + 2]);
        if (normals)
            printf("  n=(%.3f, %.3f, %.3f)", normals[v * 3 + 0],
                   normals[v * 3 + 1], normals[v * 3 + 2]);
        printf("\n");
    }

    clay_mesh_destroy(mesh);   /* positions/indices dangle after this */
    clay_document_destroy(doc);
    return 0;
}
```

```
cc -std=c11 -Ibindings/c mesh_readback.c -Lbuild/release -lclay_shared -lm -o mesh_readback
```

The attribute pointers are **borrowed** and valid exactly until
`clay_mesh_destroy`. `clay_mesh_positions` and `clay_mesh_indices` are never
NULL for a non-empty mesh; the other three are NULL when the mesh does not carry
that attribute — check, do not assume, because which attributes a mesh carries
depends on how it was produced (see [below](#which-attributes-exist-when)).

Any language with a C FFI — Swift, C#, Rust, `ctypes` — reads a mesh through
exactly these calls; the ABI is deliberately free of variadics, bitfields and
C++ types so a foreign declaration is mechanical.

### Interleaving straight into a GPU buffer

The pointer walk above is fine for export and analysis. For a *frame path* it is
two passes over geometry you just produced — interleave into a staging vector,
then copy that into the mapped buffer. `clay_vertex_layout` collapses it to one:

```c
/* position | normal | color, tightly packed: 12 + 12 + 12 = 36 bytes. */
clay_vertex_layout layout = {0};
layout.struct_size     = (uint32_t)sizeof layout;
layout.stride          = 0;   /* 0 = tightly packed, i.e. the end of the last
                                 attribute you named — the offsets are yours */
layout.position_offset = 0;
layout.normal_offset   = 12;
layout.color_offset    = 24;
layout.uv_offset       = -1;  /* -1 omits an attribute */

const size_t vertex_count = clay_mesh_vertex_count(mesh);
const size_t index_count  = clay_mesh_index_count(mesh);
const size_t stride       = 36;

void*     vertices = /* your mapped vertex buffer */;
uint32_t* indices  = /* your mapped index buffer */;

if (clay_mesh_copy_vertices(mesh, &layout, vertices, vertex_count * stride) != CLAY_OK ||
    clay_mesh_copy_indices(mesh, indices, index_count) != CLAY_OK) {
    fprintf(stderr, "copy failed: %s\n", clay_last_error());
}
```

Attribute widths are fixed: position 12 bytes, normal 12, colour 12, uv 8, all
float32 — the descriptor says *where* a value goes, not what it is converted to.

Four things are **refused rather than fudged**, because each produces a buffer
that is wrong without looking wrong:

| mistake | result |
|---|---|
| naming an attribute the mesh does not carry | `CLAY_ERROR_INVALID_ARGUMENT` — not a zero-fill |
| overlapping attribute offsets | `CLAY_ERROR_INVALID_ARGUMENT` |
| a stride that does not clear the attributes named | `CLAY_ERROR_INVALID_ARGUMENT` |
| `dst_bytes` / `dst_count` not exactly the required size | `CLAY_ERROR_INVALID_ARGUMENT` — short is not truncated, long is not padded |

Both counts are queryable before the call, so sizing exactly costs nothing. The
first row is the one that bites: a mesh from `clay_document_mesh` has no uvs, so
a layout carrying `uv_offset` fails with

```
the layout names uvs, which this mesh does not carry
```

rather than handing you a silently untextured model.

## C++

In-process there is no ABI in the way — `mesh::Mesh` is the type itself, and its
members are the buffers.

```cpp
#include <cstdio>

#include "clay/io/mesh_io.h"
#include "clay/mesh/marching.h"
#include "clay/mesh/mesh_data.h"
#include "clay/scene/document.h"
#include "clay/scene/tape.h"

int main() {
    using namespace clay;

    scene::Document doc;
    scene::Layer& layer = doc.add_sdf_layer("body");
    scene::Node sphere;
    sphere.prim = scene::Prim::sphere(0.5f);
    layer.sdf->insert(sphere);

    // A document evaluates through a TAPE; the mesher takes one plus a region.
    const scene::Tape tape = scene::compile_document(doc);
    const mesh::Mesh m = mesh::mesh_tape(tape, tape.bounds, 1.0f / 64.0f,
                                         {mesh::NormalMode::Gradient, /*colors=*/true});

    std::printf("%zu vertices, %zu triangles\n", m.positions.size(), m.triangle_count());

    // indices is a flat triangle list; three entries per face.
    for (int corner = 0; corner < 3; ++corner) {
        const std::uint32_t v = m.indices[corner];
        const kernel::cfloat3& p = m.positions[v];
        std::printf("  v%u  p=(%.3f, %.3f, %.3f)", v, p.x, p.y, p.z);
        if (!m.normals.empty()) {
            const kernel::cfloat3& n = m.normals[v];
            std::printf("  n=(%.3f, %.3f, %.3f)", n.x, n.y, n.z);
        }
        std::printf("\n");
    }
    return 0;
}
```

```
c++ -std=c++20 -Iinclude mesh_readback.cpp build/release/libclaycore.a -o mesh_readback
```

`MeshingOptions` is where the attributes are decided:

```cpp
struct MeshingOptions {
    NormalMode normals = NormalMode::Gradient;  // None | Face | Gradient
    bool colors = true;                         // sample the tape's colour field
    float gradient_eps = 1e-4f;
};
```

`NormalMode::Gradient` is blend-faithful — it reads the field's own gradient, so
a smooth-min seam gets the normal the field actually has rather than the one the
triangles imply. `Face` is area-weighted from the triangles and cheaper. UVs are
not produced by the mesher; `mesh::uv_box_project(m, scale)` adds
box-projection UVs if you want them, and it is the only place in the library
that does (there is no C or Python entry point for it).

For an engine importer or Apple's Model I/O, `io::buffer_view` hands over the
same memory as flat typed pointers with no conversion:

```cpp
const io::MeshBufferView view = io::buffer_view(m);
// view.positions  count * 3     view.normals / colors  count * 3 or null
// view.uvs        count * 2     view.indices           index_count
```

That view borrows from `m`; it is the C++ analogue of the borrowed pointers in
the C section, with the same lifetime rule.

**A meshing call needs bounded content.** A document containing only unbounded
primitives — a plane, an infinite cylinder — has no finite region to march. C
refuses it with `CLAY_ERROR_INVALID_ARGUMENT`; `mesh_tape` returns an empty
mesh for an empty or infinite region. Either way, add something bounded or pass
a finite region of your own.

## Bytes, when you have nowhere to put a path

`save` and `load` take paths. Every format is also reachable as bytes, which is
what a host uses when its documents do not live on a filesystem it controls —
an iPadOS document provider behind a security-scoped URL, a sync request, a
project stored in the host's own container, or a WASM build with no filesystem
at all.

```python
data = mesh.to_bytes("ply")            # 'obj', 'ply', 'fbx' or 'glb'
back = clay.load_mesh_bytes(data, "ply", max_vertices=10_000_000)

blob = doc.to_bytes()                  # a whole .clayspace
doc2 = clay.load_bytes(blob)
```

```c
clay_blob* blob = NULL;
clay_mesh_save_memory(mesh, "ply", &blob);
const uint8_t* p = clay_blob_data(blob);
size_t n = clay_blob_size(blob);
/* ... hand p/n to your writer ... */
clay_blob_destroy(blob);
```

**The format is named, not derived**, because a buffer has no extension. The
names are the extensions without the dot, matched case-insensitively, and an
unknown one is refused rather than served as a default.

**The bytes are identical to what the path form writes** — that is a test, not a
promise. One exception, stated: an **in-memory OBJ carries no `mtllib` line**.
The path form writes a companion `.mtl` beside the object file and names it; a
buffer has no companion, and naming a file that was never written is worse than
naming none.

**`clay_blob` borrows.** `clay_blob_data` is valid until `clay_blob_destroy` and
is unaffected by later edits to whatever produced it — the bytes were
serialized when the handle was made — so a host can hand them to an
asynchronous writer without copying first.

**The import budget guards a buffer exactly as it guards a file.** A buffer from
a network or a pasteboard is the untrusted input those ceilings exist for. The
one exception is the file-byte ceiling, which is not offered: it bounds what a
loader reads into memory before sizing a buffer, and a caller holding a buffer
has already done that read.

## What the validator measures

`is_watertight()` / `clay_mesh_validate` answer two questions. The validator
answers eleven, in the same pass, and the report is how you get the other nine
— which are the ones that matter when the answer to the first two is *no*.

```python
r = mesh.validation_report()
# {'vertices': 57650, 'triangles': 115296, 'watertight': True, 'manifold': True,
#  'oriented': True, 'clean': True, 'boundary_edges': 0, 'non_manifold_edges': 0,
#  'degenerate_triangles': 0, 'sliver_triangles': 0, 'intersecting_pairs': 0,
#  'intersection_budget': 0, 'euler_characteristic': 2}
```

```c
clay_validation_report r;
r.struct_size = sizeof(r);
clay_mesh_validation_report(mesh, 0, &r);   /* 0 = skip the intersection pass */
```

`clean` is `watertight && manifold && oriented && degenerate_triangles == 0 &&
intersecting_pairs == 0`. `sliver_triangles` is **not** one of its terms: a
near-zero-area triangle is legal geometry that plenty of exporters produce.

**`intersection_budget` is the field to read before you trust `clean`.** The
sampled self-intersection pass is off by default, and when it does not run
`intersecting_pairs` is zero because nothing looked — which `clean` then reads
as evidence. The echoed budget is the only thing separating the two:

```python
mesh.validation_report()["clean"]                            # not checked for self-intersection
mesh.validation_report(max_intersection_pairs=20000)["clean"] # checked, up to 20000 pairs
```

The cap bounds the work: pairs are tested exactly, but only spatially-close,
non-adjacent ones, and only that many of them. It is a check you ask for on an
export, not one you run every frame.

**Volume and area** come from the same module and are the only `double`s in
the C header, because a signed-volume sum cancels heavily and narrowing it
would discard the precision the engine computes it at:

```python
mesh.signed_volume      # positive when normals point OUTWARD — an orientation check
mesh.surface_area
```

```c
double volume = 0, area = 0;
clay_mesh_measure(mesh, &volume, &area);   /* either output may be NULL */
```

Both are answered for any mesh, watertight or not. An open mesh still has a
divergence-theorem sum, and refusing to state it would hide the number you use
to notice the mesh is open.

## Ownership and lifetime

Three cases, and the middle one is the trap.

| you got the mesh from | you own it | free it with |
|---|---|---|
| `clay_document_mesh`, `clay_mesh_load`, `clay_mesh_from_triangles`, `clay_mesh_transform`, `clay_mesh_concat`, `clay_document_mesh_combined`, `clay_voxel_mesh`, `clay_brick_cache_mesh`, `clay_brick_cache_mesh_lod` | yes | `clay_mesh_destroy` |
| `clay_document_mesh_layer`, `clay_document_mesh_layer_by_id`, and the `out_mesh` of `clay_document_add_mesh_layer` | **no — the document owns it** | nothing; `clay_mesh_destroy` on it is a silent no-op |
| Python: any `Mesh` | the interpreter | — |

A **borrowed** mesh is a live window onto a mesh layer, not a snapshot: every
access looks the geometry up in the document again. Ask which layer it belongs
to with `clay_mesh_layer` (`mesh.layer` in Python; `None` for a mesh you own) —
that id is what the ordinary layer calls take for transform, visibility, ghost,
lock and ordering. Destroying the document invalidates the mesh and every
pointer taken from it. If the layer's geometry is no longer in the document at
all, the C accessors answer NULL and Python raises `mesh layer was removed from
its document`, rather than reading freed memory.

Python is safer here than C: the numpy views hold the mesh alive, and a borrowed
mesh holds the document alive, so `del doc` while you still hold arrays is fine.

## Which attributes exist, when

| producer | normals | colours | uvs |
|---|---|---|---|
| `clay_document_mesh` / `doc.mesh()` / `mesh_tape` | yes (gradient, or face) | yes, from the colour field | **no** |
| `clay_brick_cache_mesh` with a document | yes | yes | no |
| `clay_brick_cache_mesh` with `doc = NULL` | face normals only | no | no |
| `clay_brick_cache_mesh_lod` at `lod = 1` | face normals only | **refused** | no |
| `clay_voxel_mesh` / `grid.mesh()` | yes | per-face palette colour | no |
| OBJ / PLY / FBX import | as the file carries | as the file carries | as the file carries |
| `clay_mesh_from_triangles` | no | no | no |

Three consequences worth planning for:

- **A coarse brick mesh carries no field attributes.** Colours and gradient
  normals ride per-brick culled tapes, and those are exact because a vertex sits
  on the field's surface; a vertex found on the *mip's* lattice sits on the
  mip's surface, up to most of a coarse cell off it, where the culled tape and
  the whole document's are only both-out-of-band rather than equal. So
  `clay_brick_cache_mesh_lod` refuses them at `lod = 1` rather than returning
  approximate ones. Shade a coarse mesh with face normals, or re-evaluate the
  attributes yourself if you want the field's answer at those positions.
- **A meshed field has no UVs.** If your pipeline needs them, box-project in
  C++, or unwrap downstream. A `clay_vertex_layout` naming uvs will be refused,
  which is the loud version of this same fact.
- **Concatenation drops an attribute the inputs disagree on.** `clay_mesh_concat`
  and `clay_document_mesh_combined` return a mesh with *no* uvs if one input
  carried them and another did not — never padded, never truncated, because a
  ragged attribute is a malformed mesh. The meshed field carries normals, so
  combining it with a normal-less mesh layer costs the result its normals. Said
  here because the alternative is discovering it in an exported file.

Voxel meshes report `watertight=False`. Greedy meshing gives each merged quad
its own vertices, so the shared edges are not shared in the index buffer even
though the surface is geometrically closed. That is the validator being literal,
not a hole.

## Editing and handing geometry back

The read path is read-only in every language, and rebuilding is how you hand an
edited mesh back:

```python
moved = np.array(mesh.positions)
moved[:, 1] += 0.25
edited = clay.Mesh.from_triangles(moved, mesh.indices)
```

```c
clay_mesh* edited = NULL;
clay_mesh_from_triangles(positions, vertex_count, indices, index_count, &edited);
```

`clay_mesh_from_triangles` copies, so your buffers may be freed on return, and it
takes positions and indices only — an edited mesh comes back without normals,
colours or uvs. `clay_mesh_transform` is the exception worth knowing: it moves
positions by a transform and rotates normals rather than dropping them.

**A mesh layer's vertices CAN be moved in place**, by the fixed-topology mesh
brushes — `mesh::MeshSculptor`, `clay.MeshSculptor`, `clay_mesh_sculptor_*`.
They move vertices and nothing else: `indices` and `quads` come out byte for
byte as they went in, so a quad export re-imported after a retopo pass survives
being sculpted. Undo is a sparse `VertexDeltas` record rather than the command
stack, because a vertex displacement is not an edit item. See
[07 § 8](07-brushes-and-features.md).

To sculpt an imported model by RESAMPLING it instead — which is what you want
when the triangles are a means rather than the deliverable — sample it into a
field with `clay_item_volume_from_mesh` (`clay.Volume.from_mesh` in Python); see
[05 §7](05-claycore-library.md#7-meshing--mesh-processing-claymesh). That path
discards the topology, which is exactly the difference between the two.

## Reading back only what changed

Everything above copies a whole mesh, and at twenty million vertices a host that
does that per dab has already lost. A dab touches a few thousand vertices; the
readback should cost what it TOUCHED and not what the model HOLDS.

`clay_surface_view` is the seam for that, and it is one seam for all three
representations — a fixed mesh, an adaptive surface and a level of a
multiresolution hierarchy. They are partitioned into CHUNKS, a stamp marks the
chunks it reached, and a host drains that set into buffers it owns.

```c
clay_surface_view* view = NULL;
clay_surface_view_from_dynamic(sculptor, &view);   /* or _from_mesh / _from_multires */

size_t n = 0;
clay_surface_view_dirty_chunks(view, NULL, &n);    /* size query */
uint32_t* dirty = malloc(n * sizeof(uint32_t));
clay_surface_view_dirty_chunks(view, dirty, &n);

for (size_t i = 0; i < n; ++i) {
    clay_chunk_readback need = { .struct_size = sizeof(need) };
    clay_surface_view_copy_chunk(view, dirty[i], NULL, NULL, 0, NULL, 0, NULL, 0, &need);

    float* positions = malloc(need.vertex_count * 3 * sizeof(float));
    uint32_t* indices = malloc(need.index_count * sizeof(uint32_t));
    clay_chunk_readback got = { .struct_size = sizeof(got) };
    clay_surface_view_copy_chunk(view, dirty[i], NULL,
                                 positions, need.vertex_count * 3, NULL, 0,
                                 indices, need.index_count, &got);
    upload(dirty[i], positions, indices);          /* your renderer */

    /* Retire it — against what you actually copied. */
    size_t clean = 0;
    clay_surface_view_acknowledge(view, &dirty[i], &got.current, 1, &clean);
}
clay_surface_view_destroy(view);
```

Four things about that loop are the whole point.

- **The buffers are yours, and the capacity query comes first.** Nothing here
  allocates a heap object per chunk per frame, and nothing hands back a pointer
  into the engine: a mutation can move or free anything, and at this scale it
  does so mid-drag. A buffer that is too small has NOTHING written into it —
  not a partial fill you might draw — and the counts say what it needed.
- **Four revisions, not one.** `clay_chunk_info.revisions` separates topology,
  geometry, normals and attributes, so you re-upload an index buffer only when
  connectivity actually changed and can tell a deferred normal flush from a
  move. The single `revision` beside them is the maximum of the four.
- **Acknowledge, do not clear.** `clay_surface_view_acknowledge` retires a chunk
  only if its revision still matches the one you copied. Drop a frame half way
  through the set and the rest stays dirty; if a chunk changed again between the
  copy and the acknowledgement it stays dirty too, so draining across frames at
  any rate loses nothing. `clay_surface_view_clear_dirty` is the
  all-or-nothing form, for a host that uploads everything in one pass.
- **A stale readback is identifiable.** Pass the revisions you last saw as
  `expected` and the result carries them back beside what the engine is at now,
  with `stale` set when they differ. Without that a host drawing a superseded
  chunk draws something the engine does not think it made, and nothing in the
  pixels says so.

A chunk of a fixed mesh or a multires level is WELDED — its own vertices, with
indices local to it, so it uploads as a standalone draw. An adaptive surface's
chunks are UNWELDED triangles, because its topology changes under the very stamp
being uploaded and a per-chunk vertex map would have to be rebuilt per chunk per
frame. Read `vertex_count` rather than assuming either.

In Python the same loop is numpy-native:

```python
view = clay.SurfaceView.over_dynamic(sculptor)
for chunk in view.dirty_chunks:
    got = view.copy_chunk(chunk)          # positions (N,3), normals (N,3), indices (M,)
    upload(chunk, got["positions"], got["indices"])
    view.acknowledge(chunk, got["revisions"])
```

### Telling the engine what you can afford

A host on a memory-constrained device fills a profile and asks for memory back
when the operating system asks it for memory back. Nothing in the library
detects a device, and nothing evicts on its own high-water mark:

```python
profile = clay.SculptMemoryProfile()
profile.memory_class = clay.MemoryClass.constrained
profile.max_resident_levels = 2
hierarchy.memory_profile = profile

led = hierarchy.memory_ledger()           # bytes by category, plus three roll-ups
print(led["essential"], led["rebuildable"], led["undoable"])

report = hierarchy.trim(clay.Pressure.critical)
```

A trim never touches unsaved authoritative content — the eviction order and what
it excludes are in [05](05-claycore-library.md#memory-under-pressure-who-decides-what-and-in-what-order),
verbatim. If a save or a readback is in flight, hold a pin and the trim becomes a
no-op that reports what it WOULD have released:

```python
with clay.MemoryPin() as pin:
    report = hierarchy.trim(clay.Pressure.critical, pin)
    assert report["pinned"]               # nothing went; this is the estimate
    blob = hierarchy.serialize()
```

And any operation whose transient PEAK exceeds its result can be priced before
it is paid — `Mesh.preflight_to_dynamic`, `Mesh.preflight_global_remesh`,
`DynamicSurface.preflight_to_mesh`, `DynamicSurface.preflight_encode`,
`MultiresSurface.preflight_encode`, and `clay_multires_preflight_add_level`
which came first. Read `peak_bytes`: an operation priced by what it leaves
behind is the one that terminates the process half way through.

## Handing a sculpt to the retopology engine

`clay_mesh_save_handoff` writes the **sculpt handoff** that CyberRemesherAndUV's
reader accepts — the `sculpt → retopo → UV → bake` seam, with neither engine
linking the other.

**The format is not ours.** It is that repository's
`docs/sculpt-handoff-format.md`, version 1.0, which ships the *reading* half and
records that agreement with ClayCore was outstanding because no negotiation ever
took place. Their CLI already assumed this half existed:

```sh
producer --for-retopo | cyberremesh --target - --output low.obj --preset blender
```

Where their spec and ours disagree, theirs is right.

```python
mesh = doc.mesh_quads(cell_size=0.05)
mesh.save_handoff("sculpt.ply", material_mask=doc.mask("body"))
```

**Two guarantees the writer makes for you**, because both are conditions their
reader enforces and neither is something a caller can be expected to know:

- **The faces are always triangles.** `save()` declares a mesh's *quads* as its
  faces when it has them, and their reader rejects any other arity — *"a sculpt
  export that is not triangulated is a producer bug."* So the quad export, our
  best one, is exactly the file it would refuse. Verified against their reader:
  a quad mesh written with `save()` is rejected; the same mesh written as a
  handoff round-trips and retopologizes.
- **Normals are always present.** They are required, and a mesh meshed without
  gradients has none. They are computed into the output; your mesh is not
  modified.

**`material_mix` comes from a mask.** Their spec calls it *"the sculpt's
per-vertex blend weight between two material slots."* ClayCore has no material
slots and does not invent them — a mask is already a painted scalar in `[0,1]`
answerable at any point, which is the shape and the meaning asked for. Pass the
mask that means "the second material"; pass nothing and the required channel is
zero, which is the honest answer for a document that never expressed one.

**The in-memory buffer profile** is for when both engines share a process, which
is the case that matters on a tablet. ClayCore deliberately does **not** publish
a struct for it: four of the five arrays their `BufferView` wants — positions,
normals, colours, indices — are already borrowed pointers here, so duplicating
their struct would give the two engines two places to disagree about the layout.
`clay_mesh_handoff_material_mix` produces the one column you cannot already get.

### Wiring ClayCore into their baker

The seam was chosen deliberately: **their engine bakes; this one answers field
queries.** Baking wants UV semantics — seams, islands, padding, dilation, texel
density — which their repository owns, and a second implementation here would
disagree with theirs about exactly the details that make a bake look right.

Their `CyberFieldEvaluator` takes three C callbacks and a `void*`. Nothing
further is needed from this ABI to fill them:

| their callback | ClayCore |
|---|---|
| `distance(p)` | `clay_eval_points` |
| `gradient(p)` | `clay_eval_gradients` (already unit length) |
| `occlusion(p, n, r)` | **`1.0f -`** `clay_measure_points(CLAY_MEASURE_OCCLUSION)` |
| `curvature(p, h)` | leave it to their default, which derives it from `gradient()` |

**Note the inversion**, which is the trap in that table. Their `occlusion` is
*openness* — 1 is fully open — and `CLAY_MEASURE_OCCLUSION` is occlusion, where
1 is fully enclosed. Passing ours straight through bakes an inverted ambient
occlusion map, which looks plausible and is wrong everywhere.

`CLAY_MEASURE_CURVATURE` is deliberately **not** in that table: it is a
saturated `[0,1]` value built for masking, while theirs is signed mean curvature
in `1/length` units where a sphere of radius *r* reads `1/r`. They are not the
same number and substituting one for the other is wrong.

## Runnable examples

| | |
|---|---|
| `examples/08_meshing_and_io.py` | the three meshers compared, validation, every export format |
| `examples/19_mesh_import.py` | reading `positions` / `indices` off an imported model |
| `examples/36_mesh_layers.py` | a borrowed mesh layer, and that its arrays are views |
| `examples/45_mesh_brushes.py` | moving a carried mesh's vertices without touching its topology |
| `tests/unit/test_c_mesh_copy.cpp` | `clay_vertex_layout` including every refused layout |
| `tests/c_api/smoke.c` | a pure-C consumer end to end |

## Getting the colours back after a round trip

Anything that leaves a mesh layer loses what the layer was holding. Sampling a
model into a field and meshing it back keeps the shape; the colours and uvs are
gone, because a distance field carries neither.

`Mesh.transfer_attributes` refunds them, by asking the nearest point on the
ORIGINAL surface what belonged there:

```python
src = doc.mesh(resolution=32)              # the coloured original
vol = clay.Volume.from_mesh(src, cell=0.04)
back = other.mesh(resolution=32)           # new geometry, no colours

report = back.transfer_attributes(src)
# {'transferred': 15638, 'fell_back': 0, 'colors': True, 'uvs': False,
#  'normals': False, 'max_distance': 0.1039}
```

In C:

```c
clay_transfer_desc d; d.struct_size = sizeof d;
clay_mesh_transfer_defaults(&d);
clay_transfer_report r; r.struct_size = sizeof r;
clay_mesh_transfer_attributes(source, target, &d, &r);
```

**Read the report.** A transfer that fell back across most of the mesh is
otherwise indistinguishable from a good one — `fell_back` counts the vertices
that were farther from the source than the threshold, which happens where
geometry exists that the source never occupied.

**Normals are off by default**, and the default is the point: a resampled mesh
has its own geometry and its normals should describe *it*. Taking the source's
would make new geometry shade like the old shape. Turn it on only when the two
meshes are near-identical and the source's normals were authored.

**What it does not give back is the topology.** The target is still the
mesher's geometry — new vertices, no edge loops, no relationship to the
retopology that went in. This refunds the paint and most of the uvs; it does
not refund the mesh. If a workflow needs the topology preserved, attribute
transfer is not a partial answer to that, and `openspec/ROADMAP.md` records why
mesh-level booleans are not the answer either.

**UV seams.** Uvs are per *vertex*, which is how a seam exists at all: the
source duplicates a position into two vertices carrying different uvs. A target
vertex sitting on that seam has one slot and two correct answers, and takes
whichever triangle the closest-point query returned — which can stretch a
triangle across the layout. Colour has no such problem, being continuous across
a seam. This follows from per-vertex uvs rather than from a defect, so it is
stated rather than left to be discovered.

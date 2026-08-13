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

## Where a mesh comes from

| | Python | C | C++ |
|---|---|---|---|
| mesh the field | `doc.mesh(...)` | `clay_document_mesh` | `mesh::mesh_tape(compile_document(doc), …)` |
| field + visible mesh layers | — | `clay_document_mesh_combined` | compose by hand |
| load a file (.obj/.ply/.fbx) | `clay.load_mesh(path)` | `clay_mesh_load` | `io::load_obj_file` / `load_ply_file` / `load_fbx_file` |
| a mesh layer's triangles | `doc.mesh_layer(name)` | `clay_document_mesh_layer` | `io::ClaySpaceDoc::mesh_layers` |
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

## Ownership and lifetime

Three cases, and the middle one is the trap.

| you got the mesh from | you own it | free it with |
|---|---|---|
| `clay_document_mesh`, `clay_mesh_load`, `clay_mesh_from_triangles`, `clay_mesh_transform`, `clay_mesh_concat`, `clay_document_mesh_combined`, `clay_voxel_mesh`, `clay_brick_cache_mesh`, `clay_brick_cache_mesh_lod` | yes | `clay_mesh_destroy` |
| `clay_document_mesh_layer`, and the `out_mesh` of `clay_document_add_mesh_layer` | **no — the document owns it** | nothing; `clay_mesh_destroy` on it is a silent no-op |
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

The read path is read-only in every language — there is no call that mutates an
existing mesh's vertices in place. Rebuild instead:

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

To sculpt an imported model rather than carry it, sample it into a field with
`clay_item_volume_from_mesh` (`clay.Volume.from_mesh` in Python) — see
[05 §7](05-claycore-library.md#7-meshing--mesh-processing-claymesh).

## Runnable examples

| | |
|---|---|
| `examples/08_meshing_and_io.py` | the three meshers compared, validation, every export format |
| `examples/19_mesh_import.py` | reading `positions` / `indices` off an imported model |
| `examples/36_mesh_layers.py` | a borrowed mesh layer, and that its arrays are views |
| `tests/unit/test_c_mesh_copy.cpp` | `clay_vertex_layout` including every refused layout |
| `tests/c_api/smoke.c` | a pure-C consumer end to end |

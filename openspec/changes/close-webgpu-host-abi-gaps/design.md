# Design: close-webgpu-host-abi-gaps

Five decisions, each recorded with the alternative it beat, so a reviewer can
disagree with the reasoning rather than only with the result.

## 1. Brick colour is an RGBA8 lattice stored in the cache

**Decision.** A brick optionally carries a second lattice of `dim³` RGBA8
texels beside its `dim³` fp16 distances. It is produced by the same evaluation
that produces the distances, submitted through the same call, and read back
through the same fixed-stride contract.

**Storage: RGBA8, not fp16 RGB, not a palette index.**

- `rgba8unorm` is filterable in WebGPU and is 4 bytes per texel. Three fp16
  channels is 6 bytes and is not a texture format — `rgba16float` is four
  channels, so the honest fp16 option costs 8 bytes, twice the distance
  lattice's own payload, to carry a channel nobody reads.
- The colour field is an authored albedo, not radiance. `ctape_eval` seeds it at
  `(0.5, 0.5, 0.5)` and combine modes `cmix` between item colours; nothing in the
  kernel set produces a value outside [0, 1]. Eight bits per channel is the
  precision the voxel engine already commits to — `VoxelGrid` indexes a 255-entry
  `cfloat3` palette, so a colour that survives a voxel round-trip is already
  quantized harder than this.
- A palette index would be smaller still and is what the issue offers as an
  alternative, but a brick cache has no palette: the colour it stores comes from
  an SDF tape, where colour is continuous and blended. Building a palette would
  mean quantizing a continuous field into a table the cache would then have to
  own, invalidate and version. RGBA8 is the same 4 bytes with no table.

Alpha is written as 255 and reserved. It is not a mask channel and not coverage;
a host that wants either should not read it as one.

**Opt-in, per cache, at creation.** `clay_brick_config` gains
`int32_t colors`. Zero — what `clay_brick_config_defaults` fills and what a
zeroed-then-`struct_size`-set descriptor gets — keeps today's cache exactly.
The reason it is a creation-time flag rather than a per-call one is that the
lattice has to be *evaluated* to exist: a per-call flag would let a host ask a
distance-only cache for colours it never computed, and the only truthful answer
would be an error at read time for a mistake made three calls earlier.

Colour payload counts against `memory_budget` alongside the distance payload,
because a budget that bounds half of what a brick costs is not a ceiling. A
colour cache therefore holds roughly a third the bricks a distance-only cache
holds at the same budget. That is a real cost and it is why this is opt-in.

**Uniform bricks.** `CLAY_BRICK_INSIDE` and `CLAY_BRICK_OUTSIDE` allocate no
distance lattice and allocate no colour lattice either. On readback, a uniform
brick's colour slice is filled with a single value so an uploader still never
branches: **the colour at the brick's own centre lattice sample**, taken from
the colours that were submitted with it. Storing one RGBA8 per uniform brick is
4 bytes against the state byte it already carries, which is not a budget
question.

One rule for both signs rather than two. The first draft of this design said
"centre sample for inside, the tape's far-field seed `(0.5, 0.5, 0.5)` for
outside", and that is wrong for the outside bricks that matter: a brick just
beyond the band still evaluates to the neighbouring item's colour, not to the
seed, and forcing the seed there would put a grey shell one brick thick around
every sculpt. Where nothing is nearby the field returns the seed anyway, so the
simpler rule gives the intended answer in the case the second rule was written
for, and a better one everywhere else.

**Rejected: evaluate colour on demand at readback.** It needs a document and a
tape, so `read_bricks` would grow a `clay_document*`, would compile a tape per
call, and would cost a full field evaluation on the *upload* path — the exact
cost the cache exists to remove. The whole value of a brick cache is that
readback is a memcpy.

## 2. The apron is computed by the library, from the neighbours it already holds

**Decision.** `clay_brick_cache_read_bricks` gains `int32_t apron`. With
`apron = A` each brick is written as `(dim + 2A)³` samples: the brick's own
lattice in the middle, and a halo of `A` voxels on every face taken from the
neighbouring bricks. The stride stays fixed and the states array is unchanged —
a key's *state* is still its own brick's state, not the halo's.

**The halo comes from `BrickCache::sample`,** which already answers for any
lattice coordinate in any brick, tracked or not: an implicit inside brick
answers `-band`, an outside or never-evaluated one answers `+band`. So the halo
is defined everywhere with no special cases, and a tile at the edge of the
sculpted region filters correctly against the band rather than against garbage.

**`CLAY_BRICK_MISSING` still leaves the slice untouched.** The rule is about the
*key*, not its neighbourhood: if the cache holds nothing for the key, the whole
`(dim + 2A)³` slice is left alone. A host that wants a padded tile for a brick
that does not exist is asking for a tile of band values it can synthesize itself.

**Bounds: `0 <= apron <= dim`.** Zero is today's behaviour and stays the
documented default. The ceiling is `dim` rather than something larger because a
halo wider than a brick means the tile is mostly neighbours, at which point the
host wants a coarser lod, not a fatter apron. Above it the call is rejected
rather than clamped, for the reason lod > 1 is rejected: silently answering a
smaller tile puts wrongly-sized data in a texture.

**`values_capacity` becomes `count * (dim + 2A)³`,** still checked exactly. This
is the one argument the ABI cannot verify against the caller's memory, so it
stays required rather than inferred, unchanged in spirit.

**Cost, stated plainly.** `apron = 0` remains a memcpy of the stored vector.
`apron > 0` is a per-sample gather, because the destination layout is not the
stored layout — that is inherent to padding and is the cost the host pays
anyway, moved to the side of the boundary that can do it without reading
neighbour keys twice. The interior of a brick is still copied row-run by row-run,
not sample by sample.

**Rejected: an apron on the stored bricks.** Storing the halo would make every
brick `(dim + 2)³` — a 42% payload increase at `dim = 8` — and would make a
neighbour's edit dirty its neighbours' stored data, destroying the bit-identical
locality guarantee that is the reason this cache exists.

## 3. Subset meshing takes a key list, and reports contiguous per-key ranges

**Decision.** `clay_brick_cache_mesh` gains `const int32_t* keys_xyz` and
`size_t key_count`. `NULL` and `0` mean "every surface brick", which is today's
behaviour and stays the documented default. A key in the list that is not a
surface brick contributes nothing and is not an error — a host passing the
result of `take_dirty` will routinely include bricks that turned out uniform.

**Per-key ranges.** An optional `clay_brick_mesh_range*` output, one element per
key in the order the keys were given:

```c
typedef struct clay_brick_mesh_range {
    int32_t key[3];
    uint32_t vertex_first, vertex_count;
    uint32_t index_first,  index_count;
} clay_brick_mesh_range;
```

An **array element, not a versioned descriptor** — a caller receives one per
key, thousands at a time. It goes in `ARRAY_ELEMENT_STRUCTS` in
`tools/check_c_abi.py` with that reason, exactly as `clay_brick_request` did.

Both ranges are contiguous because the mesher marches keys in list order and
appends: every vertex a key is the first to emit lands in its vertex range, and
every triangle its cells produce lands in its index range.

**The property a host must know, and which this design does not hide.** Vertices
are welded on canonical lattice-edge keys, and that welding spans brick seams.
So a triangle in key B's index range may reference a vertex in key A's vertex
range — the *first* key to reach a shared seam vertex owns it. A host can
therefore upload a key's ranges into sub-ranges of a GPU buffer, which is what
was asked for, but cannot free one key's vertices without checking its
neighbours'. This is documented on the struct rather than engineered away:
breaking the weld would produce a seam-duplicated mesh, and `mesh_bricks` is also
the export path, where watertightness is the contract.

**Geometry is identical to the full mesh, per cell.** Marching brick K's cells
samples across the brick boundary through `BrickCache::sample`, which answers for
any key. So a subset mesh contains exactly the triangles the full mesh contains
for those cells. The only difference is that a seam vertex shared with a cell
*outside* the subset is emitted twice — once in each mesh — at an identical
position. A duplicated seam vertex, never a crack, and the incremental-display
use case wants exactly that.

## 4. Interleaving is a copy-out, not a meshing parameter

**Decision.** The mesh keeps producing its own deinterleaved buffers.
Interleaving is a separate entry point that writes into caller memory:

```c
typedef struct clay_vertex_layout {
    uint32_t struct_size;
    uint32_t stride;           /* bytes per vertex; 0 = tightly packed */
    int32_t position_offset;   /* byte offset, or -1 to omit */
    int32_t normal_offset;
    int32_t color_offset;
    int32_t uv_offset;
} clay_vertex_layout;

clay_result clay_mesh_copy_vertices(const clay_mesh* mesh, const clay_vertex_layout* layout,
                                    void* dst, size_t dst_bytes);
clay_result clay_mesh_copy_indices(const clay_mesh* mesh, uint32_t* dst, size_t dst_count);
```

Every attribute is float32 in its source form and is written as float32: this
descriptor names *where* an attribute goes, not what it is converted to. Format
conversion is a second axis and adding it now would mean choosing a format
enumeration for four attributes before anyone has asked for one.

**This costs one pass, which is the point.** Today a host does an interleave pass
into a staging vector and then a copy into the mapped buffer. Here the mapped
buffer *is* `dst`, and there is one pass over the vertices.

**Rejected: caller-supplied destinations on the meshing calls themselves.** The
issue asks for this — "let a caller supply the destination buffers" — and it
does not work: a mesher cannot report its vertex count before it has run, so a
caller cannot size a buffer beforehand, and a two-call size-then-fill would mean
meshing twice. `read_bricks` can take caller buffers precisely because the count
is known in advance and the stride is fixed; a mesh has neither property. The
copy-out delivers the same saving — one pass, host layout, host memory — without
inventing a size query that would have to mesh to answer.

`clay_mesh_vertex_count` and `clay_mesh_triangle_count` already size both
destinations, so the two-call shape a caller needs exists and is free.

**`dst_bytes` and `dst_count` are exact-fit checks in the `read_bricks` spirit**
— required, not inferred, and the call is refused rather than truncating.
Requesting an attribute the mesh does not carry (`color_offset >= 0` on a mesh
meshed without colours) is refused rather than zero-filled: a black model is a
harder bug to find than a returned error.

## 5. Batched brick raycast mirrors `clay_raycast_many` exactly

**Decision.**

```c
clay_result clay_brick_cache_raycast_many(const clay_brick_cache* cache,
                                          const float* rays_origin_dir, size_t count,
                                          int32_t* out_hits, float* out_t,
                                          float* out_positions_xyz, float* out_normals_xyz);
```

Same packed six-float ray layout, same optional outputs, same "a ray that hits
nothing is not an error". There is no design freedom here worth spending and the
value of the item is that a host writes one call shape for both surfaces.

It is a loop over the single-ray path and starts no threads, consistent with the
cache taking no lock and owning no thread: a host that wants this parallel splits
the array itself, which it can, because the call is `const` on the cache and the
host is already required to serialize mutations.

## What the gates require

- **`tools/check_c_abi.py`** — `clay_vertex_layout` takes the `struct_size`
  prefix (it is a descriptor a caller fills in). `clay_brick_mesh_range` joins
  `ARRAY_ELEMENT_STRUCTS` with its reason.
- **`tools/check_binding_parity.py`** — every new C entry point needs a pyclay
  counterpart or a stated exemption. Colour, apron, subset meshing and batched
  raycast are all real capabilities and get real pyclay counterparts.
  `clay_mesh_copy_vertices` is the one exemption candidate: interleaving into a
  caller's mapped GPU buffer has no meaning in Python, where the natural
  equivalent is the buffer protocol on the existing arrays. The exemption is
  written with that reason rather than left to the prefix rule.

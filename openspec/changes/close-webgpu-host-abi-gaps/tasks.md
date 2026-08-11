# Tasks: close-webgpu-host-abi-gaps

## 1. Brick colour (issue #43 item 2)

- [x] 1.1 `BrickConfig::colors` and a colour lattice on `Brick`; `brick_bytes()` accounts for both payloads so the memory budget bounds a whole brick
- [x] 1.2 `BrickCache::submit` accepts an optional `const float* colors_rgb` (`dim³ * 3`), quantized to RGBA8 with alpha 255; refuse colours on a cache configured without them, and refuse their absence on a cache configured with them
- [x] 1.3 Uniform bricks store one RGBA8 rather than a lattice: the field colour at the brick's own centre sample, for both signs (see `design.md` — the two-rule version put a grey shell around every sculpt)
- [x] 1.4 `BrickCache::read_colors` / extend the brick read path; the level-1 mip carries no colour and a colour read at lod 1 is refused
- [x] 1.5 `clay_brick_config.colors`; `clay_brick_cache_eval_requests` gains an optional `out_colors_rgb` fed by `clay_eval_grid`'s existing colour output; `clay_brick_cache_submit` gains the matching input
- [x] 1.6 `clay_brick_cache_read_bricks` gains `uint8_t* out_colors_rgba` with its own exact-fit capacity
- [x] 1.7 Test: refill a colour cache, read back, compare against `clay_eval_points` colours at the lattice points within the 8-bit quantization step
- [x] 1.8 Test: a distance-only cache reports the same `memory_usage` it did before this change, and refuses a colour read
- [x] 1.9 Test: budget accounting includes colour — a budget that admits N bricks without colour admits fewer with it, and is never breached

## 2. Brick apron (issue #43 item 3)

- [x] 2.1 `BrickCache` padded read: `(dim + 2A)³` per key, halo from `sample()` so implicit and never-evaluated neighbours answer band values
- [x] 2.2 Interior rows copied as runs rather than sample by sample; `apron = 0` stays the existing memcpy with no new branch on the hot path
- [x] 2.3 `clay_brick_cache_read_bricks` gains `int32_t apron`; `values_capacity` is `count * (dim + 2A)³` exactly; `0 <= apron <= dim` and above it the call is refused
- [x] 2.4 `CLAY_BRICK_MISSING` leaves the whole padded slice untouched — assert it, since the rule now spans more bytes than the brick
- [x] 2.5 Colour readback takes the same apron, in the same padded stride
- [x] 2.6 Test: two adjacent surface bricks read with `apron = 1`; each halo equals the neighbour's boundary plane, sample for sample
- [x] 2.7 Test: a brick at the edge of the tracked region — halo is `+band` for absent neighbours, `-band` for implicit inside ones
- [x] 2.8 Test: a mixed set of surface / uniform / missing keys keeps the padded stride, and only the missing slice is unwritten

## 3. Subset meshing (issue #43 item 1)

- [x] 3.1 `mesh::mesh_bricks` takes an optional `const std::vector<brick::BrickKey>*`; NULL keeps `cache.surface_bricks()`
- [x] 3.2 Optional per-key range output, filled as the builder appends; ranges are contiguous and partition the output
- [x] 3.3 `clay_brick_mesh_range`; add it to `ARRAY_ELEMENT_STRUCTS` in `tools/check_c_abi.py` with the reason
- [x] 3.4 `clay_brick_cache_mesh` gains `keys_xyz`, `key_count` and `out_ranges`; NULL/0 is today's whole-surface behaviour
- [x] 3.5 Header text: welding spans brick seams, so a key's triangles may reference an earlier key's vertices — a consumer may overwrite a range but not free it in isolation
- [x] 3.6 Test: mesh whole, then mesh every key one at a time; the union of the per-key triangles matches the whole mesh's triangles as world-space position triples
- [x] 3.7 Test: two adjacent bricks meshed separately produce bit-identical seam vertex positions
- [x] 3.8 Test: a key list containing uniform and untracked keys succeeds and contributes nothing
- [x] 3.9 Test: ranges partition — no gaps, no overlaps, last range ends at the vertex and index counts
- [x] 3.10 Benchmark the claim the issue makes — `BM_MeshBricksWhole` vs `BM_MeshBricksSubset`, gated by `check_bench.py`'s `FASTER_THAN`. **22.6 ms for 232 surface bricks against 0.64 ms for the 8 a dab dirties**

## 4. Caller-owned interleaved mesh buffers (issue #43 item 4)

- [x] 4.1 `clay_vertex_layout` with the `struct_size` prefix; `stride = 0` means tightly packed from the named attributes
- [x] 4.2 `clay_mesh_copy_vertices` / `clay_mesh_copy_indices`, exact-fit destinations, refusing an attribute the mesh does not carry
- [x] 4.3 Reject overlapping attribute ranges and a stride that does not clear the attributes it is asked to hold — the two mistakes that produce a silently wrong buffer
- [x] 4.4 Test: interleaved copy matches the deinterleaved accessors element for element, at a stride with padding and at `stride = 0`
- [x] 4.5 Test: absent attribute refused; short destination refused; overlapping offsets refused
- [x] 4.6 Exempt `clay_mesh_copy_vertices` in `tools/check_binding_parity.py` with the reason (Python's equivalent is the buffer protocol over the existing arrays), or give it a pyclay counterpart if one reads naturally

## 5. Batched brick raycast (issue #43 item 8)

- [x] 5.1 `clay_brick_cache_raycast_many`, mirroring `clay_raycast_many` in layout and optional outputs; bounded by `CLAY_MAX_BATCH` like every other batch
- [x] 5.2 Test: a batch agrees with the single-ray path ray for ray, misses included

## 6. Bindings, gates and documentation

- [x] 6.1 No pyclay counterparts: the gate runs pyclay→C, and `BrickCache` already carries a reviewed `C_ONLY_FOLLOW_UPS` entry deferring the whole binding. Its text now names the atlas surface too, and `Mesh.copy_vertices` is recorded with its own reason (numpy interleaves the existing buffer-protocol arrays without crossing the boundary at all)
- [x] 6.2 `tools/check_c_abi.py` passes: descriptor prefix rule on `clay_vertex_layout`, element exemption on `clay_brick_mesh_range`, every declaration resolving in the shared library
- [ ] 6.3 Swift smoke and the ctypes FFI exercise cover at least the colour + apron readback, since that is the path this change exists for
- [x] 6.4 `docs/06-host-gpu-previews.md` gains the brick-volume route as a first-class alternative to compiling the kernel dialect: upload the band as a sparse `r16float` + `rgba8unorm` atlas with a one-voxel apron, trace it with a brick DDA, and mesh only for export
- [x] 6.5 Announce the two arity changes in `docs/RELEASE.md` release notes, with the compile-error-not-misread argument
- [x] 6.6 Update `openspec/ROADMAP.md`
- [ ] 6.7 Reply on issue #43 covering items 1, 2, 3, 4 and 8, and stating what was decided against (no colour on the mip, no caller-owned destination on meshing itself, and why)

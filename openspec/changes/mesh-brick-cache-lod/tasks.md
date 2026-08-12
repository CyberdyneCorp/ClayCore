# Tasks: mesh-brick-cache-lod

## 1. Establish what the mip lattice actually is (#93 asked for this first)

- [x] 1.1 `BrickCache::build_mip` keeps every second lattice point of the 2x2x2 block, so a coarse brick is the cache's own `dim³` lattice at twice the spacing, still anchored at the world origin — coarse lattice coordinate `g` sits at `g * 2 * voxel_size`. Marching consumes it directly; there is no resampling and no transform beyond the doubled spacing
- [x] 1.2 Therefore a thin plumbing change, not work in `mesh_bricks`. What was missing was level-aware SAMPLING and ENUMERATION on `BrickCache`, not a mesher that understands mips

## 2. Level-aware brick cache

- [x] 2.1 `BrickCache::sample_lod(lod, key, i, j, k)`; `sample()` is it at level 0, unchanged. A level holding no brick for the key answers `+band`, as a never-evaluated brick does, so a lattice walk over any level is total
- [x] 2.2 `BrickCache::surface_bricks_lod(lod)` — `surface_bricks()` at level 0, the built mips at level 1, empty above. So "level 1 holds no bricks" and "level 1 was never built" are one enumeration rather than two
- [x] 2.3 `BrickCache::mip_count()`, O(1), so a caller can ask whether a level exists at all without enumerating it

## 3. Meshing a level

- [x] 3.1 `mesh::mesh_bricks` gains a trailing `int lod = 0`: spacing `voxel_size << lod`, samples through `sample_lod`, default key set through `surface_bricks_lod`. Every existing call site compiles unchanged
- [x] 3.2 Straddler collection takes the level too — `shell_cells` tests owners with `find_lod`, so a subset stays a filter of the whole mesh AT THAT LEVEL rather than borrowing cells from another
- [x] 3.3 A level outside [0, 1] returns an empty mesh in-engine and is refused at the C boundary — no clamping anywhere
- [x] 3.4 Field attributes are skipped above level 0 in `mesh_bricks` and refused at the C boundary, with the reason (the culled tape's bit-identity argument needs a vertex on the FIELD's surface)

## 4. C ABI

- [x] 4.1 `clay_brick_cache_mesh_lod` with `int32_t lod` immediately before the key list it reinterprets, the position it holds in `clay_brick_cache_read_bricks`
- [x] 4.2 `clay_brick_cache_mesh` keeps its signature and forwards at lod 0 — ONE shared body, so the two agree by construction and not by two implementations happening to match
- [x] 4.3 `lod > 1` (and a negative one) is `CLAY_ERROR_INVALID_ARGUMENT`, the rule `close-webgpu-host-abi-gaps/design.md` states for `read_bricks`
- [x] 4.4 An unbuilt level is `CLAY_ERROR_NOT_FOUND`: a named coarse key with no mip, and a whole-level request against a cache that holds surface bricks but not one mip. An EMPTY cache still meshes empty at every valid level, since there is nothing to be mistaken about
- [x] 4.5 Colours and gradient normals at lod 1 are `CLAY_ERROR_INVALID_ARGUMENT` — mirroring `read_bricks`' colour refusal and extending it to gradients for the culled-tape reason. `CLAY_NORMAL_FACE` works at every level
- [x] 4.6 Header prose: what the mip lattice is, what the key list means at each level, and each refusal with the failure it prevents
- [x] 4.7 `python3 tools/check_c_abi.py build/cpu-only/libclay_shared.so` passes — the new symbol resolves and the header stays bindgen-clean. No pyclay counterpart: `check_binding_parity.py` already defers the whole `BrickCache` surface with its reason

## 5. Tests — `tests/unit/test_c_brick_lod_mesh.cpp`

- [x] 5.1 **REGRESSION** (`brick lod meshing: a built mip meshes, and meshes coarser`): build the mips, mesh at lod 1, get real geometry. On main this call cannot be expressed — there is no argument for a level. Measured 7032 triangles at lod 0 against 1656 at lod 1, a factor of 4.25, and the two meshes' bounds agree to within one coarse cell and are the sphere's own box
- [x] 5.2 Ranges partition the mesh at lod 1 exactly as at lod 0 — the level changes the lattice, not the bookkeeping
- [x] 5.3 An unbuilt level is refused, per key and whole-level; building that key makes the same request answer; a different unbuilt key is still refused, so the check is per key rather than "some mip exists somewhere"
- [x] 5.4 lod 2, 3, 17 and -1 are refused and no mesh is produced
- [x] 5.5 Gradient normals and colours succeed at lod 0 with a document and are refused at lod 1, so the test is about the LEVEL and not the parameters; face normals answer at both
- [x] 5.6 An empty cache meshes EMPTY at lod 0 and lod 1 rather than erroring
- [x] 5.7 The pre-existing refusals still hold on the new entry point: null cache/params/out_mesh, a key count without keys, ranges without keys, gradients without a document
- [x] 5.8 **DID NOT BREAK ANYTHING** (`brick lod meshing: lod 0 is the call clay_brick_cache_mesh always was`): whole-cache and key-subset meshing, with and without a document, compared BYTE for byte — positions, normals, colours, uvs, indices and the range array — between `clay_brick_cache_mesh` and `clay_brick_cache_mesh_lod(..., 0, ...)`, with mips built so the level path is live
- [x] 5.9 The same claim against MAIN rather than against a sibling: one fingerprint program linked in turn against main's `libclay_shared.so` and this branch's, same Release configuration. Surface-brick count, surface-key order, and the vertex count, index count and FNV-1a hash of every buffer of four lod-0 meshes (whole/face, whole/gradient+colour, subset/face, subset/gradient+colour) plus the range array — **identical, diff clean**
- [x] 5.10 Full suite green: `ctest --preset cpu-only` 4/4, `clay_unit_tests` 792 cases / 2 119 993 assertions, 0 failed
- [ ] 5.11 No save/load round-trip: the brick cache is not serialized and this change touches no format. Recorded rather than skipped silently

## 6. Documentation

- [x] 6.1 `docs/08-mesh-readback.md`: the route table gains the coarse row, and the attribute table records that gradient normals and colours are level 0 only
- [x] 6.2 `docs/RELEASE.md` unreleased notes: additive, one new symbol, no signature changed
- [x] 6.3 `openspec/ROADMAP.md`

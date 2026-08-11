# Proposal: the brick cache is a GPU upload path that stops one step short

## Why

Issue #43 is a specification review of the C ABI by ClaySpaceDesktop, a Rust
desktop sculpting app driving claycore through `clay.h` and rendering with
`wgpu`. Its finding is worth repeating in full because it is a compliment with
a bug attached:

> `clay_brick_cache_read_bricks` is almost exactly what a GPU host wants […]
> `out_halves` can be an MTLBuffer's contents and the upload is one memcpy with
> no packing pass […] A uniform brick is FILLED with the band value of its sign,
> so an uploader never branches.

That is the design working. A host can take `clay_brick_cache_surface_bricks`,
read the payloads, upload them as a sparse `r16float` 3D atlas — hardware
filterable in WebGPU — and sphere-trace it with a brick DDA in its own shading
language. No kernel math in the shader, therefore **no drift risk**, which is
the whole problem `docs/06-host-gpu-previews.md` exists to solve, solved more
cheaply than by compiling our dialect.

The path is 90% shipped and the missing 10% is what this change is:

1. **A host cannot render colour.** `read_bricks` returns distance only, and the
   header says why — "gradient normals and colours are attributes of the FIELD".
   True of a mesh, but the consequence is that the representation *designed to
   be uploaded* is the one representation from which palette-indexed voxel
   colour is unreachable. A host that wants colour must mesh, which is exactly
   the path the brick volume was supposed to replace.
2. **A host cannot filter across a brick boundary.** `read_bricks` writes exactly
   `dim³`. Hardware trilinear filtering needs a one-voxel halo, so a host either
   fetches neighbours manually in the shader at every brick edge, or reads
   neighbour keys and repacks tiles on the CPU — throwing away the one-memcpy
   property the call was engineered for.
3. **`clay_brick_cache_mesh` meshes everything or nothing.** The dirty set exists
   so "a brush dab re-evaluates the bricks its influence bound reaches, not the
   model", and that locality holds right up to meshing, where it is discarded:
   there is no key list and no region, so seeing a dab costs a full re-mesh. On
   a 2.3M-triangle bust that is not a 50 ms operation.
   `clay_brick_cache_take_dirty` already hands out precisely the key list the
   call will not accept.
4. **Mesh output is deinterleaved and engine-owned.** `clay_mesh_positions` /
   `_normals` / `_colors` / `_uvs` borrow into separate arrays. A GPU host wants
   one interleaved vertex buffer, so every re-mesh costs an interleave pass plus
   a copy into a mapped buffer — two CPU passes over geometry that was just
   produced, on the frame path. `read_bricks` already does the caller-owned-
   destination thing correctly, one section earlier in the same header.
5. **`clay_brick_cache_raycast` is single-ray.** Minor, and named here only
   because it is the one remaining place where the cache section lacks the
   batched form the document section has as `clay_raycast_many`.

The issue's own conclusion is the cheapest route for both sides and this change
takes it: **if colour and apron land, the mesh path stops being on the frame
path at all** — meshing then runs for export, where it is already correct and
unhurried. Subset meshing is delivered anyway, because a host that wants a
triangle viewport should not be forced onto the volume path to get incremental
display, and because `take_dirty` producing a key list that `mesh` refuses is an
inconsistency inside one section of one header.

## What changes

- **Brick colour.** An opt-in per-brick RGBA8 lattice carried alongside the fp16
  distances: produced by the same evaluation, submitted through the same call,
  read back through the same fixed-stride contract, uploadable as `rgba8unorm`.
- **Brick apron.** An optional dilation on brick readback: a brick padded by N
  voxels sampled from its neighbours, so an atlas tile is directly filterable
  and the padding is computed once in the library rather than repacked by every
  host.
- **Subset meshing.** A key list on `clay_brick_cache_mesh` (NULL keeping today's
  "all surface bricks"), plus the per-key vertex and index ranges a host needs to
  patch sub-ranges of a GPU buffer instead of rebuilding it.
- **Caller-owned interleaved vertex/index copy-out.** A vertex layout descriptor
  and a copy entry point, so a mesh reaches a mapped GPU buffer in one pass in
  the host's own layout.
- **Batched brick raycast**, mirroring `clay_raycast_many`.

## What this change does not do

- **No device injection and no external memory.** Zero-copy interop is issue #43
  item 6 and is a change to who owns a backend's device, not to what crosses the
  boundary. It is proposed separately as `add-device-interop`.
- **No tape export.** Item 5 is `add-tape-abi-export`, already an open change.
  Nothing here needs it: the brick-volume path is deliberately the route that
  does *not* require a host to evaluate our kernels.
- **No colour on the mip.** A level-1 mip subsamples distances; averaging colour
  across a 2×2×2 block is a filtering policy, and picking one silently is worse
  than reporting that lod 1 has no colour lattice. Stated, not guessed.
- **No caller-owned destination for brick *evaluation*.** `read_bricks` already
  writes into caller memory; the intermediate float buffer that
  `clay_brick_cache_eval_requests` fills is the engine's contract with itself.
- **No change to how meshing welds.** Subset meshing produces the same triangles
  the full mesh produces for the same cells; only the vertex sharing across the
  subset boundary differs, and that is a duplicated seam vertex, never a crack.

## An announced ABI break

`clay_brick_cache_mesh` and `clay_brick_cache_read_bricks` gain parameters
rather than acquiring `_subset` and `_apron` siblings, following the precedent
`clay_mesh_load` set in `close-c-abi-issue-gaps`: two entry points differing by
one nullable argument would be two ways to say one thing.

`docs/RELEASE.md` requires that a break never be silent. Both arities change, so
a caller compiled against 0.22 gets a **compile error** rather than a misread —
there is no way for old code to link and behave differently. Announced here and
in the release notes.

`clay_brick_config` gains a `colors` field, which is additive: the `struct_size`
prefix rule covers it, and a caller compiled against the older layout keeps the
distance-only cache it already had.

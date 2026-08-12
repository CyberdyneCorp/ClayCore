# Proposal: brick-mesh field attributes go through per-brick culled tapes

## Why

Issue #73. `clay_brick_cache_mesh` with gradient normals evaluated the WHOLE
document tape at every vertex, so re-meshing a fixed set of bricks grew
linearly with the total document: 4.8 ms at 1 node to 120.1 ms at 193 on the
reporter's 80-brick set, while meshing the identical bricks with normals off
stayed flat at 4.3 ms and `clay_brick_cache_eval_requests` over the same
bricks was flat too — the culling the brick cache exists for reached refill
but not the attribute pass. The nodes in the measurement were two world units
from the measured bricks, an order of magnitude beyond any influence bound,
and moving them on top of the bricks changed nothing: the cost was a function
of how many nodes the document held, not of how many could affect the region.

On an interactive host this is a sculpting session getting slower the more it
is sculpted, without bound and without touching more geometry — the
reporter's dab latency left its budget at 13 ms fresh, 103 ms after 192 dabs,
with a constant 125 bricks re-meshed throughout.

## What changes

Gradient normals and vertex colours of a brick mesh are evaluated through
per-brick CULLED tapes — the same `cull_region` culling the refill path uses —
instead of the whole document's tape. Vertices are grouped by the brick that
owns their position, and each group is evaluated against a tape culled to
that brick's band-dilated region, so the attribute cost follows the bricks
being meshed rather than the size of the document.

Quality is unchanged, not traded: inside a brick's band-dilated cull region
the culled tape's band-clamped results are bit-identical to the full tape's
(the guarantee refill already relies on), mesh vertices sit on the surface
where |d| is far inside the band, and the gradient's tetrahedron taps move
`gradient_eps` — orders of magnitude less than the band, and the cull region
is additionally dilated by it. So the normals and colours equal a full-tape
evaluation on every vertex.

Engine-side, `mesh_bricks` now takes the `scene::Document` for attributes
instead of a pre-compiled tape, since a caller-supplied whole tape is exactly
what cannot be culled per brick. The C ABI is unchanged:
`clay_brick_cache_mesh` already took the document handle.

Measured on the issue's shape (unit sphere, dim 8 / voxel 0.02 / band 3, the
80 bricks at the +x pole, dabs at the -x pole, medians): gradient normals
went 7.3 ms → 130.0 ms over 1 → 193 nodes before; 6.8 ms → 7.7 ms after,
against 6.0 ms with normals off. The scaling is gated in CI by the
`BM_MeshBricksGradGrownDoc` / `BM_MeshBricksGradFreshDoc` ratio.

A follow-up remains open (option 1 of the issue): computing gradients from
the cached fp16 brick samples by finite difference would let a host ask for
normals without passing a document at all. That changes normal quality
(fp16-quantized banded samples against the exact tape gradient) and so needs
its own measurement before it can be the default; culling preserves the
existing normals bit for bit and removes the O(document) cost now.

## Impact

- Affected specs: `meshing`
- Affected code: `include/clay/mesh/marching.h`, `src/mesh/marching.cpp`,
  `bindings/c/clay_c.cpp` (passes the document through), `bindings/c/clay.h`
  (docs), `benchmarks/bench_main.cpp` + `tools/check_bench.py` (ratio gate)
- No C ABI change; `mesh_bricks`' engine signature swaps the tape pointer for
  a document pointer

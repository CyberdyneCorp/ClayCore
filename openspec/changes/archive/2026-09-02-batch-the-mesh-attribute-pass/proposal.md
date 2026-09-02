# Proposal: the non-brick meshers' attribute pass evaluates one vertex at a time

## Why

`apply_tape_attributes` walks the tape once per vertex for the colour and four
more times for the gradient, on one thread. It is the attribute pass for every
non-brick mesher — `mesh_tape`, `mesh_tape_dc` and the dual-grid path — reached
from the C ABI at `clay_c.cpp:1637` and from Python at
`pyclay_module.cpp:1339`. It is the export path.

x86-64 release, twelve cores, a sculpted sphere. `mesh_tape` with
`colors = true, normals = Gradient` against the same call with
`colors = false, normals = Face`:

| | mesh, no attributes | + colour + gradient | the attribute pass |
|---|---:|---:|---:|
| 1,199 instrs, 141,942 verts, voxel 0.02 | 334.8 ms | 10,168 ms | **9,833 ms (97%)** |
| 3,999 instrs, 252,654 verts, voxel 0.015 | 2,524 ms | 60,706 ms | **58,182 ms (96%)** |

`mesh_bricks` had the same disease and #73 fixed it: group the vertices, hand
the whole set to `eval_points_batch`, scatter back. That gets the thread pool
and the blocked evaluator #207 built. `apply_tape_attributes` never got it, and
has no backend lookup at all.

## What

The pass builds the point array once and asks the CPU backend for the colours
and gradients together, then scatters them back — the same shape `mesh_bricks`
uses, minus the per-brick culling, which needs a `Document` this signature does
not have.

The serial walk stays as the fallback for a build with no CPU backend, and
becomes the definition the batched path is tested against.

## Impact

| the attribute pass | before | after | |
|---|---:|---:|---|
| 141,942 verts, 1,199 instrs | 9,833 ms | **167 ms** | 58.7x |
| 252,654 verts, 3,999 instrs | 58,182 ms | **995 ms** | 58.5x |

| the whole `mesh_tape` call | before | after | |
|---|---:|---:|---|
| voxel 0.02 | 10,168 ms | **508 ms** | 20.0x |
| voxel 0.015 | 60,706 ms | **3,459 ms** | 17.6x |

Byte-identical: 0 of 141,942 and 0 of 252,654 normals differed, and 0 colours,
compared as float bits.

## One gate comes out

`check_bench.py` gated `("BM_SurfaceNets", "BM_MeshTape")` for the meshing
spec's "preview is cheaper than marching" claim. Both benchmarks mesh with the
default options — colour and gradient normals — so both were paying the serial
attribute pass, and surface nets emits **3.2x fewer vertices**. The pair was a
vertex-count comparison wearing a stopwatch.

| `bench_document`, voxel 0.02 | marching | surface nets | |
|---|---:|---:|---|
| geometry only | 82.5 ms | 138.3 ms | nets **1.68x slower** |
| + attributes, before | 500.7 ms | 275.0 ms | nets 1.8x faster |
| + attributes, after | 91.0 ms | 144.4 ms | nets 1.59x slower |

The mesher is slower, so the pair cannot be made to hold by re-scoping it to
measure geometry — that is the comparison it loses. It comes out of
`FASTER_THAN`, with the measurement left in the file where it used to be, and
#304 carries what the spec should say instead. Nothing regressed: surface nets
is faster in absolute terms than it was (275.0 ms to 144.4 ms), only less
flattered.

## Non-goals

**Per-brick culling for the non-brick meshers.** `mesh_bricks` culls a tape per
brick, which is where its independence from document size comes from. This
function is handed a `Tape`, not a `Document`, so it cannot compile a culled one
without a signature change — and the bit-identity argument for culling rests on
a vertex sitting inside a brick's band-dilated region, which a caller-chosen
region does not supply. The 58x above is pooling and blocking alone.

**The meshing itself.** With the attribute pass fixed, `mesh_lattice_parallel`
becomes the larger half of a coloured mesh (2.5 s of the 3.5 s at voxel 0.015).
That is a different function and a different question.

# Proposal: mesh bricks in parallel

## Why

`clay_brick_cache_mesh` costs a flat **~0.11 ms per brick** whether one brick is
requested or 343 (#111, measured by a host on macOS with Metal active). Flat
means serial: work spread across a pool gets *cheaper* per unit as the batch
grows, and this does not.

The inversion is what makes it urgent. Refill takes 20.5× from the Metal
backend; meshing takes nothing and is now **56× the cost per brick**. A bake
stroke re-meshes 3,080 bricks — **143 ms**, which is not slow, it is stopped.
Of a median dab's 3.1 ms, **1.8 ms is `clay_brick_cache_mesh`** and 0.6 ms is
everything else the engine does.

## What

`mesh::mesh_bricks` marches bricks concurrently and welds them serially.

### The split is forced, not chosen

#119's inventory called this row *"nothing structural: marching tets per brick
is pure; concatenate per-brick buffers in key order"*. **That is wrong for this
implementation**, and the reason is the sentence already in the source:

> ONE builder for every brick: lattice-edge welding spans brick seams, keeping
> the result watertight across the sparse set.

The `Builder`'s vertex map is shared mutable state that every brick touches, and
it exists so a lattice edge shared by two bricks yields **one** vertex. Sharding
it per brick and concatenating would duplicate every seam vertex and open the
mesh along every brick boundary — the exact defect the shared builder was
written to prevent.

So:

- **Phase one, parallel:** each brick marches into its own `ShellCollector` —
  the recorder the straddler pass already uses, described there as *"no welding,
  since every recorded edge is re-emitted through the Builder that welds"*.
  Marching is pure: it reads the cache through `sample_lod`, a const lookup and
  a half-to-float, and writes only its own output.
- **Phase two, serial:** replay the recordings through the single `Builder`, in
  key order, calling `edge_vertex` in the same sequence the serial loop called
  it.

### Why that is byte-identical rather than merely equivalent

The `Builder` sees an **identical call sequence**, so it produces an identical
vertex array, an identical index array and identical ranges. Not a tolerance, not
a re-derivation — the same object receiving the same calls in the same order.

Repeated `edge_vertex` calls for one lattice edge (several tets sharing it)
dedup in the builder exactly as they did when the march made them directly.

## Not this

- **Not a change to any output.** Verified against `main` on the same fixture:
  same vertex count, triangle count, range count, and the same hash of positions
  + indices + ranges.
- **Not the attribute pass**, which `batch-mesh-attribute-taps` already threads
  through `eval_points_batch`.
- **Not the straddler collection**, still serial. It runs once per subset call
  and is not the measured cost.
- **Not a device measurement.** The numbers here are a 24-core desktop. The
  interactive claim in #111 is an iPad claim and needs the device gate.

## Impact

- `meshing` spec gains the parallelism requirement and the byte-identity bar.
- No API change, no host burden: a caller that already serialised its calls, as
  it must, does nothing.

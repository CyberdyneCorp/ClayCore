# Proposal: the Metal path pays a full upload and three allocations per call

## Why

Metal is the iPad app's production path, and every dispatch through it does
this:

```cpp
// backends/metal/metal_backend.cpp
MTL::Buffer* pts  = copy_in(q.points_xyz, ...);            // allocate + copy
MTL::Buffer* dist = device_->newBuffer(...);               // allocate
MTL::Buffer* cols = device_->newBuffer(...);               // allocate
TapeBuffers tb    = upload_tape(tape);                     // allocate + copy the WHOLE tape, x3
bool ok = dispatch(...);                                   // commit, then waitUntilCompleted
std::memcpy(out.distances, dist->contents(), ...);         // copy back
release_all({...});                                        // free all six
```

Three costs, none of which is compute:

**The tape is re-uploaded on every call.** Instructions, params and blob, in
full, whether or not the document changed since the last dispatch. A preview
frame that raycasts and then evaluates has uploaded the same tape twice.

**Six buffers are allocated and freed per call.** `newBuffer` is not free, and
this happens on the interactive path at frame rate.

**Every dispatch blocks the calling thread** on `waitUntilCompleted`, then
copies results back out of shared memory that the caller could have been handed
directly. On unified-memory hardware — every device this backend runs on — the
copies at both ends are avoidable outright: `newBuffer(bytesNoCopy:)` wraps
caller memory, and `contents()` is already the CPU-visible pointer.

The 0.24.0 brick measurement is the receipt. Metal costs 288 µs per 8³ brick
against the CPU's 114 µs and "never wins at any thread count", with the
crossover at 16³. That was read as "a brick is too little work to cover a
dispatch", which is true, but the dispatch it has to cover is this one — an
allocation storm and a full tape upload for 512 samples. A cheaper dispatch
moves the crossover down, and where the crossover sits decides which backend the
sculpting path uses.

Two further things the code does that the spec does not:

- **`caps().device_meshing` is `false`** and `mesh()` routes through
  `grid_mesh`, triangulating on the host. `evaluation-backends` says the Metal
  backend "SHALL implement the full backend interface including `eval_bricks`
  and on-device meshing".
- **Gradients silently fall back to the CPU.** `gradients_from_taps` calls
  `eval_points_reference` for the whole batch. The comment says gradients are
  "rarely requested on the GPU path", which holds for brick fills and does not
  hold for anything drawing a shaded preview.

## What changes

**Tape residency.** The tape's three buffers are uploaded when the tape changes
and reused when it does not, keyed on the same revision the C ABI's tape cache
already maintains. A dab that dispatches 24 bricks against one document uploads
one tape.

**A buffer pool.** Point, result and color buffers are taken from a per-backend
pool sized to the largest recent request instead of allocated per call.

**No copy where the hardware does not need one.** Inputs wrapped with
`bytesNoCopy` where alignment permits, results read from `contents()` in place,
with the copying path kept for the cases where the caller's memory cannot be
wrapped.

**Gradients on the device**, removing the whole-batch CPU fallback.

**`device_meshing` becomes honest**: either meshing runs on the device, or the
spec is corrected to say the Metal backend triangulates on the host and the
capability flag says so. The current state — a spec that says one thing and a
flag that says another — is the only outcome not allowed.

## What it is not

**Not an async backend interface.** `Status eval_points(...)` returning filled
buffers is synchronous for every backend, and making it asynchronous is a change
to the interface all four implement, with a completion and ordering contract to
design. Worth doing; not this. This change removes the cost *inside* the
synchronous call and leaves the shape alone.

**Not a change to any value.** Metal and the CPU agree exactly over a full brick
fill today (max abs difference 0.0, measured). That is the acceptance test.

**Not a re-tuning of the 64-thread threadgroup cap** or the dispatch geometry.
Those are real questions and they are separable; mixing them in makes it
impossible to attribute the improvement.

## Open questions

- **What keys tape residency.** The C ABI's revision counter is right there, but
  the backend interface takes a `const scene::Tape&` and knows nothing about
  documents. Whether the tape grows an identity, or the backend hashes what it
  was given, or the residency lives one layer up, is the design decision.
  Hashing a tape per dispatch to avoid uploading it is a trade, not a win, and
  must be measured before it is chosen.
- **Whether `bytesNoCopy` is usable in practice.** It requires page-aligned,
  page-multiple allocations. Caller buffers are ordinary arrays, so this may
  only apply to buffers the library owns.
- **Where the crossover lands afterwards.** The 16³ number is a property of the
  current dispatch cost. It must be re-measured, and `docs/RELEASE.md`'s advice
  to keep brick fills on the CPU re-examined against the new number.
- **Whether on-device meshing is worth it at all**, given that the host-side
  triangulation is shared with every other backend through `grid_mesh` and works.

## Impact

`evaluation-backends`' Metal requirement gains residency and allocation
statements and has its meshing claim reconciled with reality. No public
signature changes; no output values change.

# Design: cancel a long operation, and report its progress

## The shape of the surface

Three entry points and one opaque handle:

    clay_cancel_token* clay_cancel_token_create(void);
    void               clay_cancel_token_cancel(clay_cancel_token*);
    void               clay_cancel_token_destroy(clay_cancel_token*);

plus one output descriptor read by the host:

    clay_cancel_token_progress(const clay_cancel_token*, clay_progress* out);

and one new value on the result enum, appended to nine existing ones:

    CLAY_ERROR_CANCELLED = 9

An operation that accepts a token takes it as its last parameter and accepts
`NULL` to mean "no cancellation", so every existing call site keeps compiling
and behaving identically.

## Why a token and not a callback

A progress callback is the obvious design and it is the wrong one here.

**`clay.h` has no function pointers.** Not "few" — the grep for `(*name)(` over
the whole header returns nothing. Every consumer today marshals plain data.
Introducing the first callback means every FFI consumer gains a problem it does
not have: a C# host must pin the delegate against its GC for the life of the
call, a Rust host must guarantee no panic unwinds across the boundary, and a
Swift host must decide what a `@convention(c)` closure may capture. The `c-abi`
capability's `FFI-general design` requirement exists to keep this class of
pattern out, and it names bindgen cleanliness as the test.

**A callback also inherits the threading contract's worst case.** The header
already requires calls on one handle to be serialized, const readers included.
A callback fires on whichever thread is inside the operation — with the pool
below, that is a worker thread, not the caller's — so the header would have to
grow a rule about what a progress callback is allowed to touch, and the honest
answer is "almost nothing". A token inverts that: the engine writes, the host
reads, and both sides are plain atomics.

**What the token costs instead** is that a host wanting a progress bar must
poll. That is what a host drawing at 60 Hz is doing anyway.

Two alternatives were considered and rejected in the proposal rather than here:
asynchrony (spawns threads the caller did not ask for and owns a completion
queue) and resumption (stores intermediate state whose lifetime and
invalidation the ABI would then own).

## Where the checkpoints go

These operations are already loop- and window-shaped, which is why this is
plumbing rather than a rewrite.

- `field::FieldVolume::sample_blocks` drives the bake through a fill function
  called **per window of bricks** (`src/scene/consolidate.cpp`,
  `fill_window`). The window boundary is the checkpoint, and the brick count
  is the progress denominator.
- `consolidate_layer` is visibly multi-phase: sample → `field::redistance` →
  `compact` → `fill_colors_blocks` (skipped when the layer holds one colour)
  → `measure_sample_lipschitz` → commit. Progress on it is not one fraction;
  it is a phase plus a fraction within that phase.
- `brush::mask_extrude` runs a separable exact Euclidean distance transform —
  three axis passes over the lattice (`src/brush/mask_extrude.cpp`) — then
  samples a volume. Each pass is a checkpoint, and at 4.4 s it is the case
  that most needs one.

**The check must be cheap enough to be invisible.** A relaxed atomic load per
window or per scanline, never per sample. `sample_blocks` already batches, so
the natural granularity is the one that exists.

## The thread pool needs no change, and one rule

`parallel::ThreadPool::parallel_for` joins by waiting for
`done >= num_tasks`, and `run()` increments `done` only after `job.fn(b, e)`
returns:

    job.fn(b, e);
    if (job.done.fetch_add(1, ...) + 1 == job.num_tasks) { ... notify ... }

So a cancelled chunk **must return normally**. It must not throw — an
exception unwinding out of `fn` skips the increment, `done` never reaches
`num_tasks`, and the issuing thread blocks on the condition variable forever.
That is a hang, not a slow cancel, and it is the single most likely way to get
this wrong.

The consequence is that cancelling a `parallel_for` does not stop it claiming
chunks: every remaining chunk is still claimed and still calls `fn`, which
returns immediately after its atomic load. That is fine — the cost is
`num_tasks` relaxed loads — and it means the pool itself needs no cancellation
concept at all.

## What a cancelled operation leaves behind

Nothing. Each of these builds a result and installs it at the end:
`bake_layer` returns an `optional<FieldVolume>` that `consolidate_layer`
commits through the command vocabulary. So cancellation is a discard, the
document is byte-identical to before the call, and the undo stack gains no
entry. This is a spec requirement rather than an implementation note, because
the alternative — a half-baked layer left in the document — is precisely the
failure a user pressing Stop is trying to avoid.

## Progress: what the number means

A fraction is a promise. `clay_progress` carries:

- a **phase index and count**, so a multi-phase operation is honest about
  being one;
- a **fraction within the current phase**, monotonic within that phase;
- a **unit count done and total** where the operation has an honest one
  (bricks, scanlines), and zero where it does not.

It deliberately does NOT carry a time estimate. The phases have very different
per-unit costs — `redistance` and a colour fill are not the same work per
brick — so a remaining-time figure derived from a fraction would be wrong in
the direction that annoys users most, and the host has the wall clock anyway.

## Open decisions

These are the ones with no defensible default yet, and they are the DECIDE
tasks in `tasks.md`:

1. **Which entry points take a token in the first slice.** The measured six
   are the obvious set. `clay_document_mesh` is the one that is not measured
   and is plainly an "export" by the harness's own definition.
2. **Whether progress is opt-in.** Writing progress costs two relaxed stores
   per checkpoint whether or not anyone reads them. Probably always on and
   unmeasurable; needs measuring rather than asserting, because this library
   has been caught by exactly that assumption before.
3. **Whether one token may serve two concurrent operations.** Forbidding it is
   one sentence and one owner field; allowing it means progress has no single
   meaning. Lean to forbidding.
4. **Whether a token is reusable after a cancel.** A host that keeps one token
   per document and cancels it once must be able to reset it, or every cancel
   costs an allocation on the interactive path.
5. **What pyclay does with it.** A context manager is the Python-natural
   shape; a thread that cancels it is not, and the GIL makes the polling story
   different from C. Parity is required, so this needs an answer before build.

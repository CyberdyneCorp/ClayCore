# Proposal: cancel a long operation, and report its progress

## Why

This library has a formal name for an operation that takes seconds.
`tests/device/Shared/LatencyHarness.swift:33` defines it:

    /// An explicit user action: consolidate, mask extrude, an export.
    case operation

It has budgets for them, it gates releases on them, and on the reference iPad
they are what the name implies. From `tests/device/baseline.json` (ABI 0.39.0,
iPad15,5, iOS 26.5.2), worst measured p95 across the growth axis:

| case | measured | its budget |
|---|---|---|
| `mask_extrude` | **4403 ms** | 6605 ms |
| `sdf_consolidate` | **661 ms** | 992 ms |
| `volume_relax` | **358 ms** | 536 ms |
| `volume_hpolish` | 153 ms | 229 ms |
| `volume_flatten` | 152 ms | 227 ms |
| `mask_extract` | 97 ms | 146 ms |

Every one of those is a single synchronous C call that a host enters and
cannot leave. There is no cancel, no progress, no chunked form, and no
estimate. `cancel`, `progress` and `interrupt` do not appear in
`bindings/c/clay.h` at all, and they do not appear in `openspec/ROADMAP.md`
either — this is not a deferred decision, it is a surface nobody has drawn.

**The engine already assumes the host solved it.** `src/scene/consolidate.cpp`
justifies charging a second full evaluation pass to the bake on the grounds
that consolidation "is an operation with progress UI rather than a frame". The
ABI makes that progress UI impossible to build.

Two things stop a host from working around it.

- **The threading rule closes the obvious escape.** `bindings/c/clay.h` states
  that calls on one handle must be serialized by the host, *const readers
  included*, and that `clay_brick_cache_eval_requests` is "NOT safe
  concurrently with a mutating `clay_document_*` / `clay_layer_*` call". So a
  host cannot run consolidate on a worker and read anything from the document
  on the main thread to drive a progress bar. It can start the call and it can
  wait.
- **A four-second unresponsive main thread is not only a bad frame on iOS.**
  It is inside watchdog territory, on the same platform and for the same
  reason `add-history-budget` argues about memory. The interactive-path work
  will make these faster; none of it makes them interruptible, and a sculpt
  large enough to need consolidation is exactly the one where the number is
  largest.

The roadmap's whole interactive-path section is about making operations
cheaper. This is about what the host does with the seconds that are left after
that work lands — and unlike a latency row, it does not get better with a
faster machine, because the operation grows with the document.

## What changes

**A cancellation token, not a callback.** `clay.h` contains **zero function
pointers today** — checked, not assumed. Adding the first one would be the
first callback any FFI consumer has to marshal, and the `FFI-general design`
requirement in `c-abi` exists to keep exactly that kind of pattern out. A
token is an opaque handle, three plain entry points, and no calling convention
to get wrong from C#, Rust or Swift.

- **An opaque `clay_cancel_token`**, created and destroyed by the host,
  passed into an operation that accepts one. Cancelling it is the ONE thing a
  host may legally do to a live operation from another thread, and the header
  says so — because the rest of the threading contract says the opposite about
  everything else.
- **Progress readable from the same token.** The engine writes how far it has
  got; the host polls it from its own thread when it wants to draw. No
  callback into host code, no reentrancy question, no rule about what a
  callback is allowed to call.
- **Cancelling leaves the document exactly as it was.** These operations
  already build a result and install it at the end — `bake_layer` returns a
  `FieldVolume` that `consolidate_layer` then commits — so a cancelled
  operation discards and returns. This is a guarantee, not an implementation
  note: a cancelled consolidate must not leave a half-baked layer.
- **`CLAY_ERROR_CANCELLED`**, appended to a nine-value enum, so a host can
  tell "the user stopped it" from "it failed". A cancellation is an ordinary
  outcome of an interactive session, the same framing
  `clay_brick_cache_submit` already uses for a rejected submit.
- **The operations that carry the token first** are the ones the table above
  measures, plus meshing an export, because those are what a user waits on.

## What this is NOT

**Not asynchrony.** No threads are spawned, no completion queue, no futures.
The call still blocks the thread that made it; what changes is that a second
thread can end it early and read how far it got. A host that wants the call
off its main thread already owns that decision, and this is what makes the
decision usable.

**Not a time budget.** `CLAY_ERROR_BUDGET_EXCEEDED` exists and means the host
declared a limit up front. Cancellation is a user changing their mind
mid-operation, which no budget can predict, and the two must not be confused
in a UI: one is "too big", the other is "stopped".

**Not resumption.** A cancelled operation is discarded, not checkpointed.
Resumable work means storing intermediate state whose lifetime and
invalidation the ABI would then own, and nothing here needs that: the answer
to a cancelled consolidate is to run it again, possibly at a coarser cell.

**Not a progress estimate for gesture- or interactive-class calls.** A stamp
is 5.8 ms and a progress bar on it is noise. The token is refused, not
ignored, where it would be meaningless — a host that passes one everywhere
should be told which calls actually honour it.

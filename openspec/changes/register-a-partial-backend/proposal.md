# Proposal: one failing pipeline should not silently delete a backend

## Why

Issue #63, second half. The first half — `clay_raycast` failing to compile on
Apple Paravirtual GPUs — was fixed in 0.27.3 by marking `TapeField::operator()`
`noinline`. What remains is the part the issue called "arguably the more
serious of the two, and independent of the first":

```cpp
return pso_points_ && pso_grid_ && pso_grid_batch_ && pso_rays_;
```

A device that can perfectly well run `eval_points` and `eval_grid` gets **no
Metal at all**, and `clay_list_backends` answers `cpu` — indistinguishable from
a build with no Metal compiled in. The host cannot tell "this machine has no
GPU backend" from "this machine has one and it was thrown away over one
kernel".

Two things have changed since the issue was written, and both make the case
stronger rather than weaker:

1. **The gate got wider.** 0.29.1's batched refill added `pso_grid_batch_` to
   that conjunction. Every kernel added to the accelerated path is now another
   way to lose the whole backend, which is exactly the failure mode the issue
   predicted for "the next kernel that fails on any device".

2. **Nothing in the shipped ABI reaches Metal's raycast.** `clay_raycast` and
   `clay_raycast_many` both call `Registry::find("cpu")` explicitly
   (`bindings/c/clay_c.cpp:3198`, `:3222`) — they are the only two
   `Backend::raycast` call sites in the library. So today the backend is
   discarded over a kernel that **no C ABI entry point can call**.

## What

**A backend registers when its core operations work, and reports the ones it
cannot run.** The interface already has both halves of this and neither is
being invented here:

- `Status::Unsupported` is documented as "capability not provided by this
  backend", and the spec's existing "Capability flags honored" scenario already
  describes a backend refusing `mesh()` and the caller falling back.
- `tests/unit/test_parity.cpp:626` already reads
  `if (s == eval::Status::Unsupported) continue;` — the parity suite was
  written for backends that cannot raycast.

So: `BackendCaps` gains a flag per operation; Metal registers when
`eval_points` and `eval_grid` build; a pipeline that did not build makes its
operation report `false` and return `Unsupported`. `eval_grid_batch` is a
special case worth naming — the base class already provides a loop over
`eval_grid` with identical results, so a failed batch kernel costs speed and
nothing else.

**And the failure becomes visible through the ABI**, which is the minimum the
issue asked for. Two additive calls:

- `clay_backend_supports(name, op, &supported)` — per-operation, which only
  becomes a meaningful question once a backend can be partial.
- `clay_backend_diagnostic(name, buffer, &size)` — why a backend is absent or
  degraded, empty when it is neither. This is what makes `cpu` distinguishable
  from `cpu` — a host on a paravirtual machine can now print the reason instead
  of guessing at it, and the reason is the compiler's own log, which #59
  already captured but sent only to stderr.

## What this is not

**Not a change to what any existing call computes.** Adoption of a partial
backend changes where work runs, never results — the same rule
`make_backend` already states.

**Not a promise that every backend becomes partial-capable.** CUDA, OpenCL and
Vulkan keep their current all-or-nothing initialisation; they gain the
diagnostic sink for free when they choose to report into it. Metal is the one
with a demonstrated device class that fails one kernel.

**Not a route to shipping a broken backend quietly.** The opposite: a backend
that registers partially says so in `caps()` and in the diagnostic, where today
a backend that fails entirely says nothing an ABI caller can read.

## Open question, settled here rather than left implicit

**Should a partial backend register under the same name?** Yes. A host asks for
`metal` to get acceleration, and per-operation refusal is already how this
interface expresses "not from me" — a second name (`metal-partial`) would make
every host string-match on a name it has never seen, which is a worse failure
than an `Unsupported` it already has to handle. The decision is recorded in the
spec rather than in a comment, because it is the one a future backend author
will need.

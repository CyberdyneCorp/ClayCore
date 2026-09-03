## Why

`add-sdf-prefix-cache` shipped the fix for the cold-window problem and left it
**unreachable from a host**. Task 17.3 says so in as many words: "THE CACHE
ACROSS THE ABI. Deliberately not guessed at here … The preview delta is the only
C surface in it."

The engine side works, and the benchmark says by how much (`build/release`, load
1.26 before / 5.70 after, `--benchmark_min_time=1x`):

| fixture | full walk | with a prefix | |
|---|---:|---:|---:|
| spread 5,000 items | 89.8 ms | 2.32 ms | 38.7x |
| spread 20,000 items | 242 ms | **2.32 ms** | **104x** |
| piled 20,000 items | 245 ms | 2.33 ms | 105x |

**2.32 ms at 5,000 items and 2.32 ms at 20,000** — the accelerated dab is flat
in history, which is the whole property. `suffix_roots` is 64 in every row and
`fallback_windows` is 0.

None of it is reachable through `bindings/c/`. `clay_sdf_smooth_begin` calls
`SdfSmoothTransaction::begin` and lets the `cache` argument default to null, so
**every C and Python host pays the full walk on every cold window**, at 242 ms
per dab on a 20,000-item layer. The C++ API has had the fast path since #371;
nothing that ships to a host can ask for it.

## What Changes

- **A host-owned cache handle.** `clay_sdf_prefix_cache`, created and destroyed
  by whoever wants one. The ownership question 17.3 deferred is already answered
  inside the library and this adopts that answer rather than inventing one:
  `clay_brick_cache`'s header states it — "a cache belongs to whoever made it,
  never to a document" — so this takes no document, and destroy returns void.
- **`clay_sculpt_policy` grows the three cache knobs**, because that is exactly
  how the C++ policy is shaped: `SdfSculptPolicy` NESTS an `SdfPrefixPolicy` and
  `begin` overwrites its `cell_size`/`band`/`padding` from the sculpt policy's
  own. Mirroring that is not a convenience — the cache is keyed on resolution,
  so a host that could set the two independently could build a cache that never
  hits and get no error and no acceleration. A grown descriptor is compatible by
  `struct_size`, which is the mechanism that exists for this.
- **`clay_sdf_smooth_begin_cached`**, taking the cache beside the policy.
- **The build, exposed**, since `SdfSourceField::open` deliberately never builds:
  `clay_sdf_prefix_cache_build` is the door for a host that has somewhere to put
  the work, plus `clay_sdf_prefix_boundary_for` so it can ask whether a layer is
  worth caching before paying for one.
- **Budget and diagnostics**: `set_max_bytes`, `invalidate_layer`, `clear`, and
  the stats a host needs to tell "fast because the cache worked" from "fast
  because the test touched the same window twice".
- **The measured scheduling note (17.2)**, which had been "answered only in
  prose" because the benchmark it needed did not exist. It does now.

## Capabilities

### Modified Capabilities
- `c-abi`: a new requirement that the prefix cache is reachable, host-owned and
  budgeted, and that its sampling cannot silently disagree with the transaction
  that consumes it.

## Impact

- `bindings/c/clay.h`, `clay_internal.h`, `clay_c.cpp` — the handle, the grown
  policy, the six entry points.
- `bindings/python/` — parity.
- `tests/c_api/`, `tests/unit/`, `bindings/python/tests/`.
- `docs/05-claycore-library.md`, `docs/09-brush-latency-and-coverage.md` — the
  scheduling note and the numbers behind it.
- **ABI grows: 0.78.0 -> 0.79.0** in `CMakeLists.txt`, `bindings/c/clay.h`,
  `pyproject.toml`.

**Not in this change: 17.4, a boundary inside a group** — and the reason is
sharper than "deferred". A cached prefix stores ONE SCALAR per sample. A
boundary inside a group leaves a STACK of pending combines, which is why
`TapeCheckpointFrame` and `checkpoint_stack_levels` exist for the brick-resume
path, whose seed is a stack. A sampled volume cannot hold one. Closing 17.4
means deciding what a volume stores at a group boundary, and that is a design
question, not an extension of this one.

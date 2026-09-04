## Why

`test_mesh_import.cpp`'s "the tree is what makes it affordable" gates a real
property — summarizing works, so ten times the triangles do not cost ten times
the work — with a **wall clock**, and it has now failed twice on `macos-latest`
with the tree unchanged:

| | reading | bound |
|---|---:|---:|
| earlier failure (recorded in the test's own comment) | 5.379 | 5.333 |
| PR #456, attempt 1 and 2 | **5.459** | 5.333 |

Both sides of the ratio run in well under a millisecond, so one preemption
inside the coarse side inflates it. The test has already been hardened once for
this — `c51a8325`, "score the BVH shape check on its fastest pass, not one
sample" — and it was not enough.

**It failed PR #456 twice for no reason connected to that change.** An A/B on
identical hardware, six runs a side, put the branch at 2.254 and main at 2.246 —
0.4% apart, no effect. The assertion times `Bvh::signed_distance` over a mesh;
#456 touched the tape compiler and `FieldVolume`, neither of which it calls. The
likely mechanism is that adding a test file shifts the binary's layout enough to
cross a threshold sitting 2.4% away.

So the gate charges unrelated pull requests, and the next one pays it too.

## What Changes

**The property was never a duration.** Summarizing means the walk STOPS at a
distant node and answers with one dipole term instead of descending it, so what
it saves is nodes visited and triangles tested — integers, identical on every
machine. This is the discipline the rest of the tree already states, in
`test_extreme_poly_scaling.cpp`: "THE COUNTS, which are deterministic and are
the real gate ... THE TIME, last and with a wide band."

- `mesh::BvhWalkStats` — nodes visited, triangles tested, and subtrees
  summarized — reported through a borrowed pointer that is null by default.
- The gate asserts the WORK ratio. It reads **0.863x for 16x the triangles**
  against a bound of 5.333, where the wall clock read 5.459.
- A second case proves the gate catches its own regression: with summarizing
  disabled (`beta = 0`, the documented way, and what the approximation is
  already compared against two cases up) the same ratio is **16.016x** — the
  triangle ratio exactly.

**Zero cost when nobody is measuring, and that took measuring too.** Counting
both walks cost **3.8%** of a `signed_distance` sweep with the pointer null;
counting only `winding_number` — where summarizing actually lives — cost
**2.3%**; compiling the counters out with `if constexpr` costs **0.4%**, which
is noise. A permanent charge on a query picking and meshing lean on, paid so one
test can assert a count, is not a trade worth making silently.

## Capabilities

### Modified Capabilities
- `meshing`: the claim that the tree makes an import affordable is gated on work
  done rather than on time taken.

## Impact

- `include/clay/mesh/bvh.h`, `src/mesh/bvh.cpp` — the counters and the templated
  walk.
- `tests/unit/test_mesh_import.cpp` — the gate, and its regression proof.
- No ABI change, no format change. `Bvh`'s public signatures are untouched.

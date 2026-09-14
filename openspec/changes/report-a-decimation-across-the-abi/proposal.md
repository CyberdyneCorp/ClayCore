## Why

**`mesh::DecimateReport` was C++ only, and the host that needs it is on the C
boundary.**

`grep -c DecimateReport bindings/c/clay.h` returned 0. Decimation is reachable
from C — `clay_mesh_params.decimate` — and every one of the eleven things the
report says about the call it performed stopped at the C++ API. A host reaching
claycore through the ABI set the flag, was handed a mesh, and had no way to be
told the simplification had pinched it.

The substitute it had is `clay_mesh_validation_report`, and **it answers a
different question**:

| | validating the result | the decimation report |
|---|---|---|
| the result carries a non-manifold edge | yes | `manifold` |
| decimation MADE it, or was handed it | **cannot say** | `input_manifold` |
| whether the retry ladder ran | **cannot say** | `attempts` |

The input mesh no longer exists by the time a host holds the result, so
`input_manifold` is not recoverable downstream at any cost. Attribution is the
whole point of the field — `mesh/decimate.h` says so — and it was the half that
did not cross.

## This is not an aggressive-ratio problem

Issue #575, reproduced through the C ABI in this branch's own test: a unit
sphere, one layer, one node, nothing sculpted, meshed at voxel 0.02 with
`CLAY_MESHER_MARCHING`, from an input with **no non-manifold edge at all**.

```text
20:clean 25:clean 30:clean 35:clean 40:clean 45:PINCHED 50:PINCHED 55:PINCHED
60:clean 65:PINCHED 70:PINCHED 75:PINCHED 80:clean 85:clean 90:clean 95:clean
                                                            6 of 16 pinched
```

Swept at 0.01 the same fixture is 31 of 76, in two contiguous bands (0.44–0.59
and 0.61–0.75). **0.50 is what an export panel puts in the slider by default.**
The v0.113.0 characterisation — that a pinch is what an aggressive ratio means —
does not explain this one, and issue #575 is right to say so.

## What lands

```c
typedef struct clay_decimate_report {
    uint32_t struct_size;
    int32_t manifold;        /* no edge of the RESULT carries more than two triangles */
    int32_t input_manifold;  /* only when manifold is 0 */
    int32_t attempts;        /* simplifications actually run */
} clay_decimate_report;

clay_result clay_mesh_decimate_report(const clay_mesh* mesh, clay_decimate_report* out_report);
```

Taken on **every** decimated call rather than on request. A flag on
`clay_mesh_params` would be a field older hosts do not set, and the check has
already been paid for inside `decimate` itself — what crosses the boundary is
the answer it already had, not new work.

The report is held on the mesh HANDLE, like `clay_mesh_quad_report`'s, because
it describes a CALL rather than a surface. `clay_mesh_transform` and
`clay_mesh_transform_nonuniform` carry it — the same mesh, moved, with no index
rewritten, so no edge changes incidence. A mesh loaded from a file, borrowed
from a document layer, concatenated or meshed without `decimate` is **refused**
with `CLAY_ERROR_INVALID_ARGUMENT` rather than answered with a clean report:
"decimation ran and broke nothing" and "no decimation ran" are different facts,
and all-ones is indistinguishable from both.

## What this deliberately does NOT do

**It does not try to fix the pinch.** Four candidate fixes are refuted by
measurement in issue #575 and none is attempted here:

| candidate | refuted by |
|---|---|
| flag variations beyond the shipped ladder | both retries return a **byte-identical** mesh on this input |
| perturbing the input | 0, 31 and 58 pinched of 76, from three meshes of one sphere |
| extending #549's edge guard to the tape path | 58 of 76 — it nearly **doubles** the defect |
| a repair pass | the pinches are FLAT (four triangles within ~2 degrees), on a second independent fixture |

The collapse sequence is chaotic with respect to arbitrarily small input
perturbations, and the input itself moves with floating-point contraction —
281,568 triangles against 281,544 on the same sphere one compiler flag apart,
with 31 of 76 bad ratios on the first and 0 of 76 on the second. Detect and
report is the established answer. This change is about making that answer
reachable.

## What building it found

**A test that pins which ratios pinch cannot ship, and this one nearly did.**
`clay-desktop-team` already lost four Linux CI jobs to exactly that assertion,
and `tests/unit/test_mesh.cpp` records the same lesson from #567. So the sweep
asserts the AGREEMENT — `manifold` matches what the validator finds, on every
one of the sixteen ratios — and never the band. The count is a `MESSAGE`, not a
`CHECK`.

**Which left the pinched path itself unexercised on a toolchain that does not
pinch**, so the reporting path needed a fixture that is non-manifold by
CONSTRUCTION rather than by collapse order. `CLAY_MESHER_NETS` on a torus whose
minor radius is smaller than the voxel is one: surface nets places one vertex
per cell and the header already says it is not manifold. Measured here at voxel
0.05 with R=0.6, r=0.03: **624 triangles carrying 24 non-manifold edges, and no
boundary edge.** That case fires everywhere and is the one that pins
`input_manifold`.

## Verification

The test was proved by breaking the code under it, twice.

| the code under test | result |
|---|---|
| report plumbing removed from `mesh_tape` (the pre-change state) | **3 of 4 cases fail**, 45 assertions run, 3 failed |
| report derived from validating the RESULT — the substitute a host already had | **1 of 4 cases fails**: `input_manifold` is 1 where 0 is required |

The second is the one that matters. A report a host could have computed for
itself passes the sphere sweep and still fails, because the field it cannot
compute is the field the report exists for.

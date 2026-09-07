## Why

**The device gate measures growth over DOCUMENT SIZE. Nothing measured growth
over SESSION LENGTH, and an artist's session is hours of dabs on one model that
is not growing.**

`GrowthAxis.standard` is `[10, 100, 1000]` items, and a case fails when its cost
scales faster than `N^1.25`. That catches an algorithm that is not local. The
failures on the other axis — a leak, an unbounded cache, a history outgrowing
its budget, an arena that never converges, a fragmenting allocator — **do not
scale with the document**, so no case on the size axis can fail on them however
large it is made.

**And the fixed-mesh path had never run on an iPad at all.** `Coverage.swift`
exempted `mesh_sculptor_stamp` as unmeasured, in its own words: "the device
harness has no mesh-layer fixture, so no case drives a mesh brush yet. Named
here rather than left out of VERB_PATTERNS, which would make the whole family
invisible to this gate." The classical sculpting mode — the one the whole
`mesh/sculpt.h` contract exists for — was covered by nothing on hardware.

## What Changes

**A host-side gate that runs in CI on every commit**, because a gate that only
runs on a device runs rarely. `tests/unit/test_sustained_session.cpp` drives
2,160 dabs in three windows on one mesh and asserts the counts, the arena's
convergence and the history's bound. It costs 0.15 s.

**A device-side case**, `mesh_sustained_grab`, adding what only hardware gives:
p50/p95/p99 per window, the process footprint per window, and the thermal state
each was taken in. Its own bundle and its own process, because a case whose
subject is memory over time must not inherit another bundle's high-water mark.
Last in the run, at the warm end — where a new suite goes here anyway, and where
a SESSION belongs on its own terms.

**`Fixture.meshLayerPatch`** — the mesh-layer fixture the exemption named, which
closes it. The exemption is REMOVED rather than annotated: one that no longer
applies is a record of something nobody rechecked.

**A `DRIFT` verdict** in `check_device_bench.py`, beside `GROWTH`. The counts
carry it and the time carries a warning, because the counts are integers the
engine produced and the times are a warm iPad at the end of a seven-session run.

## What measuring refuted

**The fixture was wrong twice, and the numbers said so both times.**

1. **"Loop a Draw on a circle."** A sculpt fixture cannot hold its own workset
   constant while actually deforming, because the falloff is measured ALONG THE
   SURFACE IT IS CHANGING. The draw builds a bump, the bump lengthens the
   geodesic distance to the same world radius, and a later dab genuinely reaches
   **2.1x fewer vertices** over 27 revolutions. A gate reading that would be
   reading the geometry it was changing and calling it a session.

2. **"Alternate the draw's sign so it cancels."** It does not. A draw deposits
   along the region's AVERAGED NORMAL, which the deformation itself turns, so
   `+s` and `-s` do not cancel: over 82 revolutions the workset still fell by a
   third.

**A Grab does.** It displaces by `direction * weight` and names no normal, so
alternating the direction per revolution returns the surface. Measured over
29,520 dabs on a 37,249-vertex plane:

| window | p50 ms | p95 ms | considered | scratch | history B |
|---|---:|---:|---:|---|---:|
| early | 0.0941 | 0.1049 | 1,966,791 | converged | 249,688 |
| middle | 0.0945 | 0.1022 | 1,965,899 | converged | 249,688 |
| late | 0.0948 | 0.1037 | 1,965,254 | converged | 249,688 |

**0.08% count drift over 29,520 dabs**, p50 at 1.007x, and the history bounded
by what the stroke reached rather than by how many stamps it took — which is
`VertexDeltas`'s coalescing proving itself.

The residual 0.08% is float addition not cancelling exactly, not drift, and the
gate's 2% tolerance is set from it: wide enough that the residue cannot fail it,
narrow enough that the 2.1x a draw fixture produces could not pass.

## Capabilities

### Modified Capabilities
- `device-gate`: a case can be measured over session length as well as document
  size, and the fixed-mesh path is measured on hardware.

## Impact

- `tests/unit/test_sustained_session.cpp` — the CI gate.
- `tests/device/Shared/SustainedHarness.swift`, `tests/device/Sustained/` — the
  device case.
- `tests/device/Shared/Fixture.swift` — `meshLayerPatch`.
- `tests/device/Shared/LatencyHarness.swift` — `windows` on `CaseResult`,
  optional so every existing record still decodes.
- `tests/device/Shared/Coverage.swift` — one exemption closed, two corrected.
- `tests/device/project.yml`, `tools/run_device_bench.sh` — the seventh session.
- `tools/check_device_bench.py` — the `DRIFT` verdict.
- No ABI change, no format change.

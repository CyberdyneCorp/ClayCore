## Context

Three facts about the tree decide most of this design, and all three were
established by reading it rather than assumed:

1. **The metallib is compiled against the host SDK.** `CMakeLists.txt:110` and
   `:118` invoke `xcrun -sdk macosx metal` and `-sdk macosx metallib`, and the
   result is embedded into the library as a C array
   (`clay_metallib_embedded.cpp`) which `metal_backend.cpp:161` hands to
   `dispatch_data_create` at run time. Nothing about this is conditional on the
   target. An iOS slice built with `CLAY_BACKEND_METAL=ON` today embeds macOS
   AIR, and the failure surfaces as a backend that does not register — which is
   indistinguishable from one that was never enabled.
2. **The xcframework is CPU-only by design.** `tools/build_xcframework.sh`
   builds three slices, none with the Metal backend, and says so: "The Metal
   backend is wired per-app during Xcode integration." So even a correct iOS
   metallib would not reach a host through the shipping artifact.
3. **There is no Xcode project, XCTest bundle, or device test path at all.**
   `clay_bench` is a google-benchmark executable linked in
   `CMakeLists.txt:257-261`; it cannot be installed on an iPad.
   `tests/swift/smoke.swift` is a SwiftPM *executable* target, and CI only
   type-checks it (`tools/check_swift_smoke.sh typecheck`).

Against that, the specs already make a claim this change has to make true:
`evaluation-backends` calls Metal "the iPad app's production path", and
`build-packaging` says GPU backend availability "SHALL never change results —
only speed", enforced as a release gate. Speed is exactly what is never
measured there.

Two facts from prior measurement on this hardware also constrain the design: a
cold Metal shader cache cost ~48 s on first run (53 s vs 5.3 s warm), and
occasional samples land on an efficiency core, which is why medians rather than
means are the reported statistic.

## Goals / Non-Goals

**Goals:**

- Make the Metal backend buildable, registrable and *shipped* for iOS device
  and simulator targets.
- Measure per-stamp interactive latency on a real iPad, at p50/p95, across a
  document-growth axis, on both `cpu` and `metal` so the device's own CPU path
  is the control.
- Verify every brush and sculpt verb on device for **correctness** as well as
  timing, with a coverage table that turns an untested verb into an error.
- Gate it at release time against committed baselines and declared budgets.

**Non-Goals:**

- **Anything above the library.** Gestures, draw calls, frame rate, battery and
  thermals of a host app are not this library's to own — the roadmap says so
  explicitly in the armature scoping note, and that boundary does not move
  because we are now measuring on device. We measure the cost of a claycore
  call, not of a frame.
- **PR CI gating.** Chosen deliberately: shared runners have no iPad and no
  timing stability.
- **iPhone, Simulator, or Mac as the gated device.** They may be run for
  convenience; only an attached iPad produces a gating number.
- **Rewriting the host benchmarks.** `benchmarks/bench_main.cpp` and
  `tools/check_bench.py` stay as the order-of-magnitude throughput gate they
  already are. This change adds a second, different gate rather than
  reinterpreting the first.
- **Optimising anything.** This change measures. Acting on what it measures is
  the next change, and keeping them separate is what makes the first baseline
  trustworthy.

## Decisions

### The harness is an XCTest bundle driving the C ABI

**Why the C ABI and not internal C++:** it is the surface the app consumes, and
one harness source stays valid across two branches being A/B compared — the
internal C++ may change shape between them. `bindings/c/clay_c.cpp` is compiled
*into* `libclaycore.a`, so linking costs nothing extra.

**Why XCTest and not a custom app:** `xcodebuild test -destination
'platform=iOS,id=<udid>'` is the one supported way to run code on a provisioned
device from a script, and it handles install, launch, log capture and teardown.
Alternatives rejected: a hand-rolled app driven by `devicectl` (re-implements
all of that), and `swift test` (cannot target a device).

**Why not XCTest's own `measure(metrics:)`:** it reports average and standard
deviation and manages its own baselines in the pbxproj. We need p50/p95 and a
baseline format that lives in the repo and is diffable. So XCTest hosts and
launches; the timing loop, the percentile computation and the JSON are ours.

**Getting results off the device:** results are written as an `XCTAttachment`
and extracted from the `.xcresult` with `xcrun xcresulttool`. Parsing the
human-readable test log was rejected as brittle.

### Budgets come in three classes, not one number

A single "8.3 ms" budget would be wrong for most of the surface, because most
of the surface is not per-stamp work. Cases declare a class:

| Class | What it covers | Budget shape |
|---|---|---|
| `interactive` | one brush stamp on a stroke | a fraction of a 120 Hz frame, because the host needs the rest of it to draw |
| `gesture` | one drag resolved as a unit (a Move drag, a cut preview) | perceptible-but-fluid, order 100 ms |
| `operation` | an explicit user action (consolidate, mask extrude, mesh export) | order 1 s, gated for regression rather than for feel |

Rationale: `Layer.consolidate` is not on the interactive path and never was —
the roadmap describes it as an advisory-triggered bake the artist chooses. Held
to a stamp budget it would fail forever and the gate would be ignored. Held to
its own class, a 3x regression in it still fails.

The exact numbers per class are an open question below; the *structure* is the
decision.

### The document-growth axis is the point, not a refinement

`c-abi/spec.md:569` and the roadmap's "Finding 2" both record that the tape
recompiles per edit, so per-stamp cost is a function of accumulated document
size. A single-size measurement would pin the least interesting point on that
curve. Three sizes spanning two orders of magnitude, with a super-linear growth
check, is what actually guards the interactive path, and it is why the gate
checks a ratio and not only a level.

### Metal SDK selection is derived, and a mismatch is a build failure

The metallib rules gain the target SDK, derived from `CMAKE_SYSTEM_NAME` and
`CMAKE_OSX_SYSROOT` (`macosx` / `iphoneos` / `iphonesimulator`) rather than
hardcoded. The alternative — a user-supplied cache variable — was rejected
because getting it wrong produces a binary that builds, links, ships, and fails
to register its backend at run time on a customer's device. Deriving it means
the wrong combination cannot be expressed; failing the build when no mapping
exists means an unrecognised target is loud rather than silent.

### Cold-cache and scheduling noise are handled in the harness, not the gate

A warm-up pass runs before timing (the ~48 s cold shader compile would
otherwise land entirely in the first case's p95), percentiles rather than means
absorb efficiency-core outliers, and `ProcessInfo.thermalState` is sampled: a
run that starts or ends anywhere but `.nominal` is reported as invalid rather
than compared. Loosening the tolerance to absorb thermal noise was rejected —
it would hide the regressions the gate exists to catch.

### Baselines are hardware-stamped and refuse cross-device comparison

Every baseline records device model, OS version, xcframework version and
claycore commit. A comparison against a run from different hardware fails as
invalid rather than producing a number. Without this the first mismatched
device silently reports every case as a large regression or a large win, and
the gate stops being believed.

## Risks / Trade-offs

- **Thermal throttling makes a long run drift.** → Sample `thermalState` at
  start and end, invalidate on anything but nominal; keep the total run short
  enough to stay there, and split the suite if it does not.
- ~~**A hostless iOS XCTest bundle may not install on device.**~~ **Settled
  2026-08-10: it cannot.** The SwiftPM route was tried first and `xcodebuild`
  refused outright — "Tool-hosted testing is unavailable on device
  destinations. Select a host application for the test target, or use a
  simulator destination instead." SwiftPM cannot declare a test host, so a
  package alone reaches the simulator and never the iPad. The harness
  therefore needs an Xcode project with a trivial host app, which is what
  `tests/device/project.yml` builds.
- ~~**A checked-in `.xcodeproj` is hostile to review.**~~ **Resolved by
  generating it.** The original plan rejected `xcodegen` as a new dependency;
  once the host app made the project bigger than one target, an unreviewable
  pbxproj in the diff was the worse trade. `tests/device/project.yml` is the
  source of truth, the `.xcodeproj` is gitignored, and `run_device_bench.sh`
  regenerates it every run so the two cannot drift. The cost is a developer
  tool (`brew install xcodegen`) the script checks for by name.
- **The host app is a second signed bundle.** It links no claycore — the test
  bundle links the xcframework itself, so there is one copy of the static
  library in the graph and no duplicate-symbol question.
- **Device provisioning is now a release prerequisite.** → Documented in
  `docs/RELEASE.md`, and the release fails loudly on a missing device rather
  than skipping. The cost is real: a release cannot be cut from a machine with
  no iPad attached. That is the intended trade — a skipped hardware gate and a
  passing one look identical in a log.
- **Enabling Metal in the shipped iOS slices changes what consumers get.** →
  Results are unchanged by construction (parity is what this same harness
  gates); what changes is that a host that did not wire Metal now gets it. The
  slice also grows by the embedded metallib.
- **The first baseline could enshrine something already too slow.** → The
  baseline records the measurement; the budgets record the requirement. A case
  can be committed as passing-vs-baseline while visibly over budget, and that
  is reported rather than hidden. Fixing it is the follow-up change this one
  deliberately does not attempt.

## Migration Plan

1. Land the SDK fix and the Metal-enabled iOS slices first; they are
   independently verifiable (`metal` registers on device) and useful without
   the harness.
2. Land the harness and coverage table with **no** gate wired, run it, and
   inspect the numbers.
3. Commit the baseline and the budgets as a separate, reviewable step — that
   commit is where the performance requirements actually get decided.
4. Wire the release gate last.

Rollback is per-step: the gate is one workflow step, the baseline is one file,
and the iOS Metal slices revert to the current CPU-only build by dropping one
CMake argument.

## Open Questions

- **Which iPad is the reference device?** Two are attached — `iPad (52)`, an
  iPad Air 13-inch (M3), and `iPad (8)`, an iPad Pro 12.9-inch (5th gen, M1).
  The first device run used the M3. The baseline is only comparable against the
  one that produced it, so this is a decision the first baseline commits us to.
  Leaning toward the M1 as the reference: a budget met on weaker hardware is
  met everywhere above it, and the M1 iPad Pro is a likelier floor for the
  app's install base than the current-generation Air.
- **Signing is on a shared team certificate.** The wildcard profile that covers
  these devices belongs to team `2C69VJZSNR` and its certificate expires
  **2026-09-02**, about three weeks out. A release gate that cannot sign cannot
  run, so the renewal is now on the critical path for releases and should be
  named in `docs/RELEASE.md`.
- **The budget numbers per class.** Structure is decided above; the values want
  one measurement pass before they are fixed, which is why step 3 of the
  migration plan is its own commit.
- **Whether memory footprint joins latency in the same gate.** A sculpting host
  on iPad is memory-constrained and the brick cache has a budget already
  (`brick-cache`), so there is a natural case for it — but it is a different
  measurement with different noise, and folding it in now would delay the
  latency gate.
- **Whether the simulator gets a non-gating run** for developer feedback
  between device runs. Cheap to add, and the numbers are not comparable to
  device numbers; the risk is that someone reads them as if they were.

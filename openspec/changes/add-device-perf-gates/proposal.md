# Proposal: prove the brushes on the iPad, for correctness and for latency

## Why

`evaluation-backends/spec.md` calls Metal "the iPad app's production path". No
iPad has ever run it. The claim is unverified in two independent senses, and
both are structural rather than an oversight in one test:

**The shipping artifact cannot select Metal.** `tools/build_xcframework.sh`
builds all three slices without `CLAY_BACKEND_METAL`, so the xcframework a host
resolves is CPU-only everywhere — stated as a deliberate choice ("wired per-app
during Xcode integration"). And it could not do otherwise today:
`CMakeLists.txt:110` and `:118` compile the kernels with `xcrun -sdk macosx
metal` / `-sdk macosx metallib`, and `metal_backend.cpp:161` loads that blob
from the binary it was embedded in. An iOS device slice built with the backend
enabled would embed **macOS AIR**, which will not load on an iPad. So the
production path is not merely untested on device; it is unbuildable for device.

**Nothing anywhere measures latency.** The four benchmarks in
`benchmarks/bench_main.cpp` are pinned to `Registry::find("cpu")` (`:72`, `:88`),
run one static 12-primitive scene, and are gated by `tools/check_bench.py:14-19`
at 500k points/sec and **20-second** ceilings — self-described as generous
order-of-magnitude floors. CI runs them on `ubuntu-latest` with the `cpu-only`
preset (`.github/workflows/ci.yml:213-225`). Throughput on batched points says
nothing about whether one Pencil event resolves inside a frame, which is the
only number a sculpting host lives on. `c-abi/spec.md:569` and the roadmap's
"Finding 2" both record that the tape recompiles per edit, so interactive cost
grows with the document — and no test pins that curve, which means an O(n²)
creep that only appears on stroke 800 passes CI green today.

The functionality half is closer but still short: `test_parity.cpp:312` does
check every registered backend against the scalar reference, and `:298` guards
that the corpus covers every **primitive type** — but there is no equivalent
guard for the ops, deformers or blend profiles, the corpus is hand-written
scenes rather than brush output, and on GitHub's macOS runners the Metal job
runs parity only "when the runner exposes a Metal device".

## What Changes

- **A Metal-capable iOS device slice.** The metallib compile learns the target
  SDK instead of hardcoding `macosx`, so `iphoneos` and `iphonesimulator`
  slices embed AIR their own device can load. `tools/build_xcframework.sh`
  builds the iOS and iOS-simulator slices with `CLAY_BACKEND_METAL=ON`.
  **BREAKING for consumers only in the good direction**: an app that wired
  Metal itself keeps working, and one that did not now gets the backend
  registered rather than absent.
- **An XCTest harness driven through the C ABI**, run on a provisioned iPad via
  `xcodebuild test -destination 'platform=iOS,id=<udid>'`. The repo has no
  Xcode project or XCTest bundle today, and `clay_bench` links google-benchmark
  as a plain executable that cannot run as an iOS app bundle. Driving the C ABI
  rather than internal C++ measures the surface the app actually uses.
- **Latency as the metric, not throughput.** Per-stamp cost reported at p50 and
  p95 against an explicit frame budget, on both `cpu` and `metal`, so the
  device's own CPU path is the control for its GPU path.
- **A document-growth axis.** Every latency case runs at several stamp counts,
  so the shape of the growth curve is pinned and not just its first point.
- **On-device functionality, not only timing.** The parity corpus runs on the
  iPad against the scalar reference, and the corpus gains scenes built *by the
  brushes* rather than by hand.
- **A coverage guard**, in the spirit of `CAPABILITY_EXAMPLES` in
  `examples/run_all.py`: a named table where every brush and sculpt verb has a
  device case, an uncovered one is an error, and an exemption is a decision on
  the record. This extends the primitive-only guard at `test_parity.cpp:298` to
  ops, deformers and blend profiles.
- **Committed baselines and a release-time gate.** Baseline JSON lives in the
  repo; `tools/check_device_bench.py` compares a run against it and is wired
  into the release workflow. PR CI is unchanged — shared runners have no iPad
  and no trustworthy timing, so a blocking gate there would be noise.

## Capabilities

### New Capabilities

- `device-harness`: running claycore on real iPad hardware — how the harness is
  built and invoked, what it measures (per-stamp p50/p95 latency across a
  document-growth axis), the coverage table that makes an untested brush an
  error, the baseline format, and the release-time regression gate.

### Modified Capabilities

- `evaluation-backends`: the Metal backend SHALL be buildable and registrable on
  iOS device and simulator targets, which requires the kernel compile to follow
  the target SDK; and its parity SHALL be gated on real device hardware rather
  than only where a CI runner happens to expose a Metal device.
- `build-packaging`: the xcframework's iOS slices SHALL carry the Metal backend,
  and the release checklist SHALL include the device gate alongside the existing
  parity gate.

## Impact

- **Build**: `CMakeLists.txt` (metallib SDK selection),
  `tools/build_xcframework.sh` (Metal on the iOS slices).
- **New**: an Xcode test project or SwiftPM test target plus an XCTest bundle,
  `tools/check_device_bench.py`, `tools/run_device_bench.sh`, and a committed
  baseline JSON.
- **CI/release**: `.github/workflows/release.yml` gains the gate;
  `.github/workflows/ci.yml` is untouched.
- **Tests**: the parity corpus gains brush-authored scenes and a coverage guard
  over ops, deformers and blend profiles.
- **Hardware**: a provisioned iPad must be attached for the gate to run, which
  makes this a local and release-time check by construction. Signing identity
  and device provisioning become a documented prerequisite in `docs/RELEASE.md`.
- **No engine behaviour changes.** Nothing here alters what a document
  evaluates to; backends change speed, not values, and this change is about
  measuring that claim rather than modifying it.

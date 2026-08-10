## MODIFIED Requirements

### Requirement: Metal backend (tier 1)
The Metal backend SHALL use `metal-cpp` (pure C++, no Objective-C in the core), compile the kernel headers as MSL, pass tapes via argument buffers, and implement the full backend interface including `eval_bricks` and on-device meshing. It is the iPad app's production path.

Because it is that path, the backend SHALL be buildable and registrable on iOS
device and iOS simulator targets, not only on macOS. The kernel compile SHALL
follow the **target** SDK rather than assuming the host's: a build for
`iphoneos` SHALL compile and archive its metallib with the `iphoneos` SDK, and
likewise for `iphonesimulator`. Embedding a metallib built for another SDK
produces a library that loads on no device at all, which is indistinguishable
at the ABI from a backend that was never enabled.

A build configured with the Metal backend enabled SHALL fail at configure or
build time if it cannot produce a metallib for its target, rather than
producing a binary whose backend fails to register at run time.

#### Scenario: Metal brick fill
- **WHEN** a dirty brick set is submitted to the Metal backend
- **THEN** narrow-band fp16 brick data is returned matching CPU reference within parity tolerance

#### Scenario: The backend registers on an iPad
- **WHEN** an app links an iOS device slice built with the Metal backend enabled and enumerates backends through the C ABI
- **THEN** `metal` is among the registered backends and serves evaluation calls that name it

#### Scenario: A metallib built for the wrong SDK
- **WHEN** a build targets `iphoneos` and the kernel compile is configured against the `macosx` SDK
- **THEN** the build fails naming the SDK mismatch, rather than emitting a binary whose Metal backend silently fails to register

### Requirement: GPU parity tolerance
Every registered GPU backend SHALL match the CPU scalar reference within documented tolerances — default 1e-4 relative on distances, specified per-kernel where tighter or looser — on every kernel and on composed golden scenes. The parity suite SHALL run on every registered backend and SHALL be a CI gate on platforms where that backend exists.

For the Metal backend the platform where it exists includes real iPad
hardware, and CI runners are not a substitute: a runner that exposes no Metal
device skips the gate silently. Metal parity SHALL therefore also be gated on
an attached device by the `device-harness`, at release time, so the production
path is verified on the hardware it ships to rather than only where a runner
happened to have a GPU.

#### Scenario: Per-kernel parity gate
- **WHEN** the parity suite runs a backend against CPU scalar for each kernel over the standard sample corpus
- **THEN** any kernel exceeding its documented tolerance fails CI with the kernel name and worst-case error reported

#### Scenario: A runner without a Metal device
- **WHEN** the macOS CI job runs on a runner that exposes no Metal device
- **THEN** the skipped Metal parity is reported as skipped rather than passed, and the release-time device gate is what establishes the backend's parity

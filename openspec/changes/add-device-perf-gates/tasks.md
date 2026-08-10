## 1. Metal on iOS — make the production path buildable

- [x] 1.1 Derive the metallib SDK from the target in `CMakeLists.txt:100-125`: map `CMAKE_SYSTEM_NAME`/`CMAKE_OSX_SYSROOT` to `macosx` / `iphoneos` / `iphonesimulator`, replacing both hardcoded `-sdk macosx` invocations
- [x] 1.2 Fail the configure with a named diagnostic when `CLAY_BACKEND_METAL` is on and the target maps to no known SDK, rather than emitting a binary whose backend cannot register
- [x] 1.3 Pass the deployment target to the metal compile so the AIR matches the slice's minimum OS
- [x] 1.4 Build the `iphoneos` slice with `CLAY_BACKEND_METAL=ON` and confirm the embedded metallib is iOS AIR (`xcrun metal-readobj` or equivalent), not macOS
- [x] 1.5 Enable `CLAY_BACKEND_METAL` for the macos, ios and ios-sim slices in `tools/build_xcframework.sh`, linking Metal and Foundation per slice
- [x] 1.6 Verify the built xcframework still passes the existing kernel-header and module-map checks (`build-packaging`: each slice's `Headers/clay/kernel/`, module map declares only `clay.h`)

## 2. The device harness — get code running on the iPad

- [x] 2.1 Add a minimal Xcode project under `tests/device/` with a single XCTest target linking `dist/claycore.xcframework`, no app logic
- [x] 2.2 Resolve the hostless-vs-host-app question from the design's risk list: attempt a hostless bundle first, add a trivial host app target only if the device install requires one
- [x] 2.3 Write `tools/run_device_bench.sh`: resolve the target iPad's udid, run `xcodebuild test -destination 'platform=iOS,id=<udid>'`, exit non-zero with a named diagnostic when no provisioned device is attached
- [x] 2.4 Extract results from the `.xcresult` via `xcrun xcresulttool` and write them to a JSON path given on the command line
- [x] 2.5 Smoke the whole path end to end: one trivial case that calls `clay_list_backends` through the ABI and reports which backends registered on the device
- [x] 2.6 Confirm `metal` appears in that list on the iPad — this is the first direct evidence the production path runs on device at all

## 3. Measurement mechanics

- [x] 3.1 Implement the timing core: a warm-up pass excluded from the samples, then N timed repetitions collecting per-sample durations
- [x] 3.2 Compute and report p50 and p95; assert no mean is reported as the gated statistic
- [x] 3.3 Record per case: backend name, device model, OS version, sample count, and the backend that actually served the call
- [x] 3.4 Fail a case that requested `metal` but was served by another backend, rather than recording the substitute's number
- [x] 3.5 Sample `ProcessInfo.thermalState` at start and end; mark the run invalid when either is not `.nominal`
- [x] 3.6 Add the document-growth axis: run every case at three stamp counts spanning two orders of magnitude, recording each

## 4. Coverage — every brush and verb has a device case

- [x] 4.1 Write the coverage table mapping each brush and sculpt verb to its device case, with an explicit exemption entry format carrying a reason
- [x] 4.2 Fail the harness when a verb known to the engine has no table entry
- [x] 4.3 Add latency cases for the voxel sculpt verbs
- [x] 4.4 Add latency cases for the SDF verbs: relax, flatten, move, snakehook, mask paint, mask extrude
- [x] 4.5 Add latency cases for the brush stroke engine and the cut tool
- [x] 4.6 Classify every case as `interactive`, `gesture` or `operation` per the design's budget classes

## 5. Correctness on device, not only timing

- [x] 5.1 Run the existing parity corpus on the iPad against the scalar reference, using the tolerances `evaluation-backends` already documents
- [x] 5.2 Extend the parity corpus with scenes authored *by the brushes* — the tape a stroke, a cut and a mask extrude actually emit
- [x] 5.3 Extend the coverage guard at `tests/unit/test_parity.cpp:298` beyond primitive types to the combine ops, the deformers and the blend profiles
- [x] 5.4 Fix or record whatever 5.3 uncovers — an op no scene reaches today is a real gap, not a test bug
- [x] 5.5 Report a skipped Metal parity run as skipped rather than passed in the existing macOS CI job

## 6. Baselines and the gate

- [x] 6.1 Define and document the baseline JSON schema: per case, the class, the declared budget, p50/p95 per document size, plus device model, OS, xcframework version and claycore commit
- [x] 6.2 Write `tools/check_device_bench.py`: fail on regression beyond tolerance, on a case over its declared budget (checked against p95), and on any case with no declared budget
- [x] 6.3 Implement the super-linear growth check across the document-size axis, failing with the sizes and the measured ratio
- [x] 6.4 Refuse a comparison whose baseline device model or OS differs from the run's, as invalid rather than as a result
- [x] 6.5 Run the full harness on the reference iPad and inspect the numbers before any gate is wired
- [x] 6.6 Commit the baseline and the per-class budget values as their own reviewable commit — this is where the performance requirements get decided
- [x] 6.7 Wire the gate into `.github/workflows/release.yml`, failing the release when no device is attached rather than skipping; leave `ci.yml` untouched

## 7. Documentation and closing the loop

- [x] 7.1 Document the device prerequisite in `docs/RELEASE.md`: provisioning, signing identity, which iPad is the reference device
- [x] 7.2 Document how to run the harness locally and how to read a result, including that simulator and Mac numbers are not comparable to device numbers
- [x] 7.3 Update `docs/05-claycore-library.md` where it describes the latency-critical path, so the measured numbers are discoverable from the library overview
- [x] 7.4 Add a row to `openspec/ROADMAP.md` — the roadmap has no device performance entry today, which is why this gap survived to v0.25.0
- [x] 7.5 Record in the roadmap what the first measurement pass found, in the style of the existing "what actually bit" entries
- [x] 7.6 Run `openspec validate --strict` and the four presets plus `release_check`

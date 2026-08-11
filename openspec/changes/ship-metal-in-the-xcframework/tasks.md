# Tasks: ship-metal-in-the-xcframework

- [x] 1.1 Compile the metallib for the slice's SDK rather than hardcoding `macosx`, derived from `CMAKE_SYSTEM_NAME` / `CMAKE_OSX_SYSROOT`. This is the reason a one-line flag flip would not have worked: a macOS metallib in an iOS slice links cleanly and fails to register at runtime
- [x] 1.2 `build_xcframework.sh` passes `-DCLAY_BACKEND_METAL=ON` and every other backend option explicitly, so a cached value cannot decide what ships
- [x] 1.3 Fail the build if a slice produced no metallib, and again if the merged archive does not carry it — the second check is the device-independent one
- [x] 1.4 `Package.swift` declares `Metal` and `Foundation` on the target that links the binaryTarget
- [x] 1.5 `check_swift_smoke.sh` links `-framework Metal` for both the macOS and simulator harnesses
- [x] 1.6 The Swift smoke test reports the registered backends and asserts Metal is among them
- [x] 1.7 Correct the script comment that told consumers to enable the backend during Xcode integration, which a prebuilt static library makes impossible
- [x] 1.8 Run `tools/build_xcframework.sh` on an Apple machine and confirm all three slices carry a metallib for their own SDK — **done on an Apple machine by `add-device-perf-gates`.** All three build and each is verified by platform, not merely by presence: `metal-readobj --file-headers` reports `METALLIB_PLATFORM_MACOS` / `_IOS` / `_IOS_SIMULATOR` respectively, and that check is now part of the script so the wrong AIR cannot ship silently
- [x] 1.9 Confirm on a real iPad that `clay_list_backends` reports `metal`, and re-measure — **done on iPad15,5 (iPad Air 13-inch M3), iOS 26.5.2:** `backends = cpu, metal`. Re-measured far past the bake numbers: `add-device-perf-gates` carries a per-verb latency suite on that device, which also found that Metal is SLOWER than the CPU below roughly a thousand stamps (1.85 ms vs 0.21 ms p95 at ten), so shipping Metal is a win at scale and a loss early
- [ ] 1.10 Reproduce the reported Simulator parity deviation (baked field 0.166 vs 0.033 on CPU) under the parity suite rather than through an app fixture, now that a Metal-enabled framework makes it reachable — **still open, and narrowed.** `add-device-perf-gates` added a parity corpus that runs ON the iPad through the C ABI: every primitive, every combine op, every deformer, every blend profile, and scenes the brushes authored, all at 0.0 worst-case relative error against the CPU. That does NOT close this: no scene bakes through `clay_item_volume_from_document` and compares backends, which is what the report describes. The gap is a scene, not a mechanism
- [x] 1.11 Decide whether the Metal compile needs an explicit deployment-target flag — **decided yes, and implemented.** `-mios-version-min` / `-mios-simulator-version-min` / `-mmacosx-version-min` are passed from `CMAKE_OSX_DEPLOYMENT_TARGET`, and the metallib records it: `metal-readobj` reports `PlatformMajor: 16` for the iOS slices. Without it the AIR takes the SDK's own version, which is exactly the silent non-registration this change is about

## Not done

- **No CPU-only variant.** The issue offers it as an option if a CPU artifact
  is still wanted. Nothing in this repository needs one, and two artifacts that
  differ only in a way invisible at the API is a way to deploy the wrong one.
  Trivial to add if a consumer asks.
- **Nothing verified on Apple hardware.** Every change here is build-time and
  none of it could be executed on the machine it was written on. The scripts
  parse, CMake configures, and the gates are written to fail loudly — but the
  first real evidence will come from the release workflow and from a device.

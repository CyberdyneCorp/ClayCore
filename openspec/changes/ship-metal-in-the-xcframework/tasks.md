# Tasks: ship-metal-in-the-xcframework

- [x] 1.1 Compile the metallib for the slice's SDK rather than hardcoding `macosx`, derived from `CMAKE_SYSTEM_NAME` / `CMAKE_OSX_SYSROOT`. This is the reason a one-line flag flip would not have worked: a macOS metallib in an iOS slice links cleanly and fails to register at runtime
- [x] 1.2 `build_xcframework.sh` passes `-DCLAY_BACKEND_METAL=ON` and every other backend option explicitly, so a cached value cannot decide what ships
- [x] 1.3 Fail the build if a slice produced no metallib, and again if the merged archive does not carry it — the second check is the device-independent one
- [x] 1.4 `Package.swift` declares `Metal` and `Foundation` on the target that links the binaryTarget
- [x] 1.5 `check_swift_smoke.sh` links `-framework Metal` for both the macOS and simulator harnesses
- [x] 1.6 The Swift smoke test reports the registered backends and asserts Metal is among them
- [x] 1.7 Correct the script comment that told consumers to enable the backend during Xcode integration, which a prebuilt static library makes impossible
- [ ] 1.8 Run `tools/build_xcframework.sh` on an Apple machine and confirm all three slices carry a metallib for their own SDK — **not done here: no Apple toolchain on the machine this was written on. The release workflow builds the xcframework on macOS and now fails rather than shipping a CPU-only slice**
- [ ] 1.9 Confirm on a real iPad that `clay_list_backends` reports `metal`, and re-measure the bake numbers from the issue. The simulator is not a device, and the whole bug was a difference nobody measured
- [ ] 1.10 Reproduce the reported Simulator parity deviation (baked field 0.166 vs 0.033 on CPU) under the parity suite rather than through an app fixture, now that a Metal-enabled framework makes it reachable
- [ ] 1.11 Decide whether the Metal compile needs an explicit deployment-target flag to match `CMAKE_OSX_DEPLOYMENT_TARGET`. An AIR version a 16.0 device refuses would present as the same silent non-registration this change is about

## Not done

- **No CPU-only variant.** The issue offers it as an option if a CPU artifact
  is still wanted. Nothing in this repository needs one, and two artifacts that
  differ only in a way invisible at the API is a way to deploy the wrong one.
  Trivial to add if a consumer asks.
- **Nothing verified on Apple hardware.** Every change here is build-time and
  none of it could be executed on the machine it was written on. The scripts
  parse, CMake configures, and the gates are written to fail loudly — but the
  first real evidence will come from the release workflow and from a device.

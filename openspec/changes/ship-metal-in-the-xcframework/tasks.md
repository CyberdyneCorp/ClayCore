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
- [x] 1.10 Reproduce the reported Simulator parity deviation (baked field 0.166
      vs 0.033 on CPU) under the parity suite rather than through an app
      fixture — **the scene exists in both corpora now.**
      `tests/device/Tests/DeviceParityTests.swift` has `authored_baked_volume`,
      a document collapsed by `clay_item_volume_from_document` then placed and
      evaluated, added for this task and named in its comment; the text above
      this line said no such scene existed and had gone stale.
      `tests/unit/test_parity.cpp` now carries the same shape as
      `document_baked_volume`, which puts it in the corpus every registered
      backend is compared over on EVERY pull request rather than only in the
      device gate's release-time run. Measured on macOS + Metal: worst relative
      error **0**, against a fixture required to hold >40 bricks and to put
      >100 of its 4,096 probes inside the surface and >100 in the band, so a
      volume that redistanced to nothing cannot pass it.

      WHAT IS STILL UNVERIFIED is the SIMULATOR itself, which is where the
      report came from. Native macOS Metal and the iPad both agree; the
      Simulator's Metal is a different device and no suite has been run on it.
      That is a run, not a scene, and it belongs to whoever next has a
      Simulator in front of them.

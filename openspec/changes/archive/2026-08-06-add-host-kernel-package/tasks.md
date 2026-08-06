# Tasks: add-host-kernel-package

- [x] 1.1 `shim.h` selects its backend from the compiler's own macros; an explicit `CLAY_KERNEL_*` still wins
- [x] 1.2 `clay/kernel/kernels.h` umbrella, backend-aware about what each dialect can compile
- [x] 1.3 Metal profiles in the kernel-dialect gate: real `xcrun metal` on Apple, stubbed `metal_stdlib` elsewhere
- [x] 1.4 Layering check: kernel headers may not include another module
- [x] 1.5 Parity fixture — cases, JSON export, `clay parity-fixture`
- [x] 1.6 `tools/package_kernels.py` and kernel headers in the xcframework
- [x] 1.7 Tests: expectations match the tape and every registered backend, export is deterministic, a support-`k` smin fails the fixture
- [x] 1.8 Docs (`docs/06-host-gpu-previews.md`, README, roadmap), CI steps, full verification

# Tasks: add-device-perf-budgets

- [ ] 1.1 DECIDE and record in `design.md`: which iPad is the reference device. The oldest supported one is the honest choice and the least convenient; name it either way, because a budget without a device is not a requirement
- [ ] 1.2 DECIDE and record: which document sizes are measured. 100 / 2 400 / 10 000 items makes the numbers comparable with what is already recorded; a real `.clayspace` is more honest and less reproducible
- [ ] 1.3 DECIDE and record: how the harness reaches the device — XCTest performance target, a CLI binary through `devicectl`, or the existing Swift smoke program extended. The constraint is one person, one command
- [ ] 1.4 End-to-end dab measurement: mark dirty → take dirty → eval requests → submit, timed as a whole, at each document size
- [ ] 1.5 End-to-end preview-frame measurement: whichever of raycast or mesh the app actually draws with, timed as a whole
- [ ] 1.6 Sustained run reporting first-dab and steady-state separately, with the interval over which they diverged
- [ ] 1.7 A results format committed in the repository: device, OS, build configuration, commit, and every figure. A number without its machine is not a result
- [ ] 1.8 A verdict against the budget — pass or fail at each size against 4–8 ms and 16.7 ms — rather than a throughput figure the reader has to interpret
- [ ] 1.9 Take the first run on the reference device and commit it. If it fails at some size, record the size it passes at rather than weakening the requirement
- [ ] 1.10 Re-measure the CPU/Metal crossover on the device, and update `docs/RELEASE.md`'s "keep brick fills on cpu" advice against that number — the current one is an M2 Max result standing in for a tablet
- [ ] 1.11 Add the run to `docs/RELEASE.md`'s list of manual, hardware-dependent release checks, alongside the CUDA and OpenCL device checks, and to `tools/release_check.py` where it can be driven
- [ ] 1.12 Keep the microbenchmarks and state their role: attribution once the total moves, not a stand-in for the total
- [ ] 1.13 Re-run against the other interactive-path changes as they land — this measurement is how any of them are said to have worked

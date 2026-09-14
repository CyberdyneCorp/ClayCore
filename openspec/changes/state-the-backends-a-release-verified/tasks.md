## 1. The defect

- [x] 1.1 `parity` asserts "every backend REGISTERED IN THIS BUILD"; the
      v0.113.0 release build registered `cpu` and `metal`, so CUDA and OpenCL
      passed vacuously
- [x] 1.2 The row's detail was a test-case count — 46 with two GPUs attached and
      46 with none, so nothing in the table distinguished the two
- [x] 1.3 The four manual hardware gates lived in `docs/RELEASE.md` prose, with
      nowhere to write down whether they had been run

## 2. What the checker now says

- [x] 2.1 `parity_row` reads `PARITY_BACKENDS_CHECKED:`, names the backends
      compared, and names the hardware ones absent from the build
- [x] 2.2 A run that prints no such line FAILS the row rather than passing on a
      case count
- [x] 2.3 `hardware/<gate>` rows, one per manual gate, answered from
      `tests/hardware/manual-gates.json`
- [x] 2.4 A gate is `run` with `evidence` or `waived` with a `reason`, and
      always against a `commit`; anything else fails
- [x] 2.5 Staleness is per gate: `KERNEL_SOURCES` expires all four,
      `PARITY_CORPUS` expires only the three parity gates — nvcc either compiles
      the backend and picks an architecture or it does not, and a test file
      cannot change that

## 3. Measured

- [x] 3.1 On this build, `main` prints `[PASS] parity: test cases: 46 | 46
      passed`; the branch prints `[PASS] parity: compared cpu | 46 cases |
      1411932 assertions | NOT BUILT, so this row says nothing about them: cuda,
      opencl, vulkan`
- [x] 3.2 RTX 5060 at `e8ca6d59`, recorded in the gate file: 543 assertions
      cpu-only, 1082 with CUDA, 1621 with CUDA and OpenCL — 539 each, not 543
- [x] 3.3 31 regression tests: 31 failed against `main`'s `release_check.py`,
      31 passed against the branch's

## 4. Docs

- [x] 4.1 `docs/RELEASE.md` "Before tagging" gains step 3, the four gates and
      the record file; the old step 3 (the ABI/history entry) becomes step 4 and
      the release skill's reference to it moves with it
- [x] 4.2 `.claude/skills/claycore-release/SKILL.md` §2 carries the four gates as
      step 4 of the order of operations, beside the device gate, with the
      assertion figures and the `OCL_ICD_VENDORS` control
- [x] 4.3 The "CI no longer builds CUDA or OpenCL at all" entry points at the
      record file instead of leaving the four in prose

## 5. Still open

- [ ] 5.1 THREE ROWS ARE RED ON LANDING. The recorded RTX 5060 run predates
      #582, which put the circ easings into the list the parity case reads —
      exactly the family #578 was about. The next release re-runs them on
      hardware or records a waiver
- [ ] 5.2 Vulkan on real silicon has never run: waived, because no host in the
      fleet has it. lavapipe gates plumbing, not arithmetic
- [ ] 5.3 The cubin half of the CUDA architecture auto-detection stays
      unexercised — the recorded run reached Blackwell through PTX JIT, and a
      cubin for `sm_120` needs CUDA >= 12.8

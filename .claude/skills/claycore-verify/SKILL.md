---
name: claycore-verify
description: Build claycore and run every gate CI will run, locally, before pushing — presets, the sharded unit suite, the check_*.py tools, and the gates that pass for the wrong reason. Use before opening or updating a PR, or when a CI job fails and you need to reproduce it.
---

# Verifying a claycore change locally

## Build

```sh
cmake --preset cpu-only -DCLAY_BUILD_TESTS=ON
cmake --build --preset cpu-only -j8
ctest --preset cpu-only --output-on-failure
```

Presets: `cpu-only`, `metal`, `cuda`, `opencl`, `vulkan`, `asan-ubsan`, `tsan`.

**`cmake --preset` does not reset a cached option.** A tree configured earlier
with `CLAY_BUILD_TESTS=OFF` keeps it, and `ctest` then reports "No tests were
found!!!" — pass the `-D` explicitly or delete the build dir.

**`--parallel` with an explicit count, never a bare `-j`.** The default
generator is Unix Makefiles and a bare `-j` means *unlimited*: a 40-source
target spawns 47 concurrent compiles, and a runner SIGTERMs the tree with exit
143, no compiler error and no failing gate. That killed one release workflow
twice.

## The unit suite is sharded by source file

Four ctest entries: `clay_unit_tests_prefix`, `_cull`, `_heavy` and `_rest`.
The first three name their files explicitly in `tests/CMakeLists.txt`; `_rest`
is defined by **exclusion** (`-sfe=`), so a new test file joins it automatically
and needs no wiring. `clay_unit_shards_partition` asserts the four cover every
case exactly once.

Run one file while iterating:

```sh
./build/cpu-only/tests/clay_unit_tests '-sf=*test_layer_extent_memo.cpp'
```

Quote the pattern — zsh expands a bare `-sf=*foo.cpp` and the command fails with
a glob error, not a doctest error.

## The gates CI runs, in the order they are cheap

```sh
python3 tools/check_layering.py           # module dependency rule
python3 tools/check_kernel_dialect.py     # kernel headers under every backend profile
python3 tools/check_licenses.py           # dependency manifest
python3 tools/check_c_abi.py              # header hygiene + ctypes FFI
python3 tools/check_test_shards.py        # the four shards partition the suite
python3 tools/check_gallery.py            # the nine gallery documents
python3 tools/check_doc_latency.py
npx -y @fission-ai/openspec@1.8.0 validate --all --strict
```

Then the two slow ones: `tools/check_bench.py` (see the `claycore-bench` skill)
and the device gate (see `claycore-device-gate`).

`python3 tools/release_check.py --skip-slow` runs the whole set in one pass with
a pass/fail table, and is the fastest way to reproduce what the release workflow
does.

## Gates that pass for the wrong reason

- **Binding parity needs a BUILT pyclay.** `check_binding_parity.py` falls back
  to comparing the parsed `pyclay_module.cpp` against **itself** when no module
  can be imported, and that comparison cannot fail. `release_check.py` now
  passes `--pyclay <build>/bindings/python --require-import` and configures with
  `CLAY_BUILD_PYTHON=ON` for exactly this reason. Running the script bare, by
  hand, gets you the fallback. **Read the line it prints**: `imported <path>` is
  a real check, `parsed bindings/python/pyclay_module.cpp` is not. It also goes
  false-*red* against a stale module a different build tree happens to hold — the
  fix there is `cmake --build <dir> --target pyclay`, not a source change.
- **OpenSpec: CI pins `@fission-ai/openspec@1.8.0`.** An older local CLI passes
  deltas that CI rejects. 1.8.0's rule: a `## MODIFIED Requirements` block
  *replaces* the whole requirement, so the delta must repeat **every**
  `#### Scenario:` the live spec still carries. Pull them first:
  `awk '/^### Requirement: <name>/,/^### Requirement: [^X]/' openspec/specs/<cap>/spec.md`
- **`clay_bench` in `build/cpu-only` is stale by construction** — benchmarks are
  OFF in that cache. See the `claycore-bench` skill.
- **The Swift smoke consumes the prebuilt xcframework**, not the working tree,
  so it has been caught passing against an old one. `run_device_bench.sh`
  rebuilds it first for this reason.
- **Backend parity is a differential.** The parity cases pass *vacuously* when a
  backend failed to register, so a passing parity row on a build with no GPU
  proves nothing. Compare assertion counts against a CPU-only run:
  `clay_unit_tests -tc=*parity*,*registry*` and read the count, not the verdict.

## Before claiming a failure is not yours

Check it on `main` first, in the same build directory shape. Several standing
failures (the Metal benchmark row above, Metal parity on a runner with no Metal
device) are pre-existing and reproduce there.

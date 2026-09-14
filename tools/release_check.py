#!/usr/bin/env python3
"""Release checklist (build-packaging spec: "Release checklist enforced").

Runs every gate a claycore release must pass and prints a pass/fail table.
Used by hand before tagging and by .github/workflows/release.yml on a tag.

  python3 tools/release_check.py [--skip-slow] [--build-dir build/release]

Gates:
  version       CMake project version, C ABI version triple, and the pyclay
                wheel version agree
  abi           C ABI header hygiene + ctypes FFI exercise (check_c_abi.py)
  layering      module dependency rule
  dialect       kernel headers compile under every backend profile
  licenses      dependency manifest is permissive and in sync
  openspec      specs validate strictly
  task-symbols  every backticked identifier a tasks.md claims exists in the tree
  tests         full ctest suite (unit + C ABI smoke + CLI selftest)
  parity        every backend REGISTERED IN THIS BUILD matches CPU scalar, and
                the row NAMES THEM. A backend that was not compiled cannot fail
                this row, so the row also names what is missing and refuses to
                pass when the suite did not say what it compared. Read it as a
                DIFFERENTIAL: compare assertion counts against a CPU-only run.
  hardware/*    the four hardware gates no runner can do — CUDA parity, the
                nvcc build, OpenCL on a device, Vulkan on real silicon — were
                run or waived since the kernels last changed
                (tests/hardware/manual-gates.json)
  benchmarks    performance floors and the surface-nets/marching relation
  wheel         `pip install .` into a throwaway venv, then the quickstart
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from pathlib import Path

# Bounded build parallelism: see the --parallel call below for why a bare -j is
# not safe here. Honour CMAKE_BUILD_PARALLEL_LEVEL when a caller sets it.
BUILD_JOBS = int(os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL") or (os.cpu_count() or 4))

REPO = Path(__file__).resolve().parent.parent


class Checklist:
    def __init__(self) -> None:
        self.rows: list[tuple[str, bool, str]] = []

    def add(self, name: str, ok: bool, detail: str = "") -> None:
        self.rows.append((name, ok, detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{': ' + detail if detail else ''}",
              flush=True)

    def failed(self) -> list[str]:
        return [n for n, ok, _ in self.rows if not ok]


def run(cmd: list[str], cwd: Path | None = None,
        stdout_only: bool = False) -> tuple[bool, str]:
    proc = subprocess.run(cmd, cwd=cwd or REPO, capture_output=True, text=True)
    out = proc.stdout.strip() if stdout_only else (proc.stdout + proc.stderr).strip()
    return proc.returncode == 0, out


def check_versions(cl: Checklist) -> None:
    cmake = (REPO / "CMakeLists.txt").read_text()
    m = re.search(r"VERSION\s+(\d+)\.(\d+)\.(\d+)", cmake)
    cmake_version = tuple(m.groups()) if m else None

    header = (REPO / "bindings" / "c" / "clay.h").read_text()
    abi = tuple(
        re.search(rf"#define CLAY_ABI_{part}\s+(\d+)", header).group(1)
        for part in ("MAJOR", "MINOR", "PATCH")
    )

    pyproject = (REPO / "pyproject.toml").read_text()
    wheel = re.search(r'^version\s*=\s*"(\d+)\.(\d+)\.(\d+)"', pyproject, re.M)
    wheel_version = tuple(wheel.groups()) if wheel else None

    agree = cmake_version == abi == wheel_version
    cl.add("version", agree,
           f"cmake={'.'.join(cmake_version or '?')} abi={'.'.join(abi)} "
           f"wheel={'.'.join(wheel_version or '?')}")


# Paths whose contents decide what the engine DOES on a device, or what the
# harness MEASURES. A gate recorded before any of these changed is stale
# evidence.
#
# tests/device/ is in the list because leaving it out let the gate pass while
# certifying a suite that no longer existed: the harness grew from 25 cases to
# 50 and the recorded stamp still said PASS, because only engine paths were
# checked. A stamp has to name the same experiment that is about to ship.
DEVICE_RELEVANT = ("src/", "include/", "backends/", "bindings/", "CMakeLists.txt",
                   "tests/device/")

# ...except the gate's OWN outputs, which live under tests/device/ and are
# written by the run being recorded. Including them made the gate invalidate
# itself: the commit that records a passing run necessarily changes
# baseline.json and last-gate.json, so the stamp was stale the instant it was
# committed and the gate was red on main from the moment it landed. A check
# that cannot be satisfied is one people learn to ignore.
DEVICE_GATE_OUTPUTS = ("tests/device/baseline.json", "tests/device/last-gate.json")

# ...and pyclay's own tests, which are under bindings/ by location and are not
# the engine by any reading. The device harness is Swift against the
# xcframework: it never imports pyclay, so nothing in this directory can change
# what a verb costs on a tablet. Everything else under bindings/ stays in —
# bindings/c/clay.h is the surface the harness's own smoke test compiles
# against, and bindings/python/pyclay_module.cpp is excluded from this carve-out
# deliberately, because "it only builds a separate module" is an argument about
# the build graph rather than a fact about the file.
#
# The alternative is a ten-minute hardware run to re-certify a .py file, and a
# gate that expensive to satisfy for a change that cannot affect it is one
# people route around.
DEVICE_IRRELEVANT_PREFIXES = ("bindings/python/tests/",)


def device_relevant_changes(paths: list[str]) -> list[str]:
    """Of these changed paths, the ones that could change what a device measures.

    Split out of check_device_gate so the three-way rule — relevant prefix, minus
    the gate's own outputs, minus pyclay's tests — is one statement that can be
    read and tested, rather than a filter nobody can exercise without a git
    history to diff.
    """
    return [p for p in paths
            if any(p.startswith(prefix) for prefix in DEVICE_RELEVANT)
            and not any(p.startswith(skip) for skip in DEVICE_IRRELEVANT_PREFIXES)
            and p not in DEVICE_GATE_OUTPUTS]


def check_device_gate(cl: "Checklist") -> None:
    """The device gate ran, and it ran against this engine.

    No CI runner has an attached iPad, so the release cannot run the gate
    itself. What it CAN do is refuse to release code the gate has never seen:
    tools/check_device_bench.py records the commit it passed against, and this
    fails when the engine has changed since.

    Skipping instead of failing was rejected. A skipped hardware gate and a
    passing one are indistinguishable in a log, which is exactly how "Metal is
    the iPad app's production path" went unverified to v0.25.0.
    """
    stamp_path = REPO / "tests" / "device" / "last-gate.json"
    if not stamp_path.exists():
        cl.add("device", False,
               "no tests/device/last-gate.json — run tools/run_device_bench.sh "
               "with an iPad attached, then tools/check_device_bench.py")
        return
    try:
        stamp = json.loads(stamp_path.read_text())
    except json.JSONDecodeError as e:
        cl.add("device", False, f"last-gate.json is unreadable: {e}")
        return
    if not stamp.get("passed"):
        cl.add("device", False, "the recorded device gate did not pass")
        return

    # A commit id does not identify what ran if the tree was dirty when it
    # ran. That is not hypothetical: the first stamp this repo recorded named
    # a commit that did not contain the harness edit the run had actually
    # used, because the edit was still uncommitted.
    if stamp.get("treeDirty"):
        cl.add("device", False,
               "the recorded run was taken with uncommitted changes, so the "
               "commit it names does not identify the code that ran; commit "
               "and re-run the device bench")
        return

    commit = stamp.get("claycoreCommit")
    if not commit:
        cl.add("device", False, "the recorded device gate names no commit")
        return

    ok, out = run(["git", "diff", "--name-only", commit, "HEAD"])
    if not ok:
        # a shallow clone cannot see the recorded commit; say so rather than
        # passing on an unverifiable claim
        cl.add("device", False,
               f"cannot diff against the gated commit {commit[:9]} "
               f"(shallow clone?): {out.splitlines()[-1] if out else ''}")
        return
    changed = device_relevant_changes(out.splitlines())
    if changed:
        cl.add("device", False,
               f"engine changed since the gate ran at {commit[:9]}: "
               + ", ".join(changed[:3])
               + (f" (+{len(changed) - 3} more)" if len(changed) > 3 else ""))
        return
    # And the baseline must budget everything the recorded run measured. A
    # baseline with fewer budgets than the run has cases means the gate would
    # fail the moment it actually ran, which is not something to discover at
    # release time.
    baseline_path = REPO / "tests" / "device" / "baseline.json"
    if baseline_path.exists():
        try:
            budgets = json.loads(baseline_path.read_text()).get("budgets", {})
        except json.JSONDecodeError:
            budgets = {}
        recorded = stamp.get("caseCount") or 0
        if recorded and len(budgets) < recorded:
            cl.add("device", False,
                   f"the recorded run measured {recorded} case(s) but the "
                   f"baseline budgets only {len(budgets)}; re-run the device "
                   f"bench and update the baseline")
            return

    cl.add("device", True,
           f"{stamp.get('caseCount')} case(s) on {stamp.get('deviceModel')} "
           f"at {commit[:9]}")


# ---------------------------------------------------------------------------
# Backend parity, and the four gates no runner can run
# ---------------------------------------------------------------------------

# Printed by "parity: report which backends were actually compared" in
# tests/unit/test_parity.cpp. It is the only line in the run that names the
# backends the parity loop actually visited.
PARITY_MARKER = "PARITY_BACKENDS_CHECKED:"

# Backends that need hardware no CI runner here has. The parity case loops the
# REGISTRY, so one of these that was never compiled is not compared, cannot
# fail, and — until this row named them — was not mentioned either. That is how
# v0.113.0 shipped a kernel change under a 16/16 PASS table (#578): the row read
# "every backend matches CPU scalar" and the build contained cpu and metal.
HARDWARE_BACKENDS = ("cuda", "opencl", "vulkan")


def parity_backends(output: str) -> list[str]:
    """The backends a parity run reported having compared, in its own words."""
    m = re.search(re.escape(PARITY_MARKER) + r"[ \t]*([A-Za-z0-9_,.\- ]*)", output)
    return [name.strip() for name in m.group(1).split(",") if name.strip()] if m else []


def parity_row(ok: bool, output: str) -> tuple[bool, str]:
    """Verdict and detail for the parity row, from one run's exit status + output.

    Two rules, both of them about not implying coverage:

    * the row NAMES the backends that registered, and names the hardware ones
      that did not, so a reader cannot take a green row for a GPU result;
    * a run that did not say what it compared FAILS. Silence used to read as a
      pass, which is the same defect one level up.
    """
    registered = parity_backends(output)
    if not registered:
        return False, (f"the run reported no {PARITY_MARKER} line, so what it "
                       f"compared is unknown — this row cannot speak for any "
                       f"backend")

    cases = re.search(r"test cases:\s+(\d+)\s+\|\s+(\d+) passed", output)
    assertions = re.search(r"assertions:\s+(\d+)\s+\|\s+(\d+) passed", output)
    detail = "compared " + ",".join(registered)
    if cases:
        detail += f" | {cases.group(1)} cases"
    if assertions:
        detail += f" | {assertions.group(1)} assertions"
    missing = [b for b in HARDWARE_BACKENDS if b not in registered]
    if missing:
        detail += (" | NOT BUILT, so this row says nothing about them: "
                   + ", ".join(missing))
    return ok, detail


HARDWARE_GATE_RECORD = ("tests", "hardware", "manual-gates.json")

# What a GPU actually computes: the single-source kernel headers, the backends
# that compile them, the generators that derive the OpenCL and GLSL dialects,
# and the CMake that picks a CUDA architecture.
#
# The top-level CMakeLists.txt is deliberately NOT here, unlike DEVICE_RELEVANT.
# It carries the version line, so including it would expire all four gates on
# every release bump — and a version string is not kernel arithmetic. What does
# decide the nvcc build is cmake/ClayCudaArch.cmake, which is.
KERNEL_SOURCES = ("backends/", "include/clay/kernel/", "include/clay/eval/",
                  "cmake/ClayCudaArch.cmake", "tools/amalgamate_cl.py",
                  "tools/amalgamate_glsl.py")

# ...and the corpus the parity gates measure, which is an input to their result
# exactly as the kernels are. Same argument that put tests/device/ in
# DEVICE_RELEVANT: a stamp has to name the same experiment that is about to
# ship. #582 added the circ easings to the list the parity case reads, so a run
# recorded before it compared a corpus that did not contain them — and circ is
# the family #578 was filed about.
PARITY_CORPUS = ("tests/unit/test_parity.cpp",)

KERNEL_RELEVANT = KERNEL_SOURCES + PARITY_CORPUS


def kernel_relevant_changes(paths: list[str],
                            prefixes: tuple[str, ...] = KERNEL_RELEVANT) -> list[str]:
    """Of these changed paths, the ones that could change what a GPU answers."""
    return [p for p in paths if any(p.startswith(prefix) for prefix in prefixes)]


# The four things CI stopped gating on 2026-08-07, when the CUDA and OpenCL jobs
# were dropped for having no hardware behind them (docs/RELEASE.md). They are
# hardware-dependent and manual, and "manual" with nowhere to write the result
# down means the release table simply omits them.
#
# Each names what expires it. The three parity gates are experiments, so the
# corpus expires them; the nvcc gate is a BUILD, and a test file cannot change
# whether nvcc compiles the backend or which architecture it selects. A gate
# that fails for a reason it cannot be about is one people learn to re-stamp
# without reading.
MANUAL_HARDWARE_GATES = {
    "cuda-parity": ("CUDA device parity against the scalar reference",
                    KERNEL_RELEVANT),
    "nvcc-build": ("the nvcc build of the backend, including its architecture "
                   "auto-detection", KERNEL_SOURCES),
    "opencl-device": ("the OpenCL backend registers and passes parity on a real "
                      "device", KERNEL_RELEVANT),
    "vulkan-device": ("the Vulkan backend registers and passes parity on real "
                      "silicon — lavapipe runs the arithmetic on the CPU and is "
                      "not a substitute", KERNEL_RELEVANT),
}


def hardware_gate_verdict(entry: object, changed: list[str]) -> tuple[bool, str]:
    """Whether one manual gate is answered for the tree, and in what words.

    `changed` is the kernel-relevant diff between the commit the entry names and
    HEAD, computed by the caller because it needs git.

    A WAIVER passes. The point of this row is not that a GPU was found, it is
    that the release SAYS which of the four it ran and which it is shipping
    without — an unrunnable gate is one people delete.
    """
    if not isinstance(entry, dict):
        return False, ("never recorded — run it and record it, or record a "
                       "waiver with a reason")

    status = entry.get("status")
    if status not in ("run", "waived"):
        return False, (f"status {status!r} is neither 'run' nor 'waived'; a "
                       f"release has to say which")

    commit = entry.get("commit")
    if not commit:
        return False, f"{status}, but names no commit, so it certifies nothing"

    said = entry.get("evidence") if status == "run" else entry.get("reason")
    if not said:
        field = "evidence" if status == "run" else "reason"
        return False, f"{status} at {commit[:9]} with no {field}"

    if changed:
        return False, (f"{status} at {commit[:9]}, but the kernels changed "
                       f"since: " + ", ".join(changed[:3])
                       + (f" (+{len(changed) - 3} more)" if len(changed) > 3 else "")
                       + " — re-run it, or record a waiver against this tree")

    when = entry.get("date", "")
    return True, f"{status} at {commit[:9]}{' ' + when if when else ''}: {said}"


def check_hardware_gates(cl: "Checklist") -> None:
    """The four manual hardware gates, one row each.

    One row each rather than one summary row: a release has to state the four
    individually, and a failure has to name which of them is unanswered.
    """
    path = REPO.joinpath(*HARDWARE_GATE_RECORD)
    record: dict = {}
    problem = ""
    if not path.exists():
        problem = f"no {'/'.join(HARDWARE_GATE_RECORD)}"
    else:
        try:
            record = json.loads(path.read_text())
        except json.JSONDecodeError as e:
            problem = f"{'/'.join(HARDWARE_GATE_RECORD)} is unreadable: {e}"

    gates = record.get("gates", {}) if isinstance(record, dict) else {}
    for name, (what, expires_on) in MANUAL_HARDWARE_GATES.items():
        row = f"hardware/{name}"
        if problem:
            cl.add(row, False, f"{problem} — {what} is unrecorded")
            continue
        entry = gates.get(name)
        changed: list[str] = []
        if isinstance(entry, dict) and entry.get("commit"):
            ok, out = run(["git", "diff", "--name-only", entry["commit"], "HEAD"])
            if not ok:
                cl.add(row, False,
                       f"cannot diff against {str(entry['commit'])[:9]} "
                       f"(shallow clone?): {out.splitlines()[-1] if out else ''}")
                continue
            changed = kernel_relevant_changes(out.splitlines(), expires_on)
        ok, detail = hardware_gate_verdict(entry, changed)
        cl.add(row, ok, detail)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-slow", action="store_true",
                        help="skip the benchmark and wheel-install gates")
    parser.add_argument("--build-dir", default="build/release")
    args = parser.parse_args()

    print("claycore release checklist\n")
    cl = Checklist()
    build_dir = REPO / args.build_dir

    check_versions(cl)

    # CLAY_BUILD_PYTHON is ON here and nowhere else in this script's history,
    # because the "bindings" gate below is only a gate when a module exists to
    # import: check_binding_parity.py falls back to comparing the PARSED
    # pyclay_module.cpp against itself, which cannot catch a source/binary
    # disagreement and cannot fail. The release build produced no pyclay, so
    # that row had been passing on the fallback — a gate that reads as
    # "the bindings match the ABI" and was checking that the source matches
    # itself. CI's pyclay job covers this through pytest; the tag path did not.
    ok, out = run(["cmake", "-S", str(REPO), "-B", str(build_dir),
                   "-DCMAKE_BUILD_TYPE=Release", "-DCLAY_BUILD_BENCHMARKS=ON",
                   "-DCLAY_BUILD_PYTHON=ON"])
    cl.add("configure", ok, "" if ok else out[-400:])
    if not ok:
        return 1
    # --parallel with an explicit COUNT, not a bare -j. The default generator
    # here is Unix Makefiles, and `cmake --build ... -j` passes a bare -j
    # through to make, which means UNLIMITED: make forks every ready target at
    # once. Measured, a 40-source target spawns 47 concurrent compiles. On this
    # project that is ~100 parallel g++ against a hosted runner's memory, and
    # the runner SIGTERMs the whole process tree -- the checklist dies with exit
    # 143 after `configure`, with no compiler error and no failing gate to point
    # at. That is what killed v0.52.1's release workflow, twice, on a build that
    # passes on a developer machine with more RAM.
    ok, out = run(["cmake", "--build", str(build_dir), "--parallel", str(BUILD_JOBS)])
    cl.add("build", ok, "" if ok else out[-400:])
    if not ok:
        return 1

    ok, out = run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"])
    passed = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", out)
    cl.add("tests", ok, passed.group(0) if passed else out[-200:])

    # The unit suite's parity cases cover every backend REGISTERED IN THIS
    # BUILD. Name them: the row's old detail was a case count, which is the same
    # number on a CPU-only build and on one with two GPUs attached, so a reader
    # had nothing to distinguish "matches on CUDA" from "CUDA was never here".
    ok_parity, out_parity = run([str(build_dir / "tests" / "clay_unit_tests"),
                                 "-tc=*parity*,*registry*"])
    cl.add("parity", *parity_row(ok_parity, out_parity))

    # "bindings" rather than "parity", which already names the backend row
    # --pyclay names THIS build's module and --require-import refuses the
    # source-against-source fallback, so the row fails rather than passes when
    # the module is missing. Both are needed: the flag alone would let another
    # build tree answer, which is how the same gate went false-RED on v0.49.0
    # against a module built before the commit under test.
    parity_args = ["--pyclay", str(build_dir / "bindings" / "python"),
                   "--require-import"]
    for name, script, extra in (("layering", "check_layering.py", []),
                                ("dialect", "check_kernel_dialect.py", []),
                                ("licenses", "check_licenses.py", []),
                                ("task-symbols", "check_task_symbols.py", []),
                                ("bindings", "check_binding_parity.py",
                                 parity_args)):
        ok, out = run([sys.executable, str(REPO / "tools" / script)] + extra)
        cl.add(name, ok, out.splitlines()[-1] if out else "")

    # the kernels artifact hosts build their GPU previews from: it must still
    # be a byte-identical copy that compiles on its own (docs/06)
    pkg = [sys.executable, str(REPO / "tools" / "package_kernels.py")]
    ok, out = run(pkg + ["--clay", str(build_dir / "clay")])
    if ok:
        ok, out = run(pkg + ["--verify"])
    cl.add("kernels", ok, out.splitlines()[-1] if out else "")

    shared = next((p for p in (build_dir / "libclay_shared.so",
                               build_dir / "libclay_shared.dylib") if p.exists()), None)
    cmd = [sys.executable, str(REPO / "tools" / "check_c_abi.py")]
    if shared:
        cmd.append(str(shared))
    ok, out = run(cmd)
    cl.add("abi", ok, out.splitlines()[-1] if out else "")

    ok, out = run(["openspec", "validate", "--all", "--strict"])
    cl.add("openspec", ok, out.splitlines()[-1] if out else "")

    check_device_gate(cl)
    check_hardware_gates(cl)

    if not args.skip_slow:
        bench_json = build_dir / "release_bench.json"
        # stdout only: benchmark writes its banner to stderr, which would
        # otherwise land in the JSON
        ok, out = run([str(build_dir / "clay_bench"), "--benchmark_format=json",
                       "--benchmark_min_time=0.2s"], stdout_only=True)
        if ok:
            bench_json.write_text(out)
            ok, out = run([sys.executable, str(REPO / "tools" / "check_bench.py"),
                           str(bench_json)])
        cl.add("benchmarks", ok, out.splitlines()[-1] if out else "")

        with tempfile.TemporaryDirectory() as tmp:
            venv = Path(tmp) / "venv"
            ok, out = run([sys.executable, "-m", "venv", str(venv)])
            pip = venv / "bin" / "pip"
            py = venv / "bin" / "python"
            if not pip.exists():  # windows layout
                pip = venv / "Scripts" / "pip.exe"
                py = venv / "Scripts" / "python.exe"
            if ok:
                ok, out = run([str(pip), "install", "-q", "numpy", str(REPO)])
            if ok:
                ok, out = run([str(py), "-c", (
                    "import numpy as np, pyclay as clay;"
                    "d = clay.Document();"
                    "l = d.add_sdf_layer('body');"
                    "l.add(clay.Sphere(r=1.0), color='#38a6cf');"
                    "v = d.eval(np.array([[0,0,0],[3,0,0]], dtype=np.float32));"
                    "assert v[0] < 0 < v[1];"
                    "m = d.mesh(resolution=48);"
                    "assert m.is_watertight();"
                    "print('quickstart ok', clay.backends())")])
            cl.add("wheel", ok, out.splitlines()[-1] if out else "")

    print()
    failures = cl.failed()
    if failures:
        print(f"release check FAILED: {', '.join(failures)}")
        return 1
    print("release check: all gates passed — safe to tag")
    return 0


if __name__ == "__main__":
    sys.exit(main())

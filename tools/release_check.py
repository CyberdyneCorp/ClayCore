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
  tests         full ctest suite (unit + C ABI smoke + CLI selftest)
  parity        every backend registered in this build matches CPU scalar
                (that is what the unit suite's parity cases assert). Read it
                as a DIFFERENTIAL: the cases pass vacuously when a backend
                failed to register, so configure with the backends on and
                compare assertion counts against a CPU-only run.
  benchmarks    performance floors and the surface-nets/marching relation
  wheel         `pip install .` into a throwaway venv, then the quickstart
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from pathlib import Path

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
    changed = [p for p in out.splitlines()
               if any(p.startswith(prefix) for prefix in DEVICE_RELEVANT)
               and p not in DEVICE_GATE_OUTPUTS]
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
    ok, out = run(["cmake", "--build", str(build_dir), "-j"])
    cl.add("build", ok, "" if ok else out[-400:])
    if not ok:
        return 1

    ok, out = run(["ctest", "--test-dir", str(build_dir), "--output-on-failure"])
    passed = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", out)
    cl.add("tests", ok, passed.group(0) if passed else out[-200:])

    # the unit suite's parity cases cover every backend registered in this
    # build; surface that explicitly so the checklist records it
    ok_parity, out_parity = run([str(build_dir / "tests" / "clay_unit_tests"),
                                 "-tc=*parity*,*registry*"])
    cases = re.search(r"test cases:\s+(\d+)\s+\|\s+(\d+) passed", out_parity)
    cl.add("parity", ok_parity, cases.group(0) if cases else out_parity[-200:])

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

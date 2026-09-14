"""The release checklist may not imply backend coverage it does not have.

v0.113.0 was tagged with a 16/16 PASS table, a kernel change in it, and no CUDA,
OpenCL or Vulkan run behind any of those rows (#578). Nothing malfunctioned: the
parity row asserts "every backend REGISTERED IN THIS BUILD matches CPU scalar",
the release build registered cpu and metal, and a backend that was never
compiled cannot fail a row it never reaches.

So these are tests of the CHECKER, and they are about what it is allowed to say:

* a CPU-only parity run must produce a row that names cpu and names the GPU
  backends it did NOT compare — the old detail was a test-case count, which is
  the same number with two GPUs attached and with none;
* a run that does not say what it compared must FAIL rather than pass quietly,
  because silence reading as success is the same defect one level up;
* the four manual hardware gates must each be answered — run or waived, against
  a commit, with a sentence — and must go stale when the kernels move under
  them.

They need no GPU, no build and no git history: every rule is a pure function
over strings, which is the only reason a gate about hardware can be gated here.
"""

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

rc = pytest.importorskip("release_check")

# What the unit suite prints on a machine with no GPU backend compiled in. The
# case count and the assertion count are real ones from a cpu-only run; the
# point of the fixture is that NOTHING in it distinguishes this from a run where
# CUDA registered, except the marker line.
CPU_ONLY_RUN = """\
PARITY_BACKENDS_CHECKED: cpu
[doctest] test cases:  46 |  46 passed | 0 failed | 0 skipped
[doctest] assertions: 543 | 543 passed | 0 failed |
[doctest] Status: SUCCESS!
"""

GPU_RUN = """\
PARITY_BACKENDS_CHECKED: cpu,cuda,opencl
[doctest] test cases:   46 |   46 passed | 0 failed | 0 skipped
[doctest] assertions: 1621 | 1621 passed | 0 failed |
[doctest] Status: SUCCESS!
"""


def test_a_cpu_only_run_may_not_read_as_backend_coverage():
    """The row that shipped v0.113.0. It passes — and it must say what for."""
    ok, detail = rc.parity_row(True, CPU_ONLY_RUN)
    assert ok
    assert "cpu" in detail
    for absent in ("cuda", "opencl", "vulkan"):
        assert absent in detail, f"the row does not mention {absent} at all"
    assert "NOT BUILT" in detail
    # and it still carries the number RELEASE.md tells a releaser to read
    assert "543 assertions" in detail


def test_the_detail_is_not_just_a_case_count():
    """46 cases is what a cpu-only run and a two-GPU run BOTH report, which is
    why the previous detail could not tell them apart."""
    _, cpu_detail = rc.parity_row(True, CPU_ONLY_RUN)
    _, gpu_detail = rc.parity_row(True, GPU_RUN)
    assert cpu_detail != gpu_detail
    assert "46 cases" in cpu_detail and "46 cases" in gpu_detail


def test_a_gpu_run_names_the_gpus_and_still_names_what_is_missing():
    ok, detail = rc.parity_row(True, GPU_RUN)
    assert ok
    assert "cuda" in detail and "opencl" in detail
    assert "1621 assertions" in detail
    # vulkan was not in this build either, and saying so is the whole job
    assert "vulkan" in detail and "NOT BUILT" in detail


def test_a_run_that_does_not_say_what_it_compared_fails():
    """Exit code 0 is not evidence. A suite that printed no marker compared an
    unknown set, and an unknown set is not a pass."""
    silent = "[doctest] assertions: 543 | 543 passed | 0 failed |\n"
    ok, detail = rc.parity_row(True, silent)
    assert not ok
    assert rc.PARITY_MARKER in detail


def test_a_failing_run_stays_failed_however_well_it_reports():
    ok, _ = rc.parity_row(False, GPU_RUN)
    assert not ok


def test_backends_are_read_from_the_marker_only():
    assert rc.parity_backends(GPU_RUN) == ["cpu", "cuda", "opencl"]
    # the word appearing elsewhere in the log is not registration
    assert rc.parity_backends("building cuda_backend.cpp ... ok\n") == []


@pytest.mark.parametrize("path", [
    "backends/cuda/cuda_backend.cpp",
    "backends/opencl/clay_kernels.cl.in",
    "include/clay/kernel/ease.h",
    "include/clay/eval/backend.h",
    "cmake/ClayCudaArch.cmake",
    "tools/amalgamate_cl.py",
    # the corpus IS the experiment: a run recorded before #582 compared a list
    # of easings that did not contain circ
    "tests/unit/test_parity.cpp",
])
def test_these_expire_a_recorded_hardware_run(path):
    assert rc.kernel_relevant_changes([path]) == [path]


@pytest.mark.parametrize("path", [
    "docs/RELEASE.md",
    "README.md",
    "bindings/python/tests/test_pyclay.py",
    "tests/device/baseline.json",
    "src/voxel/grid.cpp",
    # deliberately absent, unlike the device gate's list: it carries the version
    # line, and expiring four hardware gates on every release bump would make
    # them noise
    "CMakeLists.txt",
])
def test_these_do_not(path):
    assert rc.kernel_relevant_changes([path]) == []


FRESH_RUN = {"status": "run", "commit": "e8ca6d59351874", "date": "2026-09-13",
             "evidence": "RTX 5060: 543 cpu-only, 1082 with cuda"}


def test_an_unrecorded_gate_fails():
    ok, detail = rc.hardware_gate_verdict(None, [])
    assert not ok
    assert "never recorded" in detail


def test_a_recorded_run_with_nothing_changed_passes_and_says_where():
    ok, detail = rc.hardware_gate_verdict(FRESH_RUN, [])
    assert ok
    assert "e8ca6d59" in detail and "RTX 5060" in detail


def test_a_recorded_run_expires_when_the_kernels_move():
    ok, detail = rc.hardware_gate_verdict(
        FRESH_RUN, ["backends/cuda/cuda_backend.cpp"])
    assert not ok
    assert "backends/cuda/cuda_backend.cpp" in detail


def test_a_run_with_no_evidence_is_not_a_run():
    entry = dict(FRESH_RUN)
    del entry["evidence"]
    ok, detail = rc.hardware_gate_verdict(entry, [])
    assert not ok
    assert "evidence" in detail


def test_a_run_that_names_no_commit_certifies_nothing():
    entry = dict(FRESH_RUN)
    del entry["commit"]
    assert rc.hardware_gate_verdict(entry, [])[0] is False


def test_a_waiver_with_a_reason_passes():
    """A gate nobody can run is one that gets deleted. Saying "shipping without
    it, here is why" is a release decision; leaving the row off the table is
    not."""
    ok, detail = rc.hardware_gate_verdict(
        {"status": "waived", "commit": "e8ca6d59351874",
         "reason": "no Vulkan device in the fleet; lavapipe is CPU arithmetic"},
        [])
    assert ok
    assert "waived" in detail and "lavapipe" in detail


def test_a_waiver_without_a_reason_does_not():
    ok, detail = rc.hardware_gate_verdict(
        {"status": "waived", "commit": "e8ca6d59351874"}, [])
    assert not ok
    assert "reason" in detail


def test_a_waiver_expires_too():
    """A waiver is a statement about a tree, not a permanent exemption."""
    ok, _ = rc.hardware_gate_verdict(
        {"status": "waived", "commit": "e8ca6d59351874", "reason": "no device"},
        ["include/clay/kernel/ease.h"])
    assert not ok


def test_an_invented_status_fails():
    ok, detail = rc.hardware_gate_verdict(
        {"status": "probably fine", "commit": "e8ca6d59351874",
         "evidence": "it built"}, [])
    assert not ok
    assert "run" in detail and "waived" in detail


def test_the_four_gates_are_the_four_RELEASE_md_names():
    assert set(rc.MANUAL_HARDWARE_GATES) == {
        "cuda-parity", "nvcc-build", "opencl-device", "vulkan-device"}


def test_the_corpus_expires_the_parity_gates_but_not_the_build_gate():
    """nvcc either compiles the backend and picks an architecture or it does
    not; a test file cannot change that. A gate that fails for a reason it
    cannot be about is one people re-stamp without reading."""
    corpus = list(rc.PARITY_CORPUS)
    for name in ("cuda-parity", "opencl-device", "vulkan-device"):
        _, expires_on = rc.MANUAL_HARDWARE_GATES[name]
        assert rc.kernel_relevant_changes(corpus, expires_on) == corpus

    _, nvcc = rc.MANUAL_HARDWARE_GATES["nvcc-build"]
    assert rc.kernel_relevant_changes(corpus, nvcc) == []
    # but a kernel or the arch selection still does expire it
    for path in ("backends/cuda/clay_kernels.cu", "cmake/ClayCudaArch.cmake"):
        assert rc.kernel_relevant_changes([path], nvcc) == [path]


def test_the_checked_in_record_answers_every_gate_in_a_readable_shape():
    """Staleness is not asserted here — the record goes stale on purpose when
    the kernels move, and that is the row's job, not a test failure. What must
    hold is that every gate has an entry the checker can read."""
    record = json.loads(REPO.joinpath(*rc.HARDWARE_GATE_RECORD).read_text())
    for name in rc.MANUAL_HARDWARE_GATES:
        entry = record["gates"][name]
        # an entry against its OWN commit, with nothing changed, must be valid
        ok, detail = rc.hardware_gate_verdict(entry, [])
        assert ok, f"{name}: {detail}"


# -- the WIRING, which the cases above do not reach ---------------------------
#
# Every case above tests a pure function. An adversarial review of the PR that
# added them deleted `check_hardware_gates(cl)` from `main()` AND reverted the
# parity row to the old case-count form, and this file still reported
# `31 passed`. The requirement #578 states is about the printed checklist
# TABLE, and that was exactly the part nothing held.
#
# So these two drive `main()` itself. They stub every subprocess and both
# file-backed gates, because what is under test is which rows main ASKS FOR,
# not whether this machine can pass them.


def _drive_main(monkeypatch, tmp_path):
    """Run main() with every external dependency stubbed, and return its rows."""
    rows: list[tuple[str, bool, str]] = []

    def fake_run(cmd, cwd=None, stdout_only=False):
        # The parity invocation is the one whose OUTPUT matters: it has to reach
        # parity_row, which is what turns a marker line into a backend list.
        if any("clay_unit_tests" in str(c) and "parity" in str(c) for c in cmd):
            return True, GPU_RUN
        return True, "ok\n[doctest] assertions: 1 | 1 passed | 0 failed |\n"

    monkeypatch.setattr(rc, "run", fake_run)
    monkeypatch.setattr(rc, "check_versions", lambda cl: cl.add("version", True, "stub"))
    monkeypatch.setattr(rc, "check_device_gate", lambda cl: cl.add("device", True, "stub"))

    real_add = rc.Checklist.add

    def recording_add(self, name, ok, detail=""):
        rows.append((name, ok, detail))
        real_add(self, name, ok, detail)

    monkeypatch.setattr(rc.Checklist, "add", recording_add)
    monkeypatch.setattr(sys, "argv", ["release_check.py", "--skip-slow",
                                      "--build-dir", str(tmp_path)])
    rc.main()
    return rows


def test_main_asks_for_the_hardware_rows_at_all(monkeypatch, tmp_path):
    """The gate is the printed table, so the table is what this asserts.

    Deleting `check_hardware_gates(cl)` from main() must fail here. Nothing
    else in this file notices, because every other case calls the function
    directly.
    """
    rows = _drive_main(monkeypatch, tmp_path)
    names = {name for name, _, _ in rows}
    for expected in ("hardware/cuda-parity", "hardware/opencl-device",
                     "hardware/vulkan-device"):
        assert expected in names, f"{expected} is not on the checklist: {sorted(names)}"


def test_the_parity_row_main_prints_names_the_backends(monkeypatch, tmp_path):
    """Reverting main's parity row to the old case count must fail here.

    The old form printed `46 | 46 passed`, which reads identically whether two
    GPUs were compared or none were present — the reading failure #578 is
    about. The row main actually prints has to carry the backend list.
    """
    rows = _drive_main(monkeypatch, tmp_path)
    parity = [detail for name, _, detail in rows if name == "parity"]
    assert parity, "main() printed no parity row"
    assert "cuda" in parity[0] and "opencl" in parity[0], (
        f"the parity row does not name what was compared: {parity[0]!r}")

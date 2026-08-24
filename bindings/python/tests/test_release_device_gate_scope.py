"""What counts as "the engine changed" for the release device gate.

The gate refuses to release code the hardware run has never seen, and it decides
that by prefix. bindings/ is in the list because bindings/c/clay.h is the surface
the device harness compiles against — but pyclay's own tests live under the same
prefix and cannot affect a tablet, since the harness is Swift against the
xcframework and never imports pyclay.

The carve-out is narrow on purpose. A gate that is cheap to satisfy for changes
that cannot affect it stays trusted; one that demands a ten-minute hardware run
for a .py file gets routed around.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

rc = pytest.importorskip("release_check")


def test_pyclay_tests_do_not_invalidate_a_hardware_run():
    assert rc.device_relevant_changes([
        "bindings/python/tests/test_binding_parity.py",
        "bindings/python/tests/test_device_canary_attribution.py",
    ]) == []


def test_the_c_header_still_does():
    """The reason bindings/ is in the list at all."""
    assert rc.device_relevant_changes(["bindings/c/clay.h"]) == ["bindings/c/clay.h"]


def test_the_python_module_itself_still_does():
    """The carve-out is tests only. pyclay_module.cpp is excluded from it
    deliberately — "it only builds a separate module" is a claim about the build
    graph, not a fact about the file."""
    changed = ["bindings/python/pyclay_module.cpp"]
    assert rc.device_relevant_changes(changed) == changed


@pytest.mark.parametrize("path", [
    "src/voxel/grid.cpp", "include/clay/kernel/tape.h",
    "backends/metal/metal_backend.mm", "CMakeLists.txt",
    "tests/device/Shared/Coverage.swift",
])
def test_the_engine_and_the_harness_still_do(path):
    assert rc.device_relevant_changes([path]) == [path]


def test_the_gates_own_outputs_never_do():
    """Including them made the gate invalidate itself: the commit that records a
    passing run necessarily changes them."""
    assert rc.device_relevant_changes(list(rc.DEVICE_GATE_OUTPUTS)) == []


def test_unrelated_paths_are_not_swept_in():
    assert rc.device_relevant_changes(
        ["docs/RELEASE.md", "tools/check_device_bench.py", "README.md"]) == []

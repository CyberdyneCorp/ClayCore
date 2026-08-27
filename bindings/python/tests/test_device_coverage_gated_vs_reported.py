"""Coverage says which verbs are GATED and which are only REPORTED.

`coverage: OK — 60 verb(s) measured` read as "these verbs are protected". For
most of them it meant "a number exists": `check_device_bench` requires a
regression to be both over the tolerance AND over `NOISE_FLOOR_MS`, so a case
measuring 6 us cannot fail at any plausible ratio — the floor is the binding
constraint long before the tolerance is (issue #337).

That is the same shape as the `bindings` row that compared the parsed source
against itself, and as `move_drags`'s budget sitting between its two modes: the
tool reported something other than what it checked. Saying which is which costs
nothing and does not pretend to fix the measurement.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

cov = pytest.importorskip("check_device_coverage")
bench = pytest.importorskip("check_device_bench")


def case(name, values, samples=200):
    return {"name": name, "budgetClass": "interactive",
            "measurements": [{"p95Ms": v, "p50Ms": v, "samples": samples,
                              "repeats": 1, "stamps": i + 1}
                             for i, v in enumerate(values)]}


def test_a_case_far_above_the_floor_is_gated_at_the_tolerance():
    """Big enough that the tolerance binds: the floor never enters."""
    measured, ratio = cov.gate_reach(case("sdf_consolidate", [10.0, 100.0, 300.0]))
    assert measured == pytest.approx(300.0)
    assert ratio == pytest.approx(bench.DEFAULT_TOLERANCE)


def test_a_case_below_the_floor_needs_far_more_than_the_tolerance():
    """6 us: a 1.4x regression is 2.4 us, which the floor is right to ignore."""
    measured, ratio = cov.gate_reach(case("voxel_pinch", [0.006]))
    assert measured == pytest.approx(0.006)
    assert ratio > 8  # (0.006 + 0.05) / 0.006
    assert ratio > bench.DEFAULT_TOLERANCE


def test_the_boundary_is_where_the_floor_stops_binding():
    """Above NOISE_FLOOR_MS / (tolerance - 1) the tolerance takes over."""
    boundary = bench.NOISE_FLOOR_MS / (bench.DEFAULT_TOLERANCE - 1)
    _, just_over = cov.gate_reach(case("x", [boundary * 1.05]))
    _, just_under = cov.gate_reach(case("x", [boundary * 0.95]))
    assert just_over == pytest.approx(bench.DEFAULT_TOLERANCE)
    assert just_under > bench.DEFAULT_TOLERANCE


def test_a_case_with_no_measurement_reports_no_ratio():
    """Absent rather than infinitely sensitive, so it cannot be counted gated."""
    measured, ratio = cov.gate_reach({"name": "x", "measurements": []})
    assert measured == 0.0
    assert ratio is None


def test_a_zero_measurement_does_not_divide_by_zero():
    measured, ratio = cov.gate_reach(case("x", [0.0]))
    assert ratio is None


def test_it_uses_the_gate_s_OWN_statistic_for_a_single_timing_case():
    """Or coverage would judge sensitivity on a figure the gate never scores.

    A single-timing case is scored on the median of its axis (#331), so the
    reach must be computed from the median too — using the max would call a case
    gateable that the gate cannot actually fail.
    """
    values = [0.400, 0.090, 0.103, 0.099, 0.113, 0.128, 0.119, 0.129]
    single = case("move_drags", values, samples=1)
    measured, _ = cov.gate_reach(single)
    assert measured == pytest.approx(bench.worst_p95(single))
    assert measured < max(values)   # the median, not the warm-up draw


def test_the_two_tools_agree_about_the_floor():
    """Imported rather than copied, so they cannot drift apart."""
    assert cov.NOISE_FLOOR_MS == bench.NOISE_FLOOR_MS
    assert cov.TOLERANCE == bench.DEFAULT_TOLERANCE

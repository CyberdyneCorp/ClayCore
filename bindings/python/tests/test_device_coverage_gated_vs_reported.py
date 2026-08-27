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


BUNDLE = "com.cyberdyne.claycore.devicemeasure"


def case(name, values, samples=200, canary=None):
    """`canary` is (before, after) in ms; None leaves the case unbracketed."""
    c = {"name": name, "budgetClass": "interactive",
         "measurements": [{"p95Ms": v, "p50Ms": v, "samples": samples,
                           "repeats": 1, "stamps": i + 1}
                          for i, v in enumerate(values)]}
    if canary is not None:
        c["bundle"] = BUNDLE
        c["canaryBeforeMs"], c["canaryAfterMs"] = canary
    return c


def run(*cases, settled=100.0):
    """The record a case arrives in. `settled` is the canary's rested value,
    which is the denominator normalisation divides against — grouped by bundle,
    because a run spans three processes and each times from its own start."""
    return {"cases": list(cases),
            "canary": [{"atMs": 0.0, "ms": settled, "bundle": BUNDLE,
                        "thermalState": "nominal"}]}


def reach(c, record=None):
    return cov.gate_reach(record if record is not None else run(c), c)


def test_a_case_far_above_the_floor_is_gated_at_the_tolerance():
    """Big enough that the tolerance binds: the floor never enters."""
    measured, ratio = reach(case("sdf_consolidate", [10.0, 100.0, 300.0]))
    assert measured == pytest.approx(300.0)
    assert ratio == pytest.approx(bench.DEFAULT_TOLERANCE)


def test_a_case_below_the_floor_needs_far_more_than_the_tolerance():
    """6 us: a 1.4x regression is 2.4 us, which the floor is right to ignore."""
    measured, ratio = reach(case("voxel_pinch", [0.006]))
    assert measured == pytest.approx(0.006)
    assert ratio > 8  # (0.006 + 0.05) / 0.006
    assert ratio > bench.DEFAULT_TOLERANCE


def test_the_boundary_is_where_the_floor_stops_binding():
    """Above NOISE_FLOOR_MS / (tolerance - 1) the tolerance takes over."""
    boundary = bench.NOISE_FLOOR_MS / (bench.DEFAULT_TOLERANCE - 1)
    _, just_over = reach(case("x", [boundary * 1.05]))
    _, just_under = reach(case("x", [boundary * 0.95]))
    assert just_over == pytest.approx(bench.DEFAULT_TOLERANCE)
    assert just_under > bench.DEFAULT_TOLERANCE


def test_a_case_with_no_measurement_reports_no_ratio():
    """Absent rather than infinitely sensitive, so it cannot be counted gated."""
    measured, ratio = reach({"name": "x", "measurements": []})
    assert measured == 0.0
    assert ratio is None


def test_a_zero_measurement_does_not_divide_by_zero():
    measured, ratio = reach(case("x", [0.0]))
    assert ratio is None


def test_it_uses_the_gate_s_OWN_statistic_for_a_single_timing_case():
    """Or coverage would judge sensitivity on a figure the gate never scores.

    A single-timing case is scored on the median of its axis (#331), so the
    reach must be computed from the median too — using the max would call a case
    gateable that the gate cannot actually fail.
    """
    values = [0.400, 0.090, 0.103, 0.099, 0.113, 0.128, 0.119, 0.129]
    single = case("move_drags", values, samples=1)
    measured, _ = reach(single)
    assert measured == pytest.approx(bench.worst_p95(single))
    assert measured < max(values)   # the median, not the warm-up draw


def test_it_judges_on_the_NORMALISED_figure_the_gate_decides_on():
    """Or it answers a different question than the gate asks.

    This read the RAW worst p95. The two part company exactly when the device
    was slow while a case ran: normalisation divides the measurement by that
    slowdown, so a case can clear the floor raw and sit under it normalised —
    reported GATED while the gate cannot fail it.

    `sdf_move` was that case. Raw 0.1324 ms reads as gated; normalised 0.1158 ms
    needs 1.43x, and a real 1.46x regression in `layer_move_surface` sat under
    it unreported from v0.52.2.
    """
    hot = case("sdf_move", [0.0015, 0.0133, 0.1324], canary=(114.3, 114.3))
    measured, ratio = reach(hot, run(hot, settled=100.0))
    assert measured == pytest.approx(0.1324 / 1.143, rel=1e-3)  # divided by the slowdown
    assert measured < bench.worst_p95(hot)                      # and below the raw figure
    assert ratio > bench.DEFAULT_TOLERANCE                       # so: REPORTED, not GATED


def test_an_unbracketed_case_is_still_judged_raw():
    """A case that carries no bracket is compared raw by the gate, so it must
    be judged raw here too — guessing a factor is what produced #297."""
    plain = case("x", [0.3])
    measured, _ = reach(plain)
    assert measured == pytest.approx(bench.worst_p95(plain))


def test_the_two_tools_agree_about_the_floor():
    """Imported rather than copied, so they cannot drift apart."""
    assert cov.NOISE_FLOOR_MS == bench.NOISE_FLOOR_MS
    assert cov.TOLERANCE == bench.DEFAULT_TOLERANCE

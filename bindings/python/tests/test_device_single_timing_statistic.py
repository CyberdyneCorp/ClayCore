"""What a SINGLE-TIMING case is scored on, and why it is not the worst point.

The devicegallery sessions time each stroke of a progressive sculpt once, so
their axis is a sequence of one-shot observations rather than a percentile at
each size. The max of N one-shot draws is the worst draw, and gating on it made
five of the six gallery cases un-gateable: their budgets landed BETWEEN the two
modes the cases actually produce, so the gate fired on which draw it happened to
get.

`move_drags` is the one that was caught doing it. Two records at the SAME commit
(444efea3a, ABI 0.40.0) read 0.1490 and 0.4343 — a 2.91x difference on identical
code — and two consecutive high draws on main cost a full off-device bisect
across four merges before the case's own history said it had produced both modes
without the engine changing (issue #331).

Measured over 18 records spanning five ABI versions, worst spread per statistic:

    max of all passes    5.94x   <- what the gate used to score on
    median of all passes 1.55x   <- what it scores on now
    max of passes 2..N   1.59x
    median of 2..N       1.75x
    last pass            1.77x

against a tolerance of 1.4x.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

check = pytest.importorskip("check_device_bench")


def point(p95, samples):
    """One axis point. `samples` is what makes it a percentile or a single shot."""
    return {"p95Ms": p95, "p50Ms": p95, "samples": samples, "repeats": 1, "stamps": 1}


def gallery_case(values):
    """A progressive session: every point is ONE timing."""
    return {"name": "move_drags", "budgetClass": "gesture",
            "measurements": [point(v, 1) for v in values]}


def measured_case(values):
    """A latency case: every point is a percentile over many samples."""
    return {"name": "sdf_stamp_cpu", "budgetClass": "interactive",
            "measurements": [point(v, 200) for v in values]}


# The shape actually recorded: a warm-up first pass, then the steady session.
WARMUP_THEN_STEADY = [0.400, 0.090, 0.103, 0.099, 0.113, 0.128, 0.119, 0.129]
NO_WARMUP = [0.133, 0.083, 0.098, 0.081, 0.097, 0.117, 0.124, 0.139]


def test_a_single_timing_case_is_scored_on_its_median():
    scored = check.worst_p95(gallery_case(WARMUP_THEN_STEADY))
    assert scored == pytest.approx(0.116, abs=0.001)
    # NOT the worst point, which is the warm-up.
    assert scored < 0.400


def test_a_measured_case_is_still_scored_on_its_worst_point():
    """A budget is a ceiling on the worst point of the axis, and stays one.

    This is the half that must not change: a latency case's points are
    percentiles over 200 samples at DIFFERENT document sizes, so the largest is
    the one the budget is about.
    """
    scored = check.worst_p95(measured_case([0.05, 0.29, 2.42]))
    assert scored == pytest.approx(2.42)


def test_the_warm_up_pass_no_longer_decides_the_verdict():
    """The coin flip: the same session, with and without a high first draw.

    Both records are the same engine. Before this, one passed and one failed.
    """
    with_warmup = check.worst_p95(gallery_case(WARMUP_THEN_STEADY))
    without = check.worst_p95(gallery_case(NO_WARMUP))
    assert with_warmup / without < 1.4  # inside the gate's own tolerance


def test_the_worst_point_is_still_reported():
    """Scoring on the median must not HIDE the slow stroke, which really happened."""
    c = gallery_case(WARMUP_THEN_STEADY)
    assert check.reported_p95(c) == pytest.approx(0.400)
    assert check.reported_p95(c) > check.worst_p95(c)


def test_a_real_regression_across_the_whole_axis_still_fails():
    """The teeth. A median that ignored real movement would be worse than noise."""
    before = check.worst_p95(gallery_case(WARMUP_THEN_STEADY))
    after = check.worst_p95(gallery_case([v * 1.6 for v in WARMUP_THEN_STEADY]))
    assert after / before == pytest.approx(1.6, abs=0.01)


def test_growth_along_the_axis_is_not_hidden():
    """A session whose CHEAPEST pass is first — the ones that genuinely grow.

    snakehook_tendrils and noise_detail read growth ^+0.45 and ^+0.51, and the
    median was the most stable statistic for those two as well. What matters
    here is that a regression scaling the curve still moves the score.
    """
    grows = [0.067, 0.069, 0.100, 0.136, 0.143, 0.140, 0.159, 0.172]
    before = check.worst_p95(gallery_case(grows))
    after = check.worst_p95(gallery_case([v * 1.5 for v in grows]))
    assert after / before == pytest.approx(1.5, abs=0.01)


def test_single_observation_is_what_selects_the_statistic():
    """The two must agree, or a case would be scored one way and noted another."""
    assert check.single_observation(gallery_case(NO_WARMUP)) is True
    assert check.single_observation(measured_case([0.05, 0.29, 2.42])) is False


def test_an_empty_case_does_not_crash_either_statistic():
    empty = {"name": "x", "budgetClass": "gesture", "measurements": []}
    assert check.worst_p95(empty) != check.worst_p95(empty)        # nan
    assert check.reported_p95(empty) != check.reported_p95(empty)  # nan

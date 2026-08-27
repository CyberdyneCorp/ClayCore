"""A budget so far above its case that it cannot object to anything.

The mirror of issue #337, from the other side. There, a case recorded a number
it could never object to because the MEASUREMENT sat under the gate's absolute
floor. Here the measurement is fine and the BUDGET is too large: `volume_hpolish`
measured 3.37 ms against a 132.40 ms budget, so it could have got 36x slower
with nothing failing.

The regression arm was no help, and that is structural rather than bad luck.
`write_baseline` writes `budgets` and `cases[]` from the same run, so whatever
staleness reaches one reaches the other — volume_hpolish's regression reference
was the same 0.49.0-era 88.27 ms, tripping at 123.57 ms. Both arms of the gate
had been decorative on these three verbs since the flatten work landed.

So budget-vs-run slack is a sound proxy for BOTH halves going stale, and one
ratio detects it. Reported, never failed: a generous ceiling can be deliberate
over content-varying work, which is why the class is printed and a human decides.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

bench = pytest.importorskip("check_device_bench")


def test_a_budget_at_its_measurement_has_no_slack():
    assert bench.budget_reach(1.0, 1.0) == pytest.approx(1.0)


def test_a_freshly_derived_budget_is_well_inside_the_threshold():
    """`--update` derives 1.5x, so a fresh baseline must never trip this."""
    assert bench.budget_reach(1.5, 1.0) == pytest.approx(1.5)
    assert bench.budget_reach(1.5, 1.0) < bench.BUDGET_SLACK


def test_the_three_that_were_found_all_trip_it():
    """The figures this was found by, from build/device/device-bench-337run5."""
    for measured, budget_ms, expected in ((3.3654, 132.4005, 39.3),
                                          (4.6841, 120.4141, 25.7),
                                          (0.4104, 7.1391, 17.4)):
        ratio = bench.budget_reach(budget_ms, measured)
        assert ratio == pytest.approx(expected, rel=1e-2)
        assert ratio > bench.BUDGET_SLACK


def test_a_case_merely_fast_in_one_run_is_not_named():
    """Why the threshold is 6 and not 4.

    `sdf_stamp_bricks` read 0.1231 ms against a 0.7076 ms budget in one run and
    0.4717 in the next on a nearby commit — 5.75x and 1.50x for a budget that
    describes it perfectly well. A threshold under 6 would name it.
    """
    assert bench.budget_reach(0.7076, 0.1231) > 4.0     # a 4x threshold names it
    assert bench.budget_reach(0.7076, 0.1231) < bench.BUDGET_SLACK  # 6x does not


def test_a_zero_measurement_reports_no_ratio():
    """Absent rather than infinite, so it cannot be counted slack."""
    assert bench.budget_reach(1.0, 0.0) is None
    assert bench.budget_reach(1.0, -1.0) is None


def test_it_must_compare_against_the_RUN_not_the_baseline_s_own_figure():
    """The one refactor that would silently disable this.

    A baseline's `budgetMs` and `measuredMs` are written from the same run, so a
    baseline-internal ratio is ~1.5 for every entry in the file and detects
    nothing. It is the drift between the budget and what the engine does TODAY
    that this exists to see.
    """
    baseline_internal = bench.budget_reach(132.4005, 88.2670)   # both stale, together
    assert baseline_internal < bench.BUDGET_SLACK               # detects nothing
    against_the_run = bench.budget_reach(132.4005, 3.3654)      # budget vs today
    assert against_the_run > bench.BUDGET_SLACK                 # detects it

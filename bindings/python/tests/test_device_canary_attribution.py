"""Attributing a device run's drift to the cases that actually paid for it.

A run-level canary spread says the machine moved. It does not say whether
anything was MEASURED while it had moved, and those are different facts: a
bundle that sits at x1.07 for four minutes and spikes on its last sample
reports the same headline as one that degraded throughout, while only one of
them puts a budgeted case on the bad end of it.

v0.49.0's gate reported `CONDITIONS CHANGED x1.59` on both of its runs, which
read as "this run is untrustworthy" and was hiding the useful half: 54 of 59
cases were measured inside tolerance and five specific ones were not.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

check = pytest.importorskip("check_device_bench")

MEASURE = "com.cyberdyne.claycore.devicemeasure"
GALLERY = "com.cyberdyne.claycore.devicegallery"


def run_record(canary, cases):
    return {"deviceModel": "iPad15,5", "osVersion": "26.5.2",
            "canary": canary, "cases": cases}


def sample(ms, at, bundle=MEASURE):
    return {"ms": ms, "atMs": at, "bundle": bundle, "thermalState": "nominal"}


def case(name, at, bundle=MEASURE):
    return {"name": name, "startedAtMs": at, "bundle": bundle}


# The shape actually recorded on 2026-08-24: settled at ~131, a four-minute
# plateau at ~140, and one terminal sample at 208.
PLATEAU_THEN_SPIKE = [
    sample(132.34, 0), sample(131.15, 0), sample(131.97, 0),
    sample(130.79, 1400), sample(140.26, 124300), sample(140.28, 177600),
    sample(140.64, 231300), sample(208.24, 243400),
]


def test_a_case_at_the_spike_is_attributed_and_an_early_one_is_not():
    record = run_record(PLATEAU_THEN_SPIKE,
                        [case("stroke_build", 0), case("armature_edit", 243400)])
    early = check.canary_factor(record, record["cases"][0])
    late = check.canary_factor(record, record["cases"][1])
    assert early == pytest.approx(1.0, abs=0.05), early
    assert late > check.CANARY_DRIFT_TOLERANCE, late
    assert late == pytest.approx(1.58, abs=0.02), late


def test_only_the_cases_at_the_spike_are_named():
    """The whole point: 54 fine, 5 not — not one verdict over all 59."""
    cases = [case(f"early_{i}", i * 1000) for i in range(10)]
    cases += [case(f"late_{i}", 243400) for i in range(3)]
    drifted = check.cases_measured_under_drift(run_record(PLATEAU_THEN_SPIKE, cases))
    assert [n for n, _ in drifted] == ["late_0", "late_1", "late_2"], drifted
    assert all(f > check.CANARY_DRIFT_TOLERANCE for _, f in drifted)


def test_the_settled_baseline_is_a_median_not_the_luckiest_sample():
    """One quick sample at 0.0s must not become the denominator for the run."""
    canary = [sample(60.0, 0), sample(130.0, 0), sample(131.0, 0), sample(132.0, 1000)]
    base = check.canary_baselines(run_record(canary, []))[MEASURE]
    assert base == pytest.approx(130.5, abs=1.0), (
        f"a single fast sample moved the settled baseline to {base}")


def test_offsets_are_never_compared_across_bundles():
    """Each bundle is a process timing from its own start, so a gallery sample
    at 5s must not answer for a measure case at 5s."""
    canary = [sample(130.0, 0), sample(131.0, 1000),
              sample(400.0, 5000, bundle=GALLERY), sample(401.0, 0, bundle=GALLERY)]
    record = run_record(canary, [case("measured_at_5s", 5000)])
    factor = check.canary_factor(record, record["cases"][0])
    assert factor == pytest.approx(1.0, abs=0.05), (
        f"a sample from another bundle answered for this case: x{factor}")


def test_a_case_with_no_bundle_is_not_guessed_at():
    """Records predating the bundle stamp cannot be attributed, and inventing
    an attribution is worse than declining one."""
    record = run_record(PLATEAU_THEN_SPIKE, [{"name": "old", "startedAtMs": 243400}])
    assert check.canary_factor(record, record["cases"][0]) is None
    assert check.cases_measured_under_drift(record) == []


def test_a_steady_run_names_nobody():
    canary = [sample(130.0, 0), sample(131.0, 60000), sample(132.0, 120000)]
    cases = [case("a", 0), case("b", 60000), case("c", 120000)]
    assert check.cases_measured_under_drift(run_record(canary, cases)) == []

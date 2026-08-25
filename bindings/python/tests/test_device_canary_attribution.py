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


# -- normalising a case by its own bracket (#297) ----------------------------
#
# Attribution above says WHICH cases were measured on a moved machine. It does
# not stop the gate failing them, and on 2026-08-25 that reported `sdf_move` as
# a 1.6x regression on code that measures 1.05x off-device: the case ran at
# 135 s between canary samples at 122 s (x1.07) and 147 s (x1.50), because two
# pooled cases in that gap take the device from cold to throttled in 13 s.
# Whichever sample happened to be nearer decided the verdict.
#
# A bracketed case carries the canary sampled immediately before and after
# itself, so nothing is interpolated.

def bracketed(name, at, before, after, p95, bundle=MEASURE):
    c = case(name, at, bundle)
    c["canaryBeforeMs"] = before
    c["canaryAfterMs"] = after
    c["measurements"] = [{"stamps": 1000, "p50Ms": p95 * 0.9, "p95Ms": p95, "samples": 40}]
    return c


SETTLED = [sample(130.0, 0), sample(131.0, 0), sample(130.5, 0)]


def test_a_case_measured_on_a_throttled_machine_is_divided_by_its_own_slowdown():
    # 0.130 ms measured while the machine ran at x1.50 is 0.087 ms of engine.
    run = run_record(SETTLED, [bracketed("sdf_move", 135_000, 195.0, 195.0, 0.1299)])
    norm, factor = check.normalised_p95(run, run["cases"][0])
    assert factor == pytest.approx(1.5, abs=0.02)
    assert norm == pytest.approx(0.0866, abs=0.002)


def test_the_same_engine_reads_the_same_at_both_ends_of_a_thermal_window():
    # THE case this exists for. One verb, unchanged, measured cold in one run
    # and throttled in another: raw they differ by 1.54x, which is past the
    # 1.4x tolerance and is what #297 reported. Normalised they agree.
    cold = run_record(SETTLED, [bracketed("sdf_move", 225_000, 139.0, 139.0, 0.0845)])
    hot = run_record(SETTLED, [bracketed("sdf_move", 135_000, 195.0, 201.0, 0.1299)])
    raw_ratio = check.worst_p95(hot["cases"][0]) / check.worst_p95(cold["cases"][0])
    assert raw_ratio > 1.4, "the raw comparison must be the one that fails"

    cold_n, _ = check.normalised_p95(cold, cold["cases"][0])
    hot_n, _ = check.normalised_p95(hot, hot["cases"][0])
    assert hot_n / cold_n == pytest.approx(1.0, abs=0.15)
    assert hot_n / cold_n < 1.4, "normalised, the same engine must not read as a regression"


def test_a_real_regression_still_fails_after_normalising():
    # Normalisation must not become a way to launder a genuine slowdown: the
    # same machine factor on both sides leaves the ratio untouched.
    cold = run_record(SETTLED, [bracketed("sdf_move", 225_000, 139.0, 139.0, 0.0845)])
    slow = run_record(SETTLED, [bracketed("sdf_move", 225_000, 139.0, 139.0, 0.1690)])
    cold_n, _ = check.normalised_p95(cold, cold["cases"][0])
    slow_n, _ = check.normalised_p95(slow, slow["cases"][0])
    assert slow_n / cold_n == pytest.approx(2.0, abs=0.01)


def test_an_unbracketed_case_is_compared_raw_rather_than_guessed_at():
    # Records written before the bracket existed, and the gallery bundle, which
    # sets no per-case context at all. Falling back to the nearest periodic
    # sample is what produced the false failure; raw is at least honest.
    run = run_record(SETTLED, [bracketed("sdf_move", 135_000, 0, 0, 0.1299)])
    norm, factor = check.normalised_p95(run, run["cases"][0])
    assert factor is None
    assert norm == pytest.approx(0.1299)


def test_a_bracket_without_a_bundle_baseline_is_not_normalised():
    # Same rule canary_factor already follows: an offset that cannot be lined
    # up against a settled reading is not guessed at.
    run = run_record([], [bracketed("sdf_move", 135_000, 195.0, 195.0, 0.1299)])
    assert check.bracket_factor(run, run["cases"][0]) is None


def test_the_bracket_is_the_mean_of_both_ends_not_whichever_is_nearer():
    # A case that starts cold and ends hot paid something in between. Taking
    # either end alone reproduces the interpolation problem in miniature.
    run = run_record(SETTLED, [bracketed("sdf_move", 135_000, 130.5, 195.0, 0.1299)])
    factor = check.bracket_factor(run, run["cases"][0])
    assert factor == pytest.approx(((130.5 + 195.0) / 2) / 130.5, abs=0.01)


def test_drift_attribution_reports_the_factor_the_gate_actually_divides_by():
    # The report and the gate must not disagree. Before this, attribution used
    # the nearest periodic sample while the gate divided by the bracket, so a
    # run printed "every case was measured with the canary inside tolerance"
    # while applying a 1.43x correction to sdf_flatten.
    run = run_record(
        SETTLED + [sample(133.0, 130_000), sample(190.0, 200_000)],
        [bracketed("sdf_flatten", 131_000, 180.0, 194.0, 6.8)])
    case = run["cases"][0]
    assert check.canary_factor(run, case) == pytest.approx(
        check.bracket_factor(run, case)), "attribution must follow the bracket"
    assert [n for n, _ in check.cases_measured_under_drift(run)] == ["sdf_flatten"]


def test_attribution_still_falls_back_to_the_nearest_sample_when_unbracketed():
    run = run_record(SETTLED + [sample(190.0, 130_000)],
                     [bracketed("legacy", 131_000, 0, 0, 6.8)])
    assert check.canary_factor(run, run["cases"][0]) == pytest.approx(190.0 / 130.5, abs=0.02)

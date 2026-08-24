#!/usr/bin/env python3
"""Gate a device run against the committed baseline and the declared budgets.

Three independent failures, because they mean different things:

  REGRESSION  this build got slower than the baseline by more than tolerance.
  BUDGET      this case is slower than what its interaction class allows,
              whether or not it regressed. A baseline can enshrine something
              already too slow; the budget is what says so.
  GROWTH      per-stamp cost grows super-linearly in document size. A level
              that passes today at 1000 stamps can still be the wrong SHAPE,
              and shape is what bites at 10000.

A run is refused outright — not compared — when it came from different
hardware or was thermally throttled. Those are different experiments, and
scoring them against the baseline produces a number that means nothing.

    tools/check_device_bench.py <run.json> [--baseline <baseline.json>]
    tools/check_device_bench.py <run.json> --update    # write the baseline
"""

import argparse
import json
import pathlib
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_BASELINE = ROOT / "tests" / "device" / "baseline.json"

# How much slower than baseline is a regression. Loose enough that ordinary
# device jitter does not fail a release, tight enough to catch a real change.
DEFAULT_TOLERANCE = 1.40

# A 120 Hz ProMotion frame is 8.33 ms and the HOST still has to draw in it, so
# the engine's share of an interactive step is half. Cases over this are not
# failures by themselves — the budget in the baseline decides that — but they
# are reported, because an `interactive` verb that cannot fit a frame is a
# product fact rather than a rounding error.
INTERACTIVE_FRAME_SHARE_MS = 8.33 / 2

# Above this exponent, cost is growing faster than the document. 1.0 is linear;
# the margin absorbs fit noise from a two-point log-log slope.
MAX_GROWTH_EXPONENT = 1.25

# Below this, a difference is measurement noise rather than a result, and a
# ratio computed from it means nothing.
#
# This is not a convenience. The session cases record ONE sample per stroke —
# they measure a session, not a distribution — and several verbs run in single
# microseconds, so `voxel_erase` moved 0.004 ms to 0.009 ms between two runs of
# identical code and tripped both BUDGET and REGRESSION at 2.03x. A gate that
# fails on five microseconds teaches people to ignore it.
#
# A regression must therefore be BOTH relatively large and absolutely
# meaningful. Nothing near the interactive budget is anywhere near this floor:
# the cases that matter are milliseconds, and 0.05 ms is a hundredth of a
# 120 Hz frame.
NOISE_FLOOR_MS = 0.05

# How far the canary may spread across a run before the run is called one where
# position mattered.
#
# DELIBERATELY LOOSE, and deliberately not derived from anything yet. The point
# of the first release of this check is to COLLECT what a steady device looks
# like: a handful of runs will say, and the number can be set from that. Setting
# it tight now would report drift on every run, which is how a check gets
# ignored. What it is defending against is large — moving one bundle from third
# to first moved a case 2.7x — so a loose threshold still catches it.
CANARY_DRIFT_TOLERANCE = 1.30


# How long a bundle is given to settle before its canary readings count as the
# level it started from. The harness takes three samples at 0.0s, before the
# process has touched anything, and a run's first seconds are the only part
# guaranteed not to be paying for an earlier case.
CANARY_SETTLE_MS = 10_000


def _by_bundle(run: dict) -> dict[str, list[dict]]:
    """Canary samples grouped by the process that took them.

    atMs is a WITHIN-bundle offset — a run spans three processes and each times
    from its own start — so every comparison below is inside one group.
    """
    grouped: dict[str, list[dict]] = {}
    for sample in run.get("canary", []):
        if sample.get("ms", 0) > 0:
            grouped.setdefault(sample.get("bundle", "unknown"), []).append(sample)
    return grouped


def canary_baselines(run: dict) -> dict[str, float]:
    """The settled reading each bundle started from.

    The MEDIAN of the settling window rather than the single lowest sample: the
    harness takes three samples at 0.0s and one of them reading quick should not
    become the denominator every later case is judged against. That is the
    difference between "this case ran on a machine 1.6x slower than the one the
    run started on" and "one early sample was lucky".
    """
    settled = {}
    for bundle, samples in _by_bundle(run).items():
        early = [s["ms"] for s in samples if s.get("atMs", 0) <= CANARY_SETTLE_MS]
        pool = early or [s["ms"] for s in samples]
        if pool:
            settled[bundle] = statistics.median(pool)
    return settled


def canary_factor(run: dict, case: dict) -> float | None:
    """How much slower the machine was when THIS case ran, or None.

    This is the question the run-level spread cannot answer. A bundle that
    plateaus at x1.07 for four minutes and spikes at the very end reports the
    same headline as one that degraded steadily throughout, and only one of
    them puts a measured case on the bad end of it.

    None when the record cannot say: a case recorded before collect_device_bench
    stamped cases with their bundle has an offset that no canary sample can be
    lined up against, and guessing which process it came from would invent the
    attribution this function exists to make honest.
    """
    bundle = case.get("bundle")
    samples = _by_bundle(run).get(bundle) if bundle else None
    base = canary_baselines(run).get(bundle) if bundle else None
    if not samples or not base:
        return None
    started = case.get("startedAtMs", 0)
    nearest = min(samples, key=lambda s: abs(s.get("atMs", 0) - started))
    return nearest["ms"] / base


def cases_measured_under_drift(run: dict) -> list[tuple[str, float]]:
    """Every case whose own canary factor is past tolerance, worst first."""
    drifted = []
    for case in run.get("cases", []):
        factor = canary_factor(run, case)
        if factor is not None and factor > CANARY_DRIFT_TOLERANCE:
            drifted.append((case["name"], factor))
    return sorted(drifted, key=lambda pair: pair[1], reverse=True)


def canary_drift(run: dict) -> tuple[float, dict, dict] | None:
    """Widest ratio between two canary samples, with the samples themselves.

    The canary touches nothing under test, so a spread here is the machine
    moving rather than the engine changing. It answers what thermalState
    cannot: that signal has four coarse levels and an M3 iPad throttles well
    inside `nominal` — the two runs that differed 2.7x on sdf_stamp_cpu both
    read nominal at every point.

    Samples are compared WITHIN a bundle. A run spans three processes and each
    times from its own start, so a ratio across bundles would be comparing two
    clocks as if they were one.
    """
    worst = None
    by_bundle: dict[str, list[dict]] = {}
    for sample in run.get("canary", []):
        by_bundle.setdefault(sample.get("bundle", "unknown"), []).append(sample)
    for samples in by_bundle.values():
        usable = [s for s in samples if s.get("ms", 0) > 0]
        if len(usable) < 2:
            continue
        lo = min(usable, key=lambda s: s["ms"])
        hi = max(usable, key=lambda s: s["ms"])
        ratio = hi["ms"] / lo["ms"]
        if worst is None or ratio > worst[0]:
            worst = (ratio, lo, hi)
    return worst


def worst_p95(case: dict) -> float:
    return max((m["p95Ms"] for m in case.get("measurements", [])), default=float("nan"))


def case_repeats(case: dict) -> int:
    """Most repeats any of this case's points needed."""
    return max((m.get("repeats", 1) for m in case.get("measurements", [])), default=1)


def single_observation(case: dict) -> bool:
    """True when every point of this case is ONE timing, not a percentile.

    The gallery sessions time each stroke of a progressive sculpt once, so
    their reported p95 is the slowest single stroke rather than a percentile
    over repeats — and a stroke cannot be repeated without changing the sculpt
    it is building. Those cases cannot be stabilised the way a benchmark point
    can, so the gate says which kind it is failing on rather than implying a
    measurement that was never taken.
    """
    ms = case.get("measurements", [])
    return bool(ms) and all(m.get("samples", 0) <= 1 for m in ms)


def worst_spread(case: dict) -> float:
    """Widest per-pass p95 spread over this case's measurement points.

    Zero for a case measured in one pass, which is every case whose points
    earned enough samples for an honest percentile. Non-zero says the harness
    had to repeat the point, and how far the answer moved when it did.
    """
    return max((m.get("p95SpreadMs", 0.0) for m in case.get("measurements", [])),
               default=0.0)


def load(path: pathlib.Path) -> dict:
    if not path.exists():
        raise SystemExit(f"device-bench: no such file: {path}")
    return json.loads(path.read_text())


def write_baseline(run: dict, path: pathlib.Path, tolerance: float) -> None:
    """Seed a baseline from a run, budgeting each case off what it measured.

    The budgets are deliberately DERIVED and then meant to be edited by hand:
    a generated number records what the engine does, and a reviewed one
    records what it must do. Committing this file is where that decision gets
    made, which is why it is its own step.
    """
    budgets = {}
    for case in run["cases"]:
        measured = worst_p95(case)
        budgets[case["name"]] = {
            "class": case["budgetClass"],
            # headroom over what it measures today
            "budgetMs": round(measured * 1.5, 4),
            "measuredMs": round(measured, 4),
        }
    baseline = {
        "deviceModel": run["deviceModel"],
        "osVersion": run["osVersion"],
        "abiVersion": run["abiVersion"],
        "claycoreCommit": run.get("claycoreCommit"),
        "tolerance": tolerance,
        "budgets": budgets,
        "cases": run["cases"],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(baseline, indent=2, sort_keys=True) + "\n")
    print(f"device-bench: wrote baseline for {len(budgets)} case(s) -> {path}")
    over = [n for n, b in budgets.items()
            if b["class"] == "interactive" and b["measuredMs"] > INTERACTIVE_FRAME_SHARE_MS]
    if over:
        print("device-bench: interactive cases that do NOT fit a 120 Hz frame share "
              f"({INTERACTIVE_FRAME_SHARE_MS:.2f} ms): {', '.join(sorted(over))}")


def main() -> int:
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("run")
    parser.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    parser.add_argument("--update", action="store_true",
                        help="write the baseline from this run instead of checking it")
    parser.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE)
    args = parser.parse_args()

    run = load(pathlib.Path(args.run))
    baseline_path = pathlib.Path(args.baseline)

    # A throttled run is not a slower result. Refuse it in both directions:
    # seeding a baseline from one bakes the throttling into the reference.
    if not run.get("valid", False):
        print(f"device-bench: REFUSED — run is invalid (thermal "
              f"{run.get('thermalStateStart')} -> {run.get('thermalStateEnd')}). "
              f"Let the device cool and run again.", file=sys.stderr)
        return 1

    # `performance-budgets`: a simulator runs the host's cores with the host's
    # memory and no thermal ceiling, so it cannot answer a question about a
    # tablet. Refused in BOTH directions — seeding a baseline from one would
    # bake a desktop number in as the device's requirement.
    platform = run.get("platform", "device")
    if platform != "device":
        print(f"device-bench: REFUSED — this run is from a {platform}, not a "
              f"device. Its renders are useful; its timings are not comparable "
              f"to a device baseline and cannot seed one.", file=sys.stderr)
        return 1

    if args.update:
        write_baseline(run, baseline_path, args.tolerance)
        return 0

    baseline = load(baseline_path)

    # Hardware identity, before any number is compared.
    for field in ("deviceModel", "osVersion"):
        if run.get(field) != baseline.get(field):
            print(f"device-bench: REFUSED — {field} is {run.get(field)!r}, baseline "
                  f"is {baseline.get(field)!r}. A comparison across different "
                  f"hardware is invalid, not merely noisy.", file=sys.stderr)
            return 1

    tolerance = baseline.get("tolerance", args.tolerance)
    budgets = baseline.get("budgets", {})
    base_cases = {c["name"]: c for c in baseline.get("cases", [])}
    failures, notes = [], []

    for case in run["cases"]:
        name = case["name"]
        measured = worst_p95(case)

        # BUDGET — every case must declare one. An unbudgeted latency number
        # is a measurement, and this tool exists to gate.
        budget = budgets.get(name)
        if budget is None:
            failures.append(f"{name}: no declared budget in the baseline")
        else:
            over = measured - budget["budgetMs"]
            if measured > budget["budgetMs"] and over > NOISE_FLOOR_MS:
                failures.append(
                    f"{name}: BUDGET {measured:.3f} ms p95 exceeds "
                    f"{budget['budgetMs']:.3f} ms ({budget['class']})")
            elif (budget["class"] == "interactive"
                  and measured > INTERACTIVE_FRAME_SHARE_MS):
                notes.append(f"{name}: {measured:.3f} ms p95 is inside its budget but "
                             f"outside a 120 Hz frame share "
                             f"({INTERACTIVE_FRAME_SHARE_MS:.2f} ms)")

        # REGRESSION — against what this build used to do.
        base = base_cases.get(name)
        if base is None:
            notes.append(f"{name}: new case, no baseline to compare against")
        else:
            base_p95 = worst_p95(base)
            grew = measured - base_p95
            if base_p95 > 0 and measured > base_p95 * tolerance and grew > NOISE_FLOOR_MS:
                # Say what this case's own repeats measured, when it had any.
                # A regression smaller than the spread the harness just saw on
                # THIS run is not evidence of anything, and reporting the
                # failure without that number is how a scheduling hiccup got
                # investigated as a 1.6x consolidation regression for a day.
                spread = worst_spread(case)
                caveat = ""
                # THIS case's conditions, not the run's widest spread. The
                # run-level number said "position may account for some of this"
                # on every case in a run whose last sample spiked, including the
                # ones measured in the first ten seconds — which is a caveat
                # attached to the wrong cases and absent from the right ones.
                factor = canary_factor(run, case)
                if factor is not None and factor > CANARY_DRIFT_TOLERANCE:
                    caveat = (f"; NOTE the canary read x{factor:.2f} of its settled "
                              f"value when this case ran, so the machine was "
                              f"slower here than where the baseline was taken")
                if single_observation(case):
                    caveat = ("; NOTE every point of this case is a SINGLE "
                              "timing, so this is one observation against "
                              "another, not a measured regression")
                elif spread > 0 and grew < spread:
                    caveat = (f"; NOTE the {case_repeats(case)} repeats of this "
                              f"case spread {spread:.3f} ms, wider than the "
                              f"{grew:.3f} ms growth — treat as unproven")
                failures.append(
                    f"{name}: REGRESSION {measured:.3f} ms p95 vs baseline "
                    f"{base_p95:.3f} ms (x{measured / base_p95:.2f}, "
                    f"tolerance x{tolerance}, +{grew:.3f} ms){caveat}")

        # GROWTH — the shape, not the level.
        growth = case.get("growthExponent")
        if growth is not None and growth > MAX_GROWTH_EXPONENT:
            sizes = ", ".join(f"{m['stamps']}:{m['p95Ms']:.3f}ms"
                              for m in case["measurements"])
            failures.append(
                f"{name}: GROWTH cost scales as N^{growth:.2f} in document size "
                f"(over N^{MAX_GROWTH_EXPONENT}) — {sizes}")

    for name in budgets:
        if name not in {c["name"] for c in run["cases"]}:
            failures.append(f"{name}: budgeted in the baseline but absent from this run")

    print(f"device-bench: {len(run['cases'])} case(s) on {run['deviceModel']}, "
          f"{run['osVersion']}")

    # Did the machine underneath the run change while it ran? Printed every
    # time, not only on failure: knowing a case's conditions BEFORE it fails is
    # the whole point, and a number that only appears in a postmortem teaches
    # nobody what a steady device looks like.
    drift = canary_drift(run)
    if drift is None:
        print("  canary: no samples — this run cannot say whether position mattered")
    else:
        ratio, lo, hi = drift
        verdict = ("CONDITIONS CHANGED while this run ran"
                   if ratio > CANARY_DRIFT_TOLERANCE else "steady")
        print(f"  canary: {verdict} — x{ratio:.2f} across the run "
              f"({lo['ms']:.2f} ms at {lo['atMs'] / 1000:.0f}s -> "
              f"{hi['ms']:.2f} ms at {hi['atMs'] / 1000:.0f}s, "
              f"tolerance x{CANARY_DRIFT_TOLERANCE})")
        if ratio > CANARY_DRIFT_TOLERANCE:
            states = {lo.get("thermalState"), hi.get("thermalState")}
            if states == {"nominal"}:
                print("    ...and thermalState read `nominal` at both, which is "
                      "the case this check exists for: the OS signal has four "
                      "levels and the device throttles inside the first.")

    # WHICH CASES actually paid for it. A run-level ratio says the machine
    # moved; it does not say whether anything was measured while it had. These
    # are the numbers in this run that are not comparable with a baseline taken
    # on a settled device, named rather than left for a reader to infer from
    # offsets.
    drifted = cases_measured_under_drift(run)
    if drifted:
        print(f"  {len(drifted)} case(s) measured while the canary was past "
              f"x{CANARY_DRIFT_TOLERANCE} of its settled value:")
        for name, factor in drifted:
            print(f"    {name}: x{factor:.2f}")
        print("    These sit at the end of their bundle and pay for every case "
              "ahead of them. Their budgets still hold, so this is a note on "
              "what the numbers mean, not a failure.")
    elif any(c.get("bundle") for c in run.get("cases", [])):
        print("  every case was measured with the canary inside tolerance")
    else:
        print("  cases carry no bundle, so this run cannot attribute drift to "
              "them — re-collect with tools/collect_device_bench.py")

    # Which cases could not be measured in one pass, and how far they moved
    # when repeated. Printed every run rather than only on failure: the point
    # is to know a case's noise BEFORE it fails, not to explain it afterwards.
    repeated = sorted(((case_repeats(c), worst_spread(c), c["name"])
                       for c in run["cases"] if case_repeats(c) > 1),
                      key=lambda t: -t[1])
    if repeated:
        print(f"  {len(repeated)} case(s) needed repeats for an honest p95:")
        for reps, spread, name in repeated[:5]:
            print(f"    {name}: median of {reps} passes, spread {spread:.3f} ms")
        if len(repeated) > 5:
            print(f"    ... and {len(repeated) - 5} more")

    for note in notes:
        print(f"  note: {note}")
    for f in failures:
        print(f"device-bench: FAIL {f}", file=sys.stderr)
    if failures:
        return 1

    # Record that the gate passed, and for WHICH code. The release checklist
    # reads this: CI runners have no attached iPad, so the only way a release
    # can require the device gate is to require evidence that someone ran it
    # against the engine being released. See tools/release_check.py.
    stamp = {
        "passed": True,
        "claycoreCommit": run.get("claycoreCommit"),
        "deviceModel": run["deviceModel"],
        "osVersion": run["osVersion"],
        "abiVersion": run["abiVersion"],
        "caseCount": len(run["cases"]),
        # Recorded so the release check can refuse a stamp whose commit does
        # not describe the code that ran.
        "treeDirty": run.get("treeDirty", False),
    }
    stamp_path = baseline_path.parent / "last-gate.json"
    stamp_path.write_text(json.dumps(stamp, indent=2, sort_keys=True) + "\n")
    print(f"device-bench: OK (recorded in {stamp_path.name})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

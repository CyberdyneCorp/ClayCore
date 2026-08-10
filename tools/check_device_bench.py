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


def worst_p95(case: dict) -> float:
    return max((m["p95Ms"] for m in case.get("measurements", [])), default=float("nan"))


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
            if measured > budget["budgetMs"]:
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
            if base_p95 > 0 and measured > base_p95 * tolerance:
                failures.append(
                    f"{name}: REGRESSION {measured:.3f} ms p95 vs baseline "
                    f"{base_p95:.3f} ms (x{measured / base_p95:.2f}, "
                    f"tolerance x{tolerance})")

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
    for note in notes:
        print(f"  note: {note}")
    for f in failures:
        print(f"device-bench: FAIL {f}", file=sys.stderr)
    if not failures:
        print("device-bench: OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

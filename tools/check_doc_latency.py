#!/usr/bin/env python3
"""docs/09's latency table quotes the baseline, and CI checks that it does.

The table is what a host reads to decide what to put behind progress UI;
`tests/device/baseline.json` is what the release gate compares against. They
were copied apart by hand and drifted in BOTH directions (issue #273):
`sdf_consolidate` read 2.3x pessimistic, `mask_extrude` 1.4x optimistic, and
`sdf_relax`'s figure matched no point on the axis its column header names.
A host sizing an affordance from an optimistic row under-budgets the one
operation in the list that takes four seconds.

The numbers already exist in a machine-readable file and every row already
names its device case, so nothing here needs to be typed. This is option (2)
from the issue -- a checker rather than generation -- because the table carries
prose, footnotes and ordering a generator would flatten.

WHAT A ROW MUST QUOTE is the PER-APPLICATION cost, which is the column header's
own claim. A batched case records what K applications cost together, so the
figure to check against is `measuredMs / batch`. Without that this check would
demand the table publish a 128-dab drag as though it were one dab.

    tools/check_doc_latency.py [docs/09-brush-latency-and-coverage.md]
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "09-brush-latency-and-coverage.md"
BASELINE = ROOT / "tests" / "device" / "baseline.json"

# How far a quoted figure may sit from the baseline's, per bundle.
#
# NOT one number, because per-case reliability is not uniform and a check that
# pretended otherwise would either nag about the measure bundle or wave the
# gallery through. The `devicemeasure` cases reproduce to under 0.5% across
# runs; the gallery cases do not -- `move_drags` moved 0.149 -> 0.434 ms
# between two runs of identical code, and `stroke_build` spans 16.6-21.9 ms
# across four. So the gallery is allowed the spread it actually has, and is
# quoted to fewer figures for the same reason.
TOLERANCE = {"devicemeasure": 1.30, "devicegallery": 2.10}
DEFAULT_TOLERANCE = 1.30

# A row whose baseline figure is deliberately NOT what the engine does today,
# with the reason. Exempt from the ratio check and printed, never skipped in
# silence — the same rule check_device_coverage.py applies to its exemptions.
#
# A held baseline is the one case where quoting the baseline would be WRONG: the
# table tells a host what to expect, and a host expects what the engine does.
HELD = {
    "sdf_move": (
        "the baseline deliberately holds the pre-regression figure (0.0791 ms) "
        "as the reference a fix must return to; the engine measures ~0.116 ms "
        "today, which is what this row quotes. Issue #358."),
}

# A table row: ... | `case_name` | 1.234 | class |
ROW = re.compile(
    r"^\|.*\|\s*`([a-z_0-9]+)`\s*\|\s*\**\s*([0-9]+(?:\.[0-9]+)?)\s*\**\s*[^|]*\|"
    r"\s*(interactive|gesture|operation)\s*\|")


def per_application(baseline: dict, name: str) -> tuple[float, str] | None:
    """What one application of this verb costs, and the bundle that timed it."""
    budget = baseline.get("budgets", {}).get(name)
    if budget is None:
        return None
    case = next((c for c in baseline.get("cases", []) if c["name"] == name), None)
    batch = 1
    bundle = ""
    if case is not None:
        bundle = (case.get("bundle") or "").split(".")[-1]
        batch = max((m.get("batch", 1) for m in case.get("measurements", [])), default=1)
    measured = budget.get("measuredMs", 0.0)
    if measured <= 0 or batch <= 0:
        return None
    return measured / batch, bundle


def main() -> int:
    doc_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DOC
    baseline = json.loads(BASELINE.read_text())
    failures, held, checked = [], [], 0

    for lineno, line in enumerate(doc_path.read_text().splitlines(), 1):
        m = ROW.match(line)
        if not m:
            continue
        name, quoted = m.group(1), float(m.group(2))
        found = per_application(baseline, name)
        if found is None:
            # A row naming a case the baseline does not budget. Not this
            # check's business -- check_device_coverage.py owns "is it
            # covered" -- but worth saying rather than skipping in silence.
            failures.append(f"{doc_path.name}:{lineno}: `{name}` is quoted at "
                            f"{quoted:g} ms and has no budget in the baseline")
            continue
        expected, bundle = found
        if name in HELD:
            held.append((name, quoted, expected, HELD[name]))
            continue
        checked += 1
        tol = TOLERANCE.get(bundle, DEFAULT_TOLERANCE)
        if quoted <= 0:
            failures.append(f"{doc_path.name}:{lineno}: `{name}` quotes {quoted:g}")
            continue
        ratio = max(quoted / expected, expected / quoted)
        if ratio > tol:
            direction = "optimistic" if quoted < expected else "pessimistic"
            failures.append(
                f"{doc_path.name}:{lineno}: `{name}` quotes {quoted:g} ms, the "
                f"baseline says {expected:.4g} ms — {ratio:.2f}x {direction}, "
                f"over the {tol:.2f}x allowed for {bundle or 'this case'}")

    print(f"doc-latency: {checked} quoted figure(s) checked against "
          f"{BASELINE.relative_to(ROOT)}")
    for name, quoted, expected, reason in held:
        print(f"  held: {name} quotes {quoted:g} ms against a baseline of "
              f"{expected:.4g} ms — {reason}")
    missing = sorted(set(HELD) - {h[0] for h in held})
    for name in missing:
        # An exemption for a row that no longer exists is an exemption nobody
        # will notice has stopped applying.
        failures.append(f"`{name}` is held but appears in no row of "
                        f"{doc_path.name}; drop the exemption")
    for f in failures:
        print(f"doc-latency: FAIL {f}", file=sys.stderr)
    if failures:
        print("doc-latency: re-derive the table from the run that produced the "
              "baseline; the figure to quote is measuredMs / batch.",
              file=sys.stderr)
        return 1
    print("doc-latency: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

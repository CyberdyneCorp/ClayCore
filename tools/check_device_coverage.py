#!/usr/bin/env python3
"""Every brush and sculpt verb has a device case, or a recorded exemption.

The same idea as CAPABILITY_EXAMPLES in examples/run_all.py: an uncovered
capability should be an error, and an exemption should be a decision on the
record. A latency suite that quietly skips half the brushes reads exactly like
one that covers them.

Checks three things against each other — the ABI, the harness's table, and the
run that actually happened:

  1. Every verb-shaped entry point in bindings/c/clay.h appears in the table.
     Catches a verb added to the engine with no case.
  2. Every non-exempt table entry produced a measurement in the run.
     Catches a table entry naming a case that never ran.
  3. Every exemption carries a reason.

    tools/check_device_coverage.py <device-bench.json>
"""

import json
import pathlib
import re
import sys

HEADER = pathlib.Path(__file__).resolve().parent.parent / "bindings" / "c" / "clay.h"

# What counts as a verb a sculptor drives. Deliberately a NAMED set of
# patterns rather than "every clay_ function": the ABI has ~600 entry points
# and most are accessors, lifetime or I/O. Adding a pattern here is how a new
# family of verbs becomes covered-or-exempt rather than invisible.
VERB_PATTERNS = [
    r"clay_voxel_sculpt_\w+",
    r"clay_voxel_(set|erase|paint)_brush",
    r"clay_voxel_apply_stroke",
    r"clay_voxel_mask_extrude",
    r"clay_stroke_resolve",
    r"clay_mask_paint",
    r"clay_document_mask_extrude",
    r"clay_item_volume_(relax|flatten|move_topological)",
    r"clay_layer_(move_surface|consolidate)",
    r"clay_cut_create",
]

# Verbs the engine has no C entry point for, so pattern-matching clay.h cannot
# find them. Named here so the table is still held to covering them.
EXTRA_VERBS = {
    # The roadmap calls this a landed brush; the binding-parity table maps
    # module.snakehook to clay_item_set_curve_points, so the RESOLVER is
    # Python-only and only its output reaches this ABI. Recorded so the
    # asymmetry is visible rather than inferred from an absence.
    "snakehook",
    # The SDF stamp is a composition (create item, add to layer, evaluate)
    # rather than one entry point, but it is the verb a stroke is made of.
    "sdf_stamp",
}


def verbs_in_header() -> set[str]:
    text = HEADER.read_text()
    found = set()
    for pattern in VERB_PATTERNS:
        # finditer + group(1) rather than findall: the patterns carry inner
        # alternation groups, and findall returns a tuple per match when a
        # pattern has more than one group.
        for match in re.finditer(rf"\b({pattern})\s*\(", text):
            name = match.group(1)
            # the table drops the clay_ prefix
            found.add(name[len("clay_"):] if name.startswith("clay_") else name)
    return found | EXTRA_VERBS


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    record = json.loads(pathlib.Path(sys.argv[1]).read_text())
    table = record.get("coverage")
    if table is None:
        print("coverage: the run carries no coverage table; the harness that "
              "produced it predates the guard", file=sys.stderr)
        return 1

    by_verb = {entry["verb"]: entry for entry in table}
    measured = {case["name"] for case in record.get("cases", [])}
    failures = []

    # 1. every verb the ABI exposes is in the table
    for verb in sorted(verbs_in_header()):
        if verb not in by_verb:
            failures.append(
                f"{verb}: exposed by the ABI and named in no table entry. Add a "
                f"device case, or an exemption saying why it has none.")

    # 2. every non-exempt entry actually ran, and 3. exemptions carry a reason
    for entry in table:
        verb, case, exemption = entry["verb"], entry.get("caseName"), entry.get("exemption")
        if exemption is not None:
            if not exemption.strip():
                failures.append(f"{verb}: exempt with an empty reason")
            if case is not None:
                failures.append(f"{verb}: both exempt and measured by '{case}'")
            continue
        if not case:
            failures.append(f"{verb}: neither measured nor exempt")
        elif case not in measured:
            failures.append(
                f"{verb}: names case '{case}', which produced no measurement in "
                f"this run")

    covered = sum(1 for e in table if e.get("exemption") is None)
    exempt = len(table) - covered
    print(f"coverage: {covered} verb(s) measured, {exempt} exempt, "
          f"{len(measured)} case(s) in the run")
    for entry in table:
        if entry.get("exemption"):
            print(f"  exempt: {entry['verb']} — {entry['exemption'][:70]}...")

    for f in failures:
        print(f"coverage: FAIL {f}", file=sys.stderr)
    if not failures:
        print("coverage: OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

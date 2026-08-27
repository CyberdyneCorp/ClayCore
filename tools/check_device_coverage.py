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
    # The voxel sculpt brushes. The negative lookahead is load-bearing: the
    # sculpt-LAYER stack spells its accessors clay_voxel_sculpt_layer{,s}_*, so
    # a bare \w+ swept seven `const clay_voxel_grid*` getters — count, name,
    # cell_count, strength, visible, bytes and the stack total — in among the
    # ten brushes and demanded a latency case for each. This list is verbs a
    # sculptor drives; an O(1) read of a layer's name is not one, and the
    # docstring above says so.
    r"clay_voxel_sculpt_(?!layers?_)\w+",
    r"clay_voxel_(set|erase|paint)_brush",
    r"clay_voxel_apply_stroke",
    r"clay_voxel_mask_extrude",
    r"clay_stroke_resolve",
    r"clay_mask_paint",
    r"clay_document_mask_extrude",
    r"clay_item_volume_(relax|flatten|move_topological)",
    r"clay_layer_(move_surface|consolidate)",
    r"clay_cut_create",
    # The resolvers that turn a gesture into an item. They were missing here
    # rather than exempt, which is a different thing: `tube` and Trim Curve are
    # brushes in docs/07's table and had no case, and nothing said so, because
    # this list is what decides what "missing" means.
    r"clay_tube_create",
    r"clay_cut_polygon_from_(open_)?curve",
    # A rig edit is a drag like any other — ZBrush's ZSpheres, and the one
    # brush-shaped verb that edits topology rather than a field.
    r"clay_layer_armature_edit",
    # Displaying a voxel sculpt. Not a verb a sculptor names, but it is on the
    # frame path with the verbs and it was the whole of #86: every edit verb
    # here fit the frame with two orders of magnitude to spare while showing
    # the result cost ~130x the budget, and this list is what decides whether
    # an absence is a gap or an omission nobody noticed.
    r"clay_voxel_mesh(_chunks)?",
    r"clay_voxel_take_dirty_chunks",
    # Subdividing. A level is charged to the EDIT that crosses it (a write
    # costs 8^d cell writes for d finer levels), so it belongs with the verbs
    # rather than with the accessors that report a stack.
    #
    # The `_region` suffix is spelled out rather than left to the prefix: a
    # pattern here is matched only where it is followed by "(", so
    # `clay_voxel_add_level` does NOT cover `clay_voxel_add_level_region`. That
    # is precisely how the level stack went missing from this list once before.
    r"clay_voxel_(add|drop)_level(_region)?",
    # The fixed-topology mesh brushes. One stamp and one stroke, not sixteen
    # entries: the verb is a field of the descriptor, so the entry points are
    # what a host calls and what a latency case would drive. The raycast is a
    # pick query rather than a verb and is deliberately not matched.
    #
    # `deform` is listed because it is NOT one of those: a whole-form taper or
    # twist is O(every vertex) where a stamp is O(the vertices a falloff
    # reached), so inheriting the stamp's number would be inheriting the wrong
    # shape. It arrived after this list was written and was invisible to it —
    # the exact failure this list exists to prevent, and the one its own
    # comment names tube, Trim Curve and the level stack for.
    r"clay_mesh_sculptor_(stamp|apply_stroke|deform)",
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
    # And the same composition repeated without an invalidation between the
    # repeats, which is a different cost and a different code path -- the
    # compiled prefix is reused and the GPU tape is patched rather than
    # re-uploaded. No entry point of its own for the same reason sdf_stamp has
    # none.
    "sdf_stroke",
    # The same unbroken stroke driven through the brick cache rather than a
    # whole-lattice evaluation. A separate verb because it is a separate path:
    # this is the one that reaches the resumed refill, and sdf_stamp_incremental
    # cannot -- its reset removes the node, and a removal breaks the append
    # chain before the fast path is consulted.
    "sdf_stroke_incremental",
    # Every deformer arrives through clay_item_add_deformer, so a pattern
    # matching that entry point would collapse fourteen warps into one verb and
    # call the family covered as soon as any single one had a case. The
    # brush-shaped ones are named individually instead. Magnify and noise are
    # already measured, under the session verbs `session_magnify_pinch` and
    # `session_noise`; pose is ZBrush's Rotate and had no case at all.
    "pose",
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


# The gate's own floor, imported rather than copied so the two cannot drift
# apart about what counts as a difference. check_device_bench requires a
# regression to be BOTH relatively large (its tolerance) and absolutely
# meaningful (this), which is right -- a 20 us move on a tablet is not a move.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
try:
    from check_device_bench import DEFAULT_TOLERANCE as TOLERANCE
    from check_device_bench import NOISE_FLOOR_MS, normalised_p95, worst_p95
except ImportError:  # pragma: no cover - the tools ship together
    NOISE_FLOOR_MS, TOLERANCE, worst_p95, normalised_p95 = 0.05, 1.4, None, None


def gate_reach(run: dict, case: dict) -> tuple[float, float | None]:
    """What this case measures, and the ratio a regression must reach to FAIL.

    A case fails only when the growth is over the tolerance AND over the floor,
    so with a measurement of `m` the smallest detectable ratio is
    `max(TOLERANCE, (m + NOISE_FLOOR_MS) / m)`. Below `NOISE_FLOOR_MS / (TOLERANCE - 1)`
    the floor is the binding constraint and the tolerance never applies.

    THE NORMALISED FIGURE, because that is the one the gate decides on. This
    read `worst_p95` — the RAW number — and so answered a different question
    than the gate asks. The two part company exactly when the device was slow
    while a case ran: normalisation divides the measurement by that slowdown,
    so a case can clear the floor raw and sit under it normalised, and be
    reported GATED while the gate cannot fail it.

    `sdf_move` was that case, and it is not academic: raw 0.1324 ms reads as
    gated, normalised 0.1158 ms needs 1.43x, and a REAL 1.46x regression in
    `layer_move_surface` has been sitting under it since v0.52.2 unreported —
    over the tolerance, under the floor, and called protected by this line.

    Returns (measured, ratio) with ratio None when the case took no measurement.
    """
    ms = case.get("measurements") or []
    if not ms:
        return (0.0, None)
    if normalised_p95:
        measured = normalised_p95(run, case)[0]
    else:  # pragma: no cover - the tools ship together
        measured = max(m["p95Ms"] for m in ms)
    if measured <= 0:
        return (0.0, None)
    return (measured, max(TOLERANCE, (measured + NOISE_FLOOR_MS) / measured))


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

    # MEASURED IS NOT PROTECTED, and until now this line said the first and was
    # read as the second (issue #337). A verb whose case measures 6 us cannot
    # fail the gate at any plausible regression: the floor is the binding
    # constraint long before the tolerance is. Saying so costs nothing and stops
    # `coverage: OK` implying a guarantee the tool does not provide.
    cases = {c["name"]: c for c in record.get("cases", [])}
    gated, reported = [], []
    for entry in table:
        if entry.get("exemption") is not None:
            continue
        case = cases.get(entry.get("caseName") or "")
        if case is None:
            continue
        value, ratio = gate_reach(record, case)
        (gated if ratio is not None and ratio <= TOLERANCE else reported).append(
            (entry["verb"], entry.get("caseName"), value, ratio))

    print(f"coverage: {covered} verb(s) measured, {exempt} exempt, "
          f"{len(measured)} case(s) in the run")
    if reported:
        print(f"  of the {covered} measured: {len(gated)} GATED, {len(reported)} "
              f"REPORTED ONLY — too small for the {NOISE_FLOOR_MS:.2f} ms floor, so a "
              f"regression in them cannot fail this check")
    else:
        # Worth saying rather than staying silent: "all of them" is the claim
        # this line exists to make, and printing the reported-only clause with a
        # count of zero reads as though the caveat still applied to something.
        print(f"  of the {covered} measured: all {len(gated)} GATED — every one "
              f"measures above the {NOISE_FLOOR_MS:.2f} ms floor, so a regression "
              f"in any of them can fail this check")
    for verb, case, value, ratio in sorted(reported, key=lambda r: -(r[3] or 0))[:8]:
        shown = f"{ratio:.0f}x" if ratio and ratio >= 10 else f"{ratio:.2f}x"
        print(f"  reported only: {verb} ({case}) {value:.4f} ms — needs {shown}")
    if len(reported) > 8:
        print(f"  ...and {len(reported) - 8} more reported-only verbs")

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

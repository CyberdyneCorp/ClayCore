"""Run every example and regenerate the committed gallery.

    python examples/run_all.py            # one process per core
    python examples/run_all.py --jobs 1   # serial, for debugging one example

A failure in any one is fatal, which is what makes the CI job meaningful: an
API change that breaks an example breaks the build, the gate the hand-written
docs snippets never had.

Examples run in SEPARATE PROCESSES, and the reason is measurement rather than
taste: this job took ~70 minutes of wall clock on a two-core CI runner while
every other job in the workflow finished inside 16, because 43 examples that
each CPU-raytrace their images were being run one after another in a single
process. They are independent — each writes and reads only its own files —
so the only thing the serial loop bought was ordered output, which is
recovered here by buffering each example's stdout and printing it in list
order once it finishes.
"""

import argparse
import ast
import contextlib
import importlib
import io
import os
import pathlib
import sys
import time
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))

EXAMPLES = [
    "00_hero",
    "01_primitives",
    "02_blends",
    "03_deformers",
    "04_repetition",
    "05_lifts",
    "06_transitions",
    "07_voxel_sculpting",
    "08_meshing_and_io",
    "09_sculpt_brushes",
    "10_editing",
    "11_masks",
    "12_strokes",
    "13_curves",
    "14_cut",
    "15_voxel_verbs_and_repair",
    "16_loft",
    "17_swept",
    "18_sampled_fields",
    "19_mesh_import",
    "20_relax",
    "21_flatten",
    "22_snakehook",
    "23_magnify",
    "24_noise",
    "25_relief",
    "26_move_brush",
    "27_move_strokes",
    "28_hpolish",
    "29_claybuildup_smooth",
    "30_trim_curve",
    "31_move_topological",
    "32_tube",
    "33_mask_extrude",
    "34_organic_character",
    "35_hard_surface_helmet",
    "36_mesh_layers",
    "37_groups",
    "38_consolidation",
    "39_multi_resolution",
    "40_armature",
    "41_voxel_smooth_display",
    "42_representation_round_trip",
    "43_consolidation_keeps_colour",
    "44_quad_export",
    "45_mesh_brushes",
    "46_mesh_brush_compositions",
    "47_mesh_brush_reach_and_undo",
    "48_mesh_to_voxels",
    "49_mesh_lattice",
    "50_sdf_lattice",
    "51_lattice_gizmo",
    "52_sculpt_layers",
    "53_sdf_alphas",
    "54_masked_operations",
    "55_mesh_brush_vocabulary",
    "56_mesh_colour_brushes",
    "57_mesh_deformers",
    "58_attribute_transfer",
    "59_undo_across_representations",
    "60_surviving_a_crash",
    "61_stopping_a_long_operation",
    "62_what_this_document_costs",
    "63_surface_groups",
    "64_measuring_the_surface",
    "65_brush_presets",
    "66_dynamic_topology",
    "67_voxel_remesh",
    "68_mesh_multires",
]


# Which example demonstrates each living capability. The gallery already
# guards that every PRIMITIVE class has an example (01_primitives.py); this is
# the same idea one level up, and it is the gate the Phase 2 plan said should
# land with the first of those rows.
#
# A capability with no entry is an error. An entry of None is an explicit
# decision that no example can show it, with the reason — so an exemption is on
# the record rather than an omission nobody noticed.
CAPABILITY_EXAMPLES = {
    "sdf-kernels": "01_primitives",  # plus 16_loft and 17_swept for the lifts
    "scene-model": "10_editing",
    "brick-cache": None,          # an internal cache; its effect is meshing speed, not a picture
    "evaluation-backends": None,  # the same field on four devices — a parity test, not a render
    # plus 19_mesh_import for the reverse direction, 44_quad_export for quads,
    # and 45-47 for the fixed-topology mesh brushes
    # plus 66_dynamic_topology for the adaptive surface — the third mesh mode
    "meshing": "08_meshing_and_io",
    "picking": "10_editing",  # plus 45_mesh_brushes for raycasting a mesh layer
    # plus 36_mesh_layers for the mesh chunk
    "file-io": "08_meshing_and_io",
    # plus 11_masks, 33_mask_extrude, 39_multi_resolution for the level stack,
    # and 48_mesh_to_voxels for the triangles-to-cells bridge
    "voxel-engine": "15_voxel_verbs_and_repair",
    # plus 26_move_brush and 11_masks for the mask brush, 45_mesh_brushes for
    # apply_to_mesh (the fourth consumer), and 65_brush_presets for the brush
    # model and the named families as data
    "brush-engine": "12_strokes",
    "cut-tool": "14_cut",
    "python-bindings": None,      # every example IS this capability
    "c-abi": None,                # exercised from Swift and C, not from the gallery
    "build-packaging": None,      # a build concern; nothing to render
    # The adaptive surface: its own capability, its own example.
    "dynamic-topology": "66_dynamic_topology",
    # The subdivision hierarchy: likewise. Listed here while the change is
    # still active, which is what the asymmetry in check_capability_coverage
    # is for — a capability lands with its example in the same commit.
    "mesh-multires": "68_mesh_multires",
    "examples": None,             # this file
}


def check_duplicate_capability_keys():
    """No capability is listed twice in CAPABILITY_EXAMPLES.

    A dict literal keeps the LAST of a repeated key and discards the rest
    silently, so a duplicate is invisible at runtime — the map reads correct,
    the coverage count is right, and an entry with its comment has simply
    stopped existing. `file-io` and `voxel-engine` were each listed twice for
    long enough that the losing entries had accumulated notes nobody could see.
    Both happened to name the same example, so nothing was ever wrong; that is
    what made it survive, and is the reason this is a gate rather than a tidy-up.

    Read from the SOURCE rather than the dict, because by the time it is a dict
    the evidence is gone.
    """
    tree = ast.parse(pathlib.Path(__file__).read_text())
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if getattr(node.targets[0], "id", None) != "CAPABILITY_EXAMPLES":
            continue
        seen, dupes = {}, []
        for key in node.value.keys:
            name = key.value
            if name in seen:
                dupes.append(f"{name!r} at lines {seen[name]} and {key.lineno}")
            seen.setdefault(name, key.lineno)
        if dupes:
            return [f"CAPABILITY_EXAMPLES lists a capability twice, and the "
                    f"earlier entry is silently discarded: {'; '.join(dupes)}"]
    return []


def check_capability_coverage():
    """Every living capability has an example, or a recorded reason it cannot."""
    specs = os.path.join(os.path.dirname(HERE), "openspec", "specs")
    if not os.path.isdir(specs):
        return []                 # not a source checkout; nothing to guard
    living = {d for d in os.listdir(specs)
              if os.path.isdir(os.path.join(specs, d))}
    # WHAT AN ACTIVE CHANGE DECLARES IS *KNOWN* BUT NOT YET *LIVING*, and the
    # asymmetry is the whole point. A capability exists in this tree from the
    # moment a change proposes it and only becomes a directory under
    # openspec/specs when that change is ARCHIVED. So:
    #
    #   - it must NOT be required to have an example yet. Several active
    #     changes propose capabilities nobody has implemented, and demanding a
    #     gallery entry for them would make this gate fail on every proposal.
    #   - it must be ALLOWED to have one. A change that lands its capability
    #     with its example should add the entry in the same commit, and this
    #     check calling that entry stale is what stops it.
    #
    # Symmetric treatment gets one of the two wrong whichever way it is set.
    known = set(living)
    changes = os.path.join(os.path.dirname(HERE), "openspec", "changes")
    if os.path.isdir(changes):
        for change in os.listdir(changes):
            proposed = os.path.join(changes, change, "specs")
            if not os.path.isdir(proposed):
                continue
            known |= {d for d in os.listdir(proposed)
                      if os.path.isdir(os.path.join(proposed, d))}
    problems = []
    unlisted = living - set(CAPABILITY_EXAMPLES)
    if unlisted:
        problems.append(f"capabilities with no example and no recorded reason: "
                        f"{sorted(unlisted)} — add them to CAPABILITY_EXAMPLES")
    stale = set(CAPABILITY_EXAMPLES) - known
    if stale:
        problems.append(f"CAPABILITY_EXAMPLES names capabilities that no longer exist: "
                        f"{sorted(stale)}")
    for capability, example in sorted(CAPABILITY_EXAMPLES.items()):
        if example is not None and example not in EXAMPLES:
            problems.append(f"{capability} points at {example!r}, which is not in EXAMPLES")
    return problems


def check_every_example_runs():
    """Every examples/NN_*.py is in EXAMPLES.

    THE GATE THAT WAS MISSING, and it cost two examples. `65_brush_presets` and
    `66_dynamic_topology` were both written, both committed, both referenced
    from CAPABILITY_EXAMPLES' comments — and neither was ever added to the run
    list, so the gallery never ran either of them and nothing said so. An
    example that does not run is not an example; it is a file that compiles in
    someone's head.

    The capability gate above cannot catch this on its own: it only looks at the
    examples a capability POINTS AT, and a capability whose entry names an older
    example is perfectly happy while a newer one rots unrun.
    """
    root = os.path.join(os.path.dirname(HERE), "examples")
    listed = set(EXAMPLES)
    problems = []
    for name in sorted(os.listdir(root)):
        if not name.endswith(".py") or not name[:2].isdigit():
            continue
        stem = name[:-3]
        if stem not in listed:
            problems.append(f"examples/{name} exists but is not in EXAMPLES, "
                            f"so the gallery never runs it")
    for stem in EXAMPLES:
        if not os.path.isfile(os.path.join(root, stem + ".py")):
            problems.append(f"EXAMPLES lists {stem}, which has no file")
    return problems


def check_fast_scaling():
    """Every render entry point must honour CLAY_EXAMPLES_FAST.

    This is a gate rather than a convention because the omission has already
    happened once: when fast mode was added, `render_array` and `render_tile`
    scaled and `render_voxels_array` did not, so 00_hero composited a 150px
    image beside a 300px one and failed. It was caught by that example's own
    assertion, which is luck — an example that renders a lone unscaled image
    would have gone on quietly costing CI the time fast mode exists to save.

    Structural, not behavioural: an entry point passes if it scales its own
    dimensions, or if it delegates to one that does. That is cheap enough to
    run every time and cannot be defeated by a render being slow or absent.
    """
    import inspect

    import _render

    entries = {name: fn for name, fn in vars(_render).items()
               if name.startswith("render") and inspect.isfunction(fn)}
    sources = {name: inspect.getsource(fn) for name, fn in entries.items()}

    def scales(name, seen):
        if name in seen:            # a delegation cycle scales nothing
            return False
        seen.add(name)
        src = sources[name]
        if "_fast_pixels(" in src or "_fast_ao(" in src:
            return True
        return any(other != name and f"{other}(" in src and scales(other, seen)
                   for other in entries)

    missing = sorted(name for name in entries if not scales(name, set()))
    if not missing:
        return []
    return [f"render entry points ignore CLAY_EXAMPLES_FAST: {missing} — scale their "
            f"dimensions through _fast_pixels/_fast_ao, or delegate to one that does"]


def check_example_references():
    """Every `NN_name.py` named in the docs or in another example must exist.

    A gate rather than a convention because the omission has already shipped:
    `docs/05-claycore-library.md` and `35_hard_surface_helmet.py`'s own
    docstring both pointed a reader at `36_groups.py`, which stopped existing
    when `36` became `36_mesh_layers.py` and the groups example moved to `37`.
    Nothing failed. The example ran, the docs built, and the two links sat
    there long enough that a later reader concluded the example had never been
    written and wrote a second one.

    That is the failure worth catching: a dangling link does not read as
    broken, it reads as a file somebody forgot to add.

    Scope is the SHIPPED surface — README, docs, and the examples themselves.
    Archived openspec changes are a record of what was true when they were
    written and are deliberately not rewritten to match later renames.
    """
    import re

    root = pathlib.Path(HERE).parent
    have = {p.name for p in pathlib.Path(HERE).glob("[0-9][0-9]_*.py")}
    pattern = re.compile(r"\b(\d\d_[a-z0-9_]+\.py)\b")

    files = [root / "README.md"]
    files += sorted((root / "docs").glob("*.md"))
    # This file is excluded: it names the dead example above, and the EXAMPLES
    # list it does hold is checked by importing every entry rather than by
    # matching text.
    files += [p for p in sorted(pathlib.Path(HERE).glob("*.py")) if p.name != "run_all.py"]
    files += [pathlib.Path(HERE) / "README.md"]

    problems = []
    for path in files:
        if not path.exists():
            continue
        for name in sorted(set(pattern.findall(path.read_text(encoding="utf-8")))):
            if name not in have:
                problems.append(f"{path.relative_to(root)} points at examples/{name}, "
                                f"which does not exist — renamed, or never written")
    return problems


def run_one(name):
    """Run one example in this process, returning (name, output, error).

    Its stdout is captured rather than left to interleave with every other
    worker's: the report below prints it in list order, so parallel output
    reads exactly like the serial run it replaced.
    """
    sys.path.insert(0, HERE)
    buffer = io.StringIO()
    try:
        with contextlib.redirect_stdout(buffer):
            module = importlib.import_module(name)
            module.main()
    except SystemExit as exc:            # examples raise SystemExit on a gap
        return name, buffer.getvalue(), str(exc)
    except Exception as exc:             # noqa: BLE001 - report, keep going
        return name, buffer.getvalue(), f"{type(exc).__name__}: {exc}"
    return name, buffer.getvalue(), None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", "-j", type=int, default=0,
                        help="worker processes; 0 picks one per core, 1 runs serially")
    args = parser.parse_args()

    sys.path.insert(0, HERE)

    try:
        import pyclay  # noqa: F401
    except ImportError:
        print("pyclay is not importable — build the bindings first, e.g.\n"
              "  cmake --preset cpu-only -DCLAY_BUILD_PYTHON=ON && "
              "cmake --build --preset cpu-only\n"
              "  PYTHONPATH=build/cpu-only/bindings/python python examples/run_all.py",
              file=sys.stderr)
        return 1

    coverage = (check_duplicate_capability_keys() + check_capability_coverage()
                + check_every_example_runs() + check_fast_scaling()
                + check_example_references())
    if coverage:
        for problem in coverage:
            print(f"  {problem}", file=sys.stderr)
        return 1
    shown = sum(1 for v in CAPABILITY_EXAMPLES.values() if v is not None)
    print(f"capability coverage: {shown} shown by an example, "
          f"{len(CAPABILITY_EXAMPLES) - shown} recorded as unshowable")

    jobs = args.jobs or (os.cpu_count() or 1)
    failures = []
    started = time.time()

    if jobs == 1:
        results = [run_one(name) for name in EXAMPLES]
    else:
        # Each example holds its own grids and images, so peak memory scales
        # with the worker count. Capped rather than unbounded for that reason.
        jobs = min(jobs, len(EXAMPLES), 8)
        print(f"running {len(EXAMPLES)} examples across {jobs} processes")
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(run_one, EXAMPLES))

    for name, output, error in results:  # list order, not completion order
        if output:
            print(output, end="")
        if error is not None:
            failures.append((name, error))
            print(f"  FAILED [{name}]: {error}", file=sys.stderr)

    elapsed = time.time() - started
    print(f"\n{len(EXAMPLES) - len(failures)}/{len(EXAMPLES)} examples "
          f"succeeded in {elapsed:.1f}s")

    if failures:
        print("\nfailures:", file=sys.stderr)
        for name, message in failures:
            print(f"  {name}: {message}", file=sys.stderr)
        return 1

    output = os.path.join(HERE, "output")
    total = sum(
        os.path.getsize(os.path.join(output, f)) for f in os.listdir(output)
    )
    print(f"gallery is {total // 1024} KiB across {len(os.listdir(output))} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
import contextlib
import importlib
import io
import os
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
    "meshing": "08_meshing_and_io",
    "picking": "10_editing",  # plus 45_mesh_brushes for raycasting a mesh layer
    "file-io": "08_meshing_and_io",
    # plus 11_masks, 33_mask_extrude, and 39_multi_resolution for the level stack
    "voxel-engine": "15_voxel_verbs_and_repair",
    "file-io": "08_meshing_and_io",  # plus 36_mesh_layers for the mesh chunk
    "voxel-engine": "15_voxel_verbs_and_repair",  # plus 11_masks and 33_mask_extrude
    # plus 26_move_brush and 11_masks for the mask brush, and 45_mesh_brushes
    # for apply_to_mesh, the fourth consumer
    "brush-engine": "12_strokes",
    "cut-tool": "14_cut",
    "python-bindings": None,      # every example IS this capability
    "c-abi": None,                # exercised from Swift and C, not from the gallery
    "build-packaging": None,      # a build concern; nothing to render
    "examples": None,             # this file
}


def check_capability_coverage():
    """Every living capability has an example, or a recorded reason it cannot."""
    specs = os.path.join(os.path.dirname(HERE), "openspec", "specs")
    if not os.path.isdir(specs):
        return []                 # not a source checkout; nothing to guard
    living = {d for d in os.listdir(specs)
              if os.path.isdir(os.path.join(specs, d))}
    problems = []
    unlisted = living - set(CAPABILITY_EXAMPLES)
    if unlisted:
        problems.append(f"capabilities with no example and no recorded reason: "
                        f"{sorted(unlisted)} — add them to CAPABILITY_EXAMPLES")
    stale = set(CAPABILITY_EXAMPLES) - living
    if stale:
        problems.append(f"CAPABILITY_EXAMPLES names capabilities that no longer exist: "
                        f"{sorted(stale)}")
    for capability, example in sorted(CAPABILITY_EXAMPLES.items()):
        if example is not None and example not in EXAMPLES:
            problems.append(f"{capability} points at {example!r}, which is not in EXAMPLES")
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

    coverage = check_capability_coverage() + check_fast_scaling()
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

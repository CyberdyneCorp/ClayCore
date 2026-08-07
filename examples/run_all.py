"""Run every example and regenerate the committed gallery.

    python examples/run_all.py

Each example is run in-process. A failure in any one is fatal, which is what
makes the CI job meaningful: an API change that breaks an example breaks the
build, the gate the hand-written docs snippets never had.
"""

import importlib
import os
import sys
import time

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
    "meshing": "08_meshing_and_io",  # plus 19_mesh_import for the reverse direction
    "picking": "10_editing",
    "file-io": "08_meshing_and_io",
    "voxel-engine": "15_voxel_verbs_and_repair",
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


def main():
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

    coverage = check_capability_coverage()
    if coverage:
        for problem in coverage:
            print(f"  {problem}", file=sys.stderr)
        return 1
    shown = sum(1 for v in CAPABILITY_EXAMPLES.values() if v is not None)
    print(f"capability coverage: {shown} shown by an example, "
          f"{len(CAPABILITY_EXAMPLES) - shown} recorded as unshowable")

    failures = []
    started = time.time()
    for name in EXAMPLES:
        try:
            module = importlib.import_module(name)
            module.main()
        except SystemExit as exc:            # examples raise SystemExit on a gap
            failures.append((name, str(exc)))
            print(f"  FAILED: {exc}", file=sys.stderr)
        except Exception as exc:             # noqa: BLE001 - report, keep going
            failures.append((name, f"{type(exc).__name__}: {exc}"))
            print(f"  FAILED: {type(exc).__name__}: {exc}", file=sys.stderr)

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

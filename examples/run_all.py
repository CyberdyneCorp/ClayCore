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
]


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

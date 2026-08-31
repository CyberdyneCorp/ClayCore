#!/usr/bin/env python3
"""Gate the committed gallery against what the examples currently produce.

`examples/run_all.py` proves every example RUNS. It never asked whether the
pictures and documents in `examples/output/` still describe what the code makes,
so an output that merely CHANGED shipped as whatever was committed last. That is
how 182 of 251 outputs came to be stale, and how `08_scene.clayspace` came to be
committed at scene format **minor 9** while the writer emitted **minor 15** —
six format minors, in a byte format, sitting in the repository as a claim nobody
checked (#410).

WHAT THIS GATES, AND WHY NOT BYTES.

A render is float output and this repository already says so: bytes differ
across platforms, and the reporter of #410 measured a `20_relax` render that
differs from the committed one on their machine while it reproduces exactly on
the machine the gallery was regenerated on. So byte equality can only ever be a
per-platform claim, and a byte gate on one runner would fail honestly-generated
files from another.

What IS platform-independent is STRUCTURE — integers a float never perturbs:

  * `.clayspace`  the magic and the format major/minor
  * `.ply`        the declared element counts
  * `.obj`        the vertex, texture, normal and face line counts

Those are gated exactly. Every one of them would have caught this drift: the
minor moved 9 -> 15, and the meshes changed element counts. Renders are
REPORTED and not gated, because a number that cannot be trusted across runners
must not be allowed to fail a build.

    tools/check_gallery.py                 # compare against git HEAD
    tools/check_gallery.py --ref <rev>     # ...or another revision
    tools/check_gallery.py --report-only   # never exit non-zero
"""

import argparse
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
GALLERY = REPO / "examples" / "output"

# Renders. Reported, never gated -- see the module docstring.
RENDER_SUFFIXES = {".png"}
# Formats this tool has no structural reader for. A missing file is still a
# failure for them; a changed one is only reported, because "the bytes moved"
# on a binary third-party container says nothing a reader can act on.
OPAQUE_SUFFIXES = {".fbx", ".glb", ".mtl"}


def committed(ref: str, rel: str) -> bytes | None:
    """The blob at `ref`, or None when the file is not tracked there."""
    out = subprocess.run(["git", "show", f"{ref}:{rel}"], cwd=REPO,
                         capture_output=True)
    return out.stdout if out.returncode == 0 else None


def clayspace_version(data: bytes) -> tuple | None:
    """(magic, major, minor) from a .clayspace header."""
    if len(data) < 8 or data[:4] != b"CLAY":
        return None
    return ("CLAY",
            int.from_bytes(data[4:6], "little"),
            int.from_bytes(data[6:8], "little"))


def ply_elements(data: bytes) -> tuple | None:
    """Declared element counts, in header order: (("vertex", n), ("face", m))."""
    head = data[:4096].split(b"end_header", 1)[0]
    try:
        text = head.decode("ascii", errors="strict")
    except UnicodeDecodeError:
        return None
    found = re.findall(r"^element\s+(\w+)\s+(\d+)\s*$", text, re.MULTILINE)
    return tuple((name, int(n)) for name, n in found) if found else None


def obj_counts(data: bytes) -> tuple | None:
    """(v, vt, vn, f) line counts."""
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None
    counts = {"v": 0, "vt": 0, "vn": 0, "f": 0}
    for line in text.splitlines():
        key = line.split(" ", 1)[0]
        if key in counts:
            counts[key] += 1
    return tuple(sorted(counts.items()))


def structure(path: pathlib.Path, data: bytes):
    """A platform-independent description, or None when there is no reader."""
    suffix = path.suffix.lower()
    if suffix == ".clayspace":
        return clayspace_version(data)
    if suffix == ".ply":
        return ply_elements(data)
    if suffix == ".obj":
        return obj_counts(data)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ref", default="HEAD",
                        help="revision to compare the working tree against")
    parser.add_argument("--report-only", action="store_true",
                        help="print the drift and exit 0 regardless")
    args = parser.parse_args()

    if not GALLERY.is_dir():
        print(f"check-gallery: no gallery at {GALLERY}", file=sys.stderr)
        return 1

    tracked = subprocess.run(
        ["git", "ls-files", "examples/output"], cwd=REPO,
        capture_output=True, text=True, check=True).stdout.split()

    failures, changed_renders, changed_opaque, missing, unreadable = [], [], [], [], []

    for rel in sorted(tracked):
        path = REPO / rel
        name = pathlib.Path(rel).name
        suffix = pathlib.Path(rel).suffix.lower()
        old = committed(args.ref, rel)
        if old is None:
            continue  # not in the ref: a new output, nothing to compare
        if not path.exists():
            missing.append(name)
            continue
        new = path.read_bytes()
        if new == old:
            continue

        if suffix in RENDER_SUFFIXES:
            changed_renders.append(name)
            continue
        if suffix in OPAQUE_SUFFIXES:
            changed_opaque.append(name)
            continue

        old_s, new_s = structure(path, old), structure(path, new)
        if old_s is None or new_s is None:
            unreadable.append(name)
        elif old_s != new_s:
            failures.append((name, old_s, new_s))

    print(f"check-gallery: {len(tracked)} tracked outputs, compared against {args.ref}")

    if failures:
        print(f"\n  {len(failures)} output(s) whose STRUCTURE changed — the committed "
              f"gallery does not describe what the code makes:")
        for name, old_s, new_s in failures:
            print(f"    {name}")
            print(f"        committed: {old_s}")
            print(f"        produced : {new_s}")
    if missing:
        print(f"\n  {len(missing)} tracked output(s) the examples no longer produce: "
              f"{', '.join(missing[:8])}{' ...' if len(missing) > 8 else ''}")
    if unreadable:
        print(f"\n  {len(unreadable)} output(s) changed and could not be parsed: "
              f"{', '.join(unreadable[:8])}")
    if changed_renders:
        print(f"\n  {len(changed_renders)} render(s) differ in bytes. NOT a failure: a "
              f"render is float output and differs across platforms.")
        print(f"    {', '.join(changed_renders[:6])}"
              f"{' ...' if len(changed_renders) > 6 else ''}")
        print("    Regenerate with a full `python examples/run_all.py` if these are "
              "yours to refresh.")
    if changed_opaque:
        print(f"\n  {len(changed_opaque)} binary container(s) differ in bytes, reported "
              f"only: {', '.join(changed_opaque[:6])}")

    bad = failures or missing or unreadable
    if not bad:
        print("\ncheck-gallery: OK — every structural claim the gallery makes still holds")
        return 0
    if args.report_only:
        print("\ncheck-gallery: drift found (--report-only, not failing)")
        return 0
    print("\ncheck-gallery: FAILED — regenerate the gallery and commit it, or say in "
          "the PR why the structure was meant to change", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Module-layering include check (openspec build-packaging).

Enforces the dependency rule: kernel depends on nothing; scene/brick/mesh/
voxel/pick depend only on kernel+math; backends depend on eval; io and
bindings sit on top; no module depends on a backend.

Scans #include directives for clay/<module>/ paths in include/, src/,
backends/, bindings/, and tools/ and validates each edge.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# module -> modules it may include (itself always allowed)
ALLOWED = {
    # The data-parallel primitive, below everything and depending on nothing but
    # the standard library. It was a PRIVATE HEADER OF THE CPU BACKEND, which
    # meant the rule below — no module depends on a backend — locked the core
    # library out of the only pool in the tree: every mesher, every voxel verb,
    # redistance and the per-brick cull were serial because they could not
    # legally reach it, not because they resist parallelism. Moving it here is
    # what lets a core module use it AND lets this gate see that it does.
    "parallel": set(),
    "kernel": set(),  # the GPU dialect: no host threading, deliberately
    "math": {"parallel", "kernel"},
    "scene": {"parallel", "kernel", "math", "field"},
    "eval": {"parallel", "kernel", "math", "scene"},
    "brick": {"parallel", "kernel", "math", "scene", "eval"},
    # voxel -> field is the return trip (#90): a sculpt converting into a
    # sampled field so it can be an operand again. It adds no edge to the
    # transitive graph — voxel already depends on scene, and scene depends on
    # field — and it creates no cycle, because field depends on nothing above
    # kernel and math. The alternative homes were worse: brush and mesh can
    # both see field, but a representation conversion is neither a brush nor a
    # mesher, and putting it there would hide it from the type that owns the
    # cells.
    "voxel": {"parallel", "kernel", "math", "scene", "mesh", "field"},  # mesh_data.h is a leaf data type
    "mesh": {"parallel", "kernel", "math", "scene", "eval", "brick", "field"},
    # brush -> field is mask extrude: the join of a mask (above scene) and a
    # sampled field (below it). It cannot live in field without making
    # field -> voxel -> scene -> field a cycle, and brush already sits above
    # both. Nothing in field knows about brush.
    # brush -> mesh is apply_to_mesh, the stroke engine's fourth consumer: a
    # resolved stroke stamped onto a mesh layer's own vertices. It is the one
    # call that sees both a mesh and a mask, which is exactly why it lives
    # here — mesh may not include voxel (voxel already includes mesh), so a
    # masked mesh brush cannot live in mesh. No cycle: nothing in mesh knows
    # about brush.
    "brush": {"parallel", "kernel", "math", "scene", "voxel", "field", "mesh"},
    "cut": {"parallel", "kernel", "math", "scene"},
    "field": {"parallel", "kernel", "math"},  # a sampled field is a leaf payload, below scene
    # pick -> mesh is raycast_mesh. A mesh layer never enters a tape, so
    # raycast_scene cannot see one and never will; picking one means asking its
    # BVH directly. Nothing in mesh knows about pick.
    "pick": {"parallel", "kernel", "math", "scene", "eval", "brick", "voxel", "mesh"},
    "io": {"parallel", "kernel", "math", "scene", "eval", "brick", "voxel", "mesh", "field"},
}
CORE_MODULES = set(ALLOWED)
INCLUDE_RE = re.compile(r'#\s*include\s*[<"]clay/(\w+)/')
SCAN_ROOTS = ["include", "src", "backends", "bindings", "tools"]
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cpp", ".cc", ".cu", ".mm", ".metal", ".cl"}


def module_of(path: Path) -> str | None:
    """Return the layering identity of a file, or None if unconstrained."""
    rel = path.relative_to(REPO)
    parts = rel.parts
    if parts[0] == "include" and len(parts) > 3 and parts[1] == "clay":
        return parts[2] if parts[2] in CORE_MODULES else None
    if parts[0] == "src" and len(parts) > 2:
        return parts[1] if parts[1] in CORE_MODULES else None
    if parts[0] == "backends":
        return "backend"
    if parts[0] == "bindings":
        return "binding"
    return None  # top-level src files, tools: unconstrained except backend ban


def main() -> int:
    errors = []
    for root in SCAN_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            mod = module_of(path)
            text = path.read_text(errors="replace")
            for target in INCLUDE_RE.findall(text):
                rel = path.relative_to(REPO)
                if target not in CORE_MODULES and target != "version":
                    errors.append(f"{rel}: includes unknown module 'clay/{target}/'")
                elif mod in CORE_MODULES and target in CORE_MODULES:
                    if target != mod and target not in ALLOWED[mod]:
                        errors.append(f"{rel}: module '{mod}' may not include 'clay/{target}/'")
                # backends may include any clay header; bindings sit on top: all allowed.
            if "backends/" not in str(path.relative_to(REPO)) and re.search(
                r'#\s*include\s*[<"]\.\./backends|[<"]backends/', text
            ):
                errors.append(f"{path.relative_to(REPO)}: no module may include backend headers")

    for e in errors:
        print(f"layering: {e}", file=sys.stderr)
    if not errors:
        print("layering: OK")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

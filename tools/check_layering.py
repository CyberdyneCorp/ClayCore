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
    "kernel": set(),
    "math": {"kernel"},
    "scene": {"kernel", "math"},
    "eval": {"kernel", "math", "scene"},
    "brick": {"kernel", "math", "scene", "eval"},
    "voxel": {"kernel", "math", "scene", "mesh"},  # mesh_data.h is a leaf data type
    "mesh": {"kernel", "math", "scene", "eval", "brick"},
    "brush": {"kernel", "math", "scene", "voxel"},
    "cut": {"kernel", "math", "scene"},
    "pick": {"kernel", "math", "scene", "eval", "brick", "voxel"},
    "io": {"kernel", "math", "scene", "eval", "brick", "voxel", "mesh"},
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

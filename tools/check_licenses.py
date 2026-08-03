#!/usr/bin/env python3
"""License-manifest gate (openspec build-packaging).

Cross-checks cmake/Dependencies.cmake against THIRD_PARTY_LICENSES.md:
every FetchContent_Declare needs a manifest row with an allowed license,
and every manifest row needs a declaration (no drift in either direction).
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ALLOWED_LICENSES = {
    "MIT",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "Apache-2.0",
    "zlib",
    "BSL-1.0",
}


def declared_deps() -> set[str]:
    text = (REPO / "cmake" / "Dependencies.cmake").read_text()
    return set(re.findall(r"FetchContent_Declare\(\s*(\w+)", text))


def manifest_rows() -> dict[str, str]:
    rows: dict[str, str] = {}
    for line in (REPO / "THIRD_PARTY_LICENSES.md").read_text().splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) >= 3 and cells[0] not in ("Dependency", "---", ""):
            if not set(cells[0]) <= set("-"):
                rows[cells[0]] = cells[2]
    return rows


def main() -> int:
    deps = declared_deps()
    rows = manifest_rows()
    errors = []

    for dep in sorted(deps - rows.keys()):
        errors.append(f"{dep}: declared in Dependencies.cmake but missing from THIRD_PARTY_LICENSES.md")
    for dep in sorted(rows.keys() - deps):
        errors.append(f"{dep}: listed in THIRD_PARTY_LICENSES.md but not declared in Dependencies.cmake")
    for dep, lic in sorted(rows.items()):
        if dep in deps and lic not in ALLOWED_LICENSES:
            errors.append(f"{dep}: license '{lic}' is not in the allowed set {sorted(ALLOWED_LICENSES)}")

    for e in errors:
        print(f"license-gate: {e}", file=sys.stderr)
    if not errors:
        print(f"license-gate: OK ({len(deps)} dependencies, all permissive)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

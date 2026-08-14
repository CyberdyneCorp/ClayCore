#!/usr/bin/env python3
"""SwiftPM manifest gate (build-packaging spec): the `claycore` library product
is declared STATIC.

Why this exists as its own gate rather than as a test.

An automatic library product lets Xcode choose the linkage, and Xcode 26 chooses
a DYNAMIC PackageProduct framework in Debug. That framework links the slice
archive, but the product's only target is `ClayCoreLink` — one empty Swift file
carrying the Metal/Foundation linker settings a binaryTarget cannot hold — and
it references no archive member, so the linker pulls zero objects. The framework
then exports no `clay_*` symbols and every consuming app fails to link with
every symbol it uses undefined, while `nm -gU` on the archive shows them all
present. That was #112, and it shipped for four releases.

Nothing in the repository caught it, and the reason is worth stating because it
is why a gate is the right shape here: `swift run claycore-smoke` is an
EXECUTABLE. Its direct symbol references pull the archive members whatever the
product's linkage, so it passes either way — it exercises the one consumer shape
that cannot see the bug. Reproducing the real failure needs Xcode, a Debug
configuration and an app target, none of which exist in CI.

So this gate asserts the DECLARATION instead of the behaviour. That is a weaker
claim and it is stated as one, but it is exactly the claim that was violated:
the linkage was never deliberately dynamic, it was left unspecified.

Two modes, and the output always says which one ran:

  authoritative — `swift package dump-package` resolves the manifest and reports
                  the product's real type. Used whenever a Swift toolchain is
                  present.
  textual       — no toolchain (Linux CI, most contributors' machines), so the
                  manifest is read as text. Weaker: it proves the source says
                  static, not that SwiftPM read it that way.

Run: python3 tools/check_swift_package.py
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "Package.swift"

# The products whose linkage is load-bearing, and why. A product not named here
# is not checked — adding one is how a new product's linkage becomes a decision
# rather than whatever SwiftPM defaulted to.
REQUIRED_STATIC = {
    "claycore": "an app consuming it links zero symbols when Xcode builds it "
                "as a dynamic PackageProduct framework (#112)",
}


def dump_package():
    """The manifest as SwiftPM reads it, or None when there is no toolchain."""
    if not shutil.which("swift"):
        return None
    try:
        out = subprocess.run(["swift", "package", "dump-package"], cwd=REPO,
                             capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        # A manifest that will not parse is a failure worth reporting rather
        # than silently falling back to reading it as text.
        print("swift-package: `swift package dump-package` failed:\n"
              + out.stderr.strip(), file=sys.stderr)
        return False
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return False


def check_authoritative(package):
    """Product types straight out of SwiftPM."""
    failures = []
    products = {p["name"]: p for p in package.get("products", [])}
    for name, why in REQUIRED_STATIC.items():
        product = products.get(name)
        if product is None:
            failures.append(f"{name}: no such product in Package.swift")
            continue
        # {"library": ["static"]} / ["dynamic"] / ["automatic"], or an
        # executable/plugin product, which has a different key entirely.
        kinds = product.get("type", {}).get("library")
        if kinds is None:
            failures.append(f"{name}: not a library product")
        elif "static" not in kinds:
            declared = ", ".join(kinds) or "unspecified"
            failures.append(
                f"{name}: declared {declared}, must be static — {why}. "
                f"Write `.library(name: \"{name}\", type: .static, targets: [...])`.")
    return failures


def check_textual(text):
    """The manifest read as source, for a machine with no Swift toolchain."""
    failures = []
    for name, why in REQUIRED_STATIC.items():
        # The declaration may wrap across lines, so match from the name to the
        # closing paren of that .library( call rather than line by line.
        match = re.search(rf'\.library\(\s*name:\s*"{re.escape(name)}"(.*?)\)\s*,',
                          text, re.S)
        if match is None:
            failures.append(f"{name}: no .library(name: \"{name}\", ...) in Package.swift")
            continue
        if not re.search(r"type:\s*\.static", match.group(1)):
            failures.append(
                f"{name}: declared without `type: .static` — {why}. "
                f"Write `.library(name: \"{name}\", type: .static, targets: [...])`.")
    return failures


def main() -> int:
    if not MANIFEST.is_file():
        print("swift-package: no Package.swift; nothing to check")
        return 0

    package = dump_package()
    if package is False:
        return 1
    if package is not None:
        failures, mode = check_authoritative(package), "authoritative (swift package dump-package)"
    else:
        failures, mode = check_textual(MANIFEST.read_text()), "textual (no Swift toolchain)"

    for failure in failures:
        print(f"swift-package: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"swift-package: OK ({len(REQUIRED_STATIC)} product(s) static, {mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

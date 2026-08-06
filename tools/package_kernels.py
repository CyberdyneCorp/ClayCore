#!/usr/bin/env python3
"""Publish the kernel dialect as a standalone artifact (build-packaging spec).

    python3 tools/package_kernels.py [--out dist/claycore-kernels] [--clay path/to/clay]
    python3 tools/package_kernels.py --verify [--out dist/claycore-kernels]

A host that previews ClayCore documents on its own GPU has to evaluate the
field itself. Design principle 1 says there is one implementation of every
distance function and operator — which only holds outside this repository if
the headers leave it, so this script copies them out VERBATIM. It transforms
nothing: a second, generated version of the math would be the very drift the
artifact exists to prevent.

The package carries, alongside the headers, the parity fixture a host runs to
prove its evaluator agrees, and a worked MSL example. --verify re-checks a
built package against the repository, which is what CI gates on.
"""

import argparse
import filecmp
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KERNEL_DIR = REPO / "include" / "clay" / "kernel"
EXAMPLE = REPO / "tools" / "kernel_package" / "host_preview.metal"
DEFAULT_OUT = REPO / "dist" / "claycore-kernels"

CLAY_INCLUDE_RE = re.compile(r'#\s*include\s*"(clay/[^"]+)"')
SYSTEM_INCLUDE_RE = re.compile(r"#\s*include\s*<([^>]+)>")


def project_version() -> str:
    text = (REPO / "CMakeLists.txt").read_text()
    m = re.search(r"^\s*VERSION\s+(\d+\.\d+\.\d+)", text, re.M)
    return m.group(1) if m else "0.0.0"


def check_self_contained(headers: list[Path]) -> list[str]:
    """The artifact must compile with nothing but itself on the include path."""
    errors = []
    available = {f"clay/kernel/{h.name}" for h in headers}
    for header in headers:
        text = header.read_text()
        for inc in CLAY_INCLUDE_RE.findall(text):
            if inc not in available:
                errors.append(f"{header.name}: includes '{inc}', which is not in the package")
        # shim.h is the one header allowed to reach a backend's own standard
        # library (<metal_stdlib>, <cmath>); everything else stays portable.
        if header.name == "shim.h":
            continue
        for inc in SYSTEM_INCLUDE_RE.findall(text):
            errors.append(f"{header.name}: system include <{inc}> outside shim.h")
    return errors


def compile_standalone(out: Path) -> list[str]:
    """Compile the umbrella with ONLY the package on the include path.

    The self-containment check above is lexical; this is the real thing, and
    it is the scenario a consumer actually performs: copy the directory into
    an unrelated project and build. Skipped where no host compiler exists.
    """
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        return []
    errors = []
    with tempfile.TemporaryDirectory() as tmp:
        tu = Path(tmp) / "standalone.cpp"
        # both branches a host actually uses: plain C++ and, via the stub
        # <metal_stdlib>, the MSL one it gets from a .metal file
        for label, prelude, flags in (
            ("cpp", "", []),
            ("metal", "#define __METAL_VERSION__ 310\n",
             [f"-isystem{REPO / 'tools' / 'metal_stub'}"]),
        ):
            tu.write_text(prelude + '#include "clay/kernel/kernels.h"\n')
            proc = subprocess.run(
                [compiler, "-std=c++20", "-fsyntax-only", "-fno-exceptions", "-fno-rtti",
                 "-Wall", "-Wextra", "-Werror", *flags,
                 f"-I{out / 'include'}", str(tu)],
                capture_output=True, text=True)
            if proc.returncode != 0:
                errors.append(f"the package does not compile standalone [{label}]:\n"
                              f"{proc.stderr.strip()}")
    return errors


def export_fixture(clay_bin: Path | None, dest: Path) -> str:
    """Run `clay parity-fixture`, or say why the package ships without one."""
    if clay_bin is None:
        for candidate in sorted(REPO.glob("build/*/clay")):
            clay_bin = candidate
            break
    if clay_bin is None or not clay_bin.exists():
        return ("no clay binary found (pass --clay <path> or build one); "
                "packaged WITHOUT the parity fixture")
    dest.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run([str(clay_bin), "parity-fixture", "-o", str(dest)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return f"clay parity-fixture failed: {proc.stderr.strip()}"
    return ""


README = """# claycore kernels

The ClayCore kernel dialect: one implementation of every distance function,
blend, deformer, lift and the tape interpreter, compiled by ClayCore into its
CPU, Metal, CUDA and OpenCL backends — and by you, here, into your own.

Version: {version}

## Use it from Metal

```metal
#include "clay/kernel/kernels.h"
using namespace clay::kernels;   // MSL reserves `kernel`
```

Put `include/` on the Metal header search path. No `-D` flags: `shim.h` reads
`__METAL_VERSION__` and selects the MSL branch itself. `examples/host_preview.metal`
is a complete, compilable evaluator over ClayCore tape buffers.

CUDA (`__CUDACC__`), OpenCL (`__OPENCL_VERSION__`) and plain C++ are detected
the same way; define `CLAY_KERNEL_METAL` / `_CUDA` / `_OPENCL` / `_CPU`
explicitly to override.

## Prove your preview agrees

`parity/kernel_parity.json` holds composed tapes, probe points, and the
distance and color ClayCore's CPU reference produces at each probe, with the
tolerances to apply. Evaluate each case's tape with `ctape_eval` and compare.
Wire it into your CI: a preview that agrees here cannot disagree with a bake.

Regenerate it with `clay parity-fixture -o kernel_parity.json`.

## Do not edit these headers

They are a byte-for-byte copy of `include/clay/kernel/` in the claycore
repository. A local edit is a fork of the math, which is the failure this
package exists to prevent — send the change upstream instead.
"""


def build(out: Path, clay_bin: Path | None) -> int:
    headers = sorted(KERNEL_DIR.glob("*.h"))
    errors = check_self_contained(headers)
    if errors:
        for e in errors:
            print(f"package-kernels: {e}", file=sys.stderr)
        return 1

    if out.exists():
        shutil.rmtree(out)
    include_dir = out / "include" / "clay" / "kernel"
    include_dir.mkdir(parents=True)
    for header in headers:
        shutil.copyfile(header, include_dir / header.name)

    (out / "examples").mkdir()
    shutil.copyfile(EXAMPLE, out / "examples" / EXAMPLE.name)
    version = project_version()
    (out / "VERSION").write_text(version + "\n")
    (out / "README.md").write_text(README.format(version=version))

    errors = compile_standalone(out)
    if errors:
        for e in errors:
            print(f"package-kernels: {e}", file=sys.stderr)
        return 1

    note = export_fixture(clay_bin, out / "parity" / "kernel_parity.json")
    if note:
        print(f"package-kernels: {note}", file=sys.stderr)
    print(f"package-kernels: {out.relative_to(REPO)} ready "
          f"({len(headers)} headers, claycore {version})")
    return 0


def verify(out: Path) -> int:
    headers = sorted(KERNEL_DIR.glob("*.h"))
    packaged = out / "include" / "clay" / "kernel"
    errors = check_self_contained(headers)
    for header in headers:
        target = packaged / header.name
        if not target.exists():
            errors.append(f"{header.name}: missing from the package")
        elif not filecmp.cmp(header, target, shallow=False):
            errors.append(f"{header.name}: package copy differs from include/clay/kernel/")
    for stray in sorted(packaged.glob("*.h")) if packaged.is_dir() else []:
        if not (KERNEL_DIR / stray.name).exists():
            errors.append(f"{stray.name}: in the package but not in the repository")
    if not errors:
        errors += compile_standalone(out)
    for e in errors:
        print(f"package-kernels: {e}", file=sys.stderr)
    if not errors:
        print(f"package-kernels: OK ({len(headers)} headers byte-identical to the repository)")
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--clay", type=Path, default=None,
                        help="clay-cli binary used to export the parity fixture")
    parser.add_argument("--verify", action="store_true",
                        help="check an existing package against the repository")
    args = parser.parse_args()
    return verify(args.out) if args.verify else build(args.out, args.clay)


if __name__ == "__main__":
    sys.exit(main())

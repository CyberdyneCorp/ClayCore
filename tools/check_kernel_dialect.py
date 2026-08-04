#!/usr/bin/env python3
"""Kernel-dialect enforcement (openspec sdf-kernels: Kernel dialect portability).

Kernel headers must compile on CPU, Metal (MSL), CUDA, and OpenCL, which
means: no virtuals, no exceptions, no heap allocation, no recursion, no
standard headers beyond a small allowlist. Two enforcement layers:

1. Lexical bans on comment/string-stripped source (catches constructs that
   still compile on the host, e.g. `new`, `throw`, disallowed includes).
2. A restrictive host compile of every header in isolation:
   -std=c++20 -fno-exceptions -fno-rtti with warnings-as-errors.

Recursion is not statically checked here; the parity/property suites and
per-backend compiles (which reject it outright on Metal/OpenCL) cover it.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KERNEL_DIR = REPO / "include" / "clay" / "kernel"

ALLOWED_STD_HEADERS = {"cstdint", "cstddef", "cfloat", "cmath", "limits", "type_traits"}
BANNED_KEYWORDS = ["virtual", "throw", "try", "catch", "new", "delete", "goto", "thread_local", "malloc", "free"]


def strip_comments_and_strings(text: str) -> str:
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", "''", text)
    return text


def lexical_check(path: Path) -> list[str]:
    errors = []
    code = strip_comments_and_strings(path.read_text())
    rel = path.relative_to(REPO)
    for kw in BANNED_KEYWORDS:
        if re.search(rf"\b{kw}\b", code):
            errors.append(f"{rel}: banned construct '{kw}'")
    # shim.h is the ONE header allowed backend includes (<metal_stdlib>, ...)
    if path.name != "shim.h":
        for inc in re.findall(r"#\s*include\s*<([^>]+)>", code):
            name = inc.split("/")[0]
            if name not in ALLOWED_STD_HEADERS and not inc.startswith("clay/"):
                errors.append(
                    f"{rel}: system include <{inc}> not in kernel allowlist {sorted(ALLOWED_STD_HEADERS)}")
    return errors


def compile_check(path: Path, compiler: str, profile: str = "cpu") -> list[str]:
    """Compile one header in isolation under a backend profile.

    cpu:  the reference profile, warnings-as-errors.
    cuda: host-emulated (__device__/__host__/__global__ defined away). This
          proves the shim's CUDA branch and the headers are valid C++ under
          it without a CUDA toolchain; nvcc device codegen is verified by the
          CUDA CI job and on real hardware.
    """
    prelude = ""
    define = "-DCLAY_KERNEL_CPU=1"
    if profile == "cuda":
        prelude = ("#define __host__\n#define __device__\n#define __global__\n"
                   "#define __forceinline__\n")
        define = "-DCLAY_KERNEL_CUDA=1"
    with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False) as tu:
        tu.write(prelude + f'#include "{path.relative_to(REPO / "include")}"\n')
        tu_path = tu.name
    cmd = [
        compiler, "-std=c++20", "-fsyntax-only",
        "-fno-exceptions", "-fno-rtti",
        "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        define,
        f"-I{REPO / 'include'}",
        tu_path,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    Path(tu_path).unlink(missing_ok=True)
    if proc.returncode != 0:
        return [f"{path.relative_to(REPO)} [{profile}]: restrictive compile failed:\n"
                f"{proc.stderr.strip()}"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="c++", help="host C++ compiler for the restrictive compile")
    args = parser.parse_args()

    headers = sorted(KERNEL_DIR.rglob("*.h"))
    if not headers:
        print("kernel-dialect: no kernel headers found", file=sys.stderr)
        return 1

    errors = []
    for header in headers:
        errors += lexical_check(header)
        errors += compile_check(header, args.compiler, "cpu")
        errors += compile_check(header, args.compiler, "cuda")

    for e in errors:
        print(f"kernel-dialect: {e}", file=sys.stderr)
    if not errors:
        print(f"kernel-dialect: OK ({len(headers)} headers x cpu+cuda profiles)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

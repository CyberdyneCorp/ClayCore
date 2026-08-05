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

Profiles, in the order they run:

  cpu    the reference profile.
  cuda   host-emulated (__device__/__host__ defined away).
  metal  host-emulated against tools/metal_stub/metal_stdlib. The headers are
         now published for host shaders to include (build-packaging: kernels
         artifact), so a break in the Metal branch of shim.h must fail on any
         runner rather than waiting for an Apple one.
  metal-native  the real thing, `xcrun metal`, where an Apple toolchain exists.
  opencl the amalgamated kernel text through clang's OpenCL frontend.

The two Metal profiles compile WITHOUT defining CLAY_KERNEL_METAL: shim.h is
expected to select the branch from the compiler's own identity, which is what
lets a host include the headers from a .metal file with no build settings.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KERNEL_DIR = REPO / "include" / "clay" / "kernel"
METAL_STUB_DIR = REPO / "tools" / "metal_stub"

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


# Host-emulated backend profiles: what to prepend to the one-line translation
# unit, and what extra flags it needs. `defines` is empty for metal on purpose
# — the point is that shim.h picks the branch from __METAL_VERSION__ alone.
COMPILE_PROFILES = {
    "cpu": {
        "prelude": "",
        "defines": ["-DCLAY_KERNEL_CPU=1"],
        "flags": [],
    },
    "cuda": {
        "prelude": ("#define __host__\n#define __device__\n#define __global__\n"
                    "#define __forceinline__\n"),
        "defines": ["-DCLAY_KERNEL_CUDA=1"],
        "flags": [],
    },
    "metal": {
        # __METAL_VERSION__ stands in for the Metal compiler's identity; the
        # stub <metal_stdlib> stands in for its standard library.
        "prelude": "#define __METAL_VERSION__ 310\n",
        "defines": [],
        "flags": [f"-isystem{METAL_STUB_DIR}"],
    },
}


def compile_check(path: Path, compiler: str, profile: str = "cpu") -> list[str]:
    """Compile one header in isolation under a host-emulated backend profile.

    Emulation is what makes these checks runnable on any machine: nvcc device
    codegen is verified by the CUDA CI job and on real hardware, and real MSL
    by metal_native_check below, but a header that stops being valid C++ under
    a backend's shim branch is caught here, on every runner.
    """
    spec = COMPILE_PROFILES[profile]
    with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False) as tu:
        tu.write(spec["prelude"] + f'#include "{path.relative_to(REPO / "include")}"\n')
        tu_path = tu.name
    cmd = [
        compiler, "-std=c++20", "-fsyntax-only",
        "-fno-exceptions", "-fno-rtti",
        "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        *spec["defines"], *spec["flags"],
        f"-I{REPO / 'include'}",
        tu_path,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    Path(tu_path).unlink(missing_ok=True)
    if proc.returncode != 0:
        return [f"{path.relative_to(REPO)} [{profile}]: restrictive compile failed:\n"
                f"{proc.stderr.strip()}"]
    return []


def metal_native_check(headers: list[Path]) -> list[str]:
    """Compile every kernel header as real MSL with `xcrun metal`.

    This is the profile that speaks for the published artifact: a host adds
    the headers to its Metal header search path and includes them, with no
    CLAY_KERNEL_* define anywhere, exactly as done here. Address-space
    qualifiers, MSL's reserved words and its actual standard library are only
    checked here — the emulated profile above erases them.
    """
    if sys.platform != "darwin" or not shutil.which("xcrun"):
        print("kernel-dialect: no Apple toolchain, skipping the native Metal profile")
        return []

    def compile_msl(path: Path, label: str) -> list[str]:
        proc = subprocess.run(
            ["xcrun", "-sdk", "macosx", "metal", "-std=metal3.0", "-fsyntax-only",
             "-Wall", "-Werror", f"-I{REPO / 'include'}", str(path)],
            capture_output=True, text=True)
        if proc.returncode != 0:
            return [f"{label} [metal-native]: MSL compile failed:\n{proc.stderr.strip()}"]
        return []

    errors = []
    with tempfile.TemporaryDirectory() as tmp:
        for header in headers:
            src = Path(tmp) / "tu.metal"
            src.write_text(f'#include "{header.relative_to(REPO / "include")}"\n')
            errors += compile_msl(src, str(header.relative_to(REPO)))
    # the worked example the kernels artifact ships: if a host cannot compile
    # this, the artifact is not usable no matter what the headers do alone
    example = REPO / "tools" / "kernel_package" / "host_preview.metal"
    errors += compile_msl(example, str(example.relative_to(REPO)))
    return errors


def opencl_check() -> list[str]:
    """Compile the amalgamated kernel text as OpenCL C.

    OpenCL C is C99: no namespaces, no overloading, no templates. clang's
    OpenCL frontend is what pocl (and most ICDs) use, so this catches
    C++-isms before they reach a device — the gap that let a namespaced
    helper into ease.h and broke the OpenCL backend on Linux while Apple's
    compiler accepted it.
    """
    if not shutil.which("clang"):
        print("kernel-dialect: clang not found, skipping the OpenCL profile")
        return []
    with tempfile.TemporaryDirectory() as tmp:
        cl_path = Path(tmp) / "kernels.cl"
        gen = subprocess.run(
            [sys.executable, str(REPO / "tools" / "amalgamate_cl.py"), "--raw",
             "clay/kernel/tape.h", str(cl_path), "clay_cl_source",
             str(REPO / "backends" / "opencl" / "clay_kernels.cl.in")],
            capture_output=True, text=True)
        if gen.returncode != 0:
            return [f"OpenCL amalgamation failed:\n{gen.stderr.strip()}"]
        proc = subprocess.run(
            ["clang", "-x", "cl", "-cl-std=CL1.2", "-Xclang", "-finclude-default-header",
             "-DCLAY_KERNEL_OPENCL=1", "-fsyntax-only", str(cl_path)],
            capture_output=True, text=True)
        if proc.returncode != 0:
            return [f"OpenCL profile compile failed:\n{proc.stderr.strip()[:2000]}"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default="c++", help="host C++ compiler for the restrictive compile")
    args = parser.parse_args()

    headers = sorted(KERNEL_DIR.rglob("*.h"))
    if not headers:
        print("kernel-dialect: no kernel headers found", file=sys.stderr)
        return 1

    profiles = list(COMPILE_PROFILES)
    errors = []
    for header in headers:
        errors += lexical_check(header)
        for profile in profiles:
            errors += compile_check(header, args.compiler, profile)
    errors += metal_native_check(headers)
    errors += opencl_check()

    for e in errors:
        print(f"kernel-dialect: {e}", file=sys.stderr)
    if not errors:
        print(f"kernel-dialect: OK ({len(headers)} headers x {'+'.join(profiles)} profiles, "
              f"plus the OpenCL amalgamation)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

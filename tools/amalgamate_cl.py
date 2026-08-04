#!/usr/bin/env python3
"""Amalgamate the kernel headers into one OpenCL C source string.

OpenCL programs are built from source strings, and `#include "clay/..."`
cannot be resolved by every runtime, so the build inlines the headers here
(each exactly once, in dependency order) and emits a C string literal the
OpenCL backend hands to clCreateProgramWithSource. Single-source rule
intact: the text comes from the same headers every other backend compiles.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"(clay/[^"]+)"\s*$')


def inline(path: Path, seen: set[str], out: list[str]) -> None:
    rel = str(path.relative_to(REPO / "include"))
    if rel in seen:
        return
    seen.add(rel)
    out.append(f"// ---- {rel} ----")
    for line in path.read_text().splitlines():
        m = INCLUDE_RE.match(line)
        if m:
            inline(REPO / "include" / m.group(1), seen, out)
            continue
        if line.strip() == "#pragma once":
            continue
        out.append(line)


def main() -> int:
    if len(sys.argv) not in (4, 5):
        print("usage: amalgamate_cl.py <entry-header> <output.cpp> <symbol> [kernels.cl]",
              file=sys.stderr)
        return 2
    entry = REPO / "include" / sys.argv[1]
    lines: list[str] = []
    inline(entry, set(), lines)
    if len(sys.argv) == 5:  # kernel entry points appended verbatim
        lines.append(f"// ---- {Path(sys.argv[4]).name} ----")
        lines.extend(Path(sys.argv[4]).read_text().splitlines())
    source = "\n".join(lines) + "\n"

    # C string literal, chunked so no line gets unwieldy
    # byte length, not character count: the headers carry UTF-8 in comments
    # and the C array is bytes (a char-count here truncates the program)
    source_bytes = len(source.encode("utf-8"))
    escaped = source.replace("\\", "\\\\").replace('"', '\\"')
    body = "\n".join(f'    "{ln}\\n"' for ln in escaped.splitlines())
    symbol = sys.argv[3]
    Path(sys.argv[2]).write_text(
        f"// generated from {sys.argv[1]} by tools/amalgamate_cl.py — do not edit\n"
        f'extern "C" {{\n'
        f"extern const char {symbol}[];\nextern const unsigned int {symbol}_size;\nconst char {symbol}[] =\n{body};\n"
        f"const unsigned int {symbol}_size = {source_bytes}u;\n"
        f"}}\n"
    )
    print(f"amalgamate_cl: {source_bytes} bytes from {len(lines)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())

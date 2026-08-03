#!/usr/bin/env python3
"""Embed a binary file as a C byte array (used to bundle the compiled
Metal library into libclaycore)."""

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: embed_binary.py <input> <output.c> <symbol>", file=sys.stderr)
        return 2
    data = Path(sys.argv[1]).read_bytes()
    symbol = sys.argv[3]
    # `extern` on the definitions: const alone would give internal linkage
    lines = [f"// generated from {Path(sys.argv[1]).name} — do not edit",
             'extern "C" {',
             f"extern const unsigned char {symbol}[] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append(f"extern const unsigned int {symbol}_size = {len(data)}u;")
    lines.append("}")
    Path(sys.argv[2]).write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

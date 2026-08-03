#!/usr/bin/env python3
"""FFI hygiene check for the C ABI (c-abi spec: bindgen-clean).

1. Lexical rules on clay.h: no variadics, no bitfields, no bare long —
   the patterns that break Swift/C#/Rust bindings generators.
2. A real cross-language FFI exercise: load the shared library with Python
   ctypes (a bindings generator equivalent) and drive the core flow.
"""

import ctypes
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def hygiene() -> list[str]:
    text = (REPO / "bindings" / "c" / "clay.h").read_text()
    # strip comments
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    errors = []
    if re.search(r",\s*\.\.\.", text):
        errors.append("variadic function in clay.h")
    for struct in re.findall(r"typedef struct \w*\s*{([^}]*)}", text):
        if re.search(r"\w+\s*:\s*\d+\s*;", struct):
            errors.append("bitfield in public struct")
    if re.search(r"\b(?<!u)long\b", text):
        errors.append("bare 'long' (platform-dependent width) in clay.h")
    if re.search(r"\bunsigned int\b|\bshort\b", text):
        errors.append("non-fixed-width integer type in clay.h")
    return errors


def ffi_exercise(lib_path: str) -> list[str]:
    lib = ctypes.CDLL(lib_path)
    errors = []

    lib.clay_version.argtypes = [ctypes.POINTER(ctypes.c_int32)] * 3
    lib.clay_last_error.restype = ctypes.c_char_p
    lib.clay_document_create.restype = ctypes.c_void_p
    lib.clay_document_destroy.argtypes = [ctypes.c_void_p]
    lib.clay_add_sdf_layer.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                       ctypes.POINTER(ctypes.c_uint32)]
    lib.clay_list_backends.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t)]

    major, minor, patch = ctypes.c_int32(-1), ctypes.c_int32(-1), ctypes.c_int32(-1)
    lib.clay_version(ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch))
    if major.value < 0 or minor.value < 0:
        errors.append(f"clay_version returned {major.value}.{minor.value}.{patch.value}")

    size = ctypes.c_size_t(0)
    if lib.clay_list_backends(None, ctypes.byref(size)) != 0 or size.value < 4:
        errors.append("clay_list_backends size query failed")
    buf = ctypes.create_string_buffer(size.value)
    if lib.clay_list_backends(buf, ctypes.byref(size)) != 0 or b"cpu" not in buf.value:
        errors.append("clay_list_backends fill failed")

    doc = lib.clay_document_create()
    if not doc:
        errors.append("clay_document_create returned null")
    else:
        layer = ctypes.c_uint32(0)
        if lib.clay_add_sdf_layer(doc, b"ffi", ctypes.byref(layer)) != 0 or layer.value == 0:
            errors.append("clay_add_sdf_layer failed")
        lib.clay_document_destroy(doc)
    return errors


def main() -> int:
    errors = hygiene()
    if len(sys.argv) > 1:
        errors += ffi_exercise(sys.argv[1])
    else:
        print("c-abi: no shared library given, hygiene checks only")
    for e in errors:
        print(f"c-abi: {e}", file=sys.stderr)
    if not errors:
        print("c-abi: OK (hygiene + ctypes FFI)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

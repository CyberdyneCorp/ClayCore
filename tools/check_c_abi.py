#!/usr/bin/env python3
"""FFI hygiene check for the C ABI (c-abi spec: bindgen-clean).

1. Lexical rules on clay.h: no variadics, no bitfields, no bare long —
   the patterns that break Swift/C#/Rust bindings generators — and a leading
   uint32_t struct_size on every public descriptor struct, so fields can be
   appended without a major bump.
2. A real cross-language FFI exercise: load the shared library with Python
   ctypes (a bindings generator equivalent) and drive the core flow,
   including the struct_size prefix rule and the item builder.
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
    # body-bearing structs only: the opaque handle typedefs have no braces
    for name, body in re.findall(r"typedef struct (\w*)\s*{([^}]*)}", text):
        if re.search(r"\w+\s*:\s*\d+\s*;", body):
            errors.append(f"bitfield in public struct {name}")
        if not re.match(r"\s*uint32_t\s+struct_size\s*;", body):
            errors.append(f"public struct {name} lacks a leading uint32_t struct_size")
    if re.search(r"\b(?<!u)long\b", text):
        errors.append("bare 'long' (platform-dependent width) in clay.h")
    if re.search(r"\bunsigned int\b|\bshort\b", text):
        errors.append("non-fixed-width integer type in clay.h")
    return errors


class ItemDesc(ctypes.Structure):
    """clay_item_desc as a bindings generator would emit it."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("prim", ctypes.c_int32),
        ("params", ctypes.c_float * 7),
        ("position", ctypes.c_float * 3),
        ("rotation", ctypes.c_float * 4),
        ("scale", ctypes.c_float),
        ("op", ctypes.c_int32),
        ("blend", ctypes.c_int32),
        ("blend_k", ctypes.c_float),
        ("rounding", ctypes.c_float),
        ("color", ctypes.c_float * 3),
        ("mirror", ctypes.c_int32),
    ]


class FutureItemDesc(ctypes.Structure):
    """clay_item_desc as a caller compiled against a later header sees it."""

    _fields_ = ItemDesc._fields_ + [("appended", ctypes.c_float * 4)]


class ItemDescV0_1_0(ctypes.Structure):
    """clay_item_desc as ABI 0.1.0 declared it, before the struct_size prefix.

    Spelled out rather than derived from the current header: a binary built
    against 0.1.0 carries these bytes, and the library must reject it instead
    of reading every field one slot late off the end of a shorter object.
    """

    _fields_ = ItemDesc._fields_[1:]


def struct_size_exercise(lib, doc, layer: int) -> list[str]:
    """The versioned-descriptor contract, driven across the FFI boundary."""
    errors = []
    item = ItemDesc()
    item.prim = 0  # CLAY_PRIM_SPHERE
    item.params[0] = 1.0
    item.rotation[3] = 1.0
    item.scale = 1.0
    # setting it is mandatory: 0 is not a sentinel for the original layout,
    # because a 0.1.0 descriptor is indistinguishable from a zeroed one
    if lib.clay_add_item(doc, layer, ctypes.byref(item), None) != 1:
        errors.append("clay_add_item accepted a descriptor that declares no struct_size")
    item.struct_size = ctypes.sizeof(ItemDesc)
    if lib.clay_add_item(doc, layer, ctypes.byref(item), None) != 0:
        errors.append("clay_add_item rejected an explicit struct_size")
    item.struct_size = 4  # shorter than any layout that ever shipped
    if lib.clay_add_item(doc, layer, ctypes.byref(item), None) != 1:
        errors.append("clay_add_item accepted a struct_size below the original layout")
    item.struct_size = 0x3D23D70A  # the bits of 0.04f: not a struct size
    if lib.clay_add_item(doc, layer, ctypes.byref(item), None) != 1:
        errors.append("clay_add_item accepted a struct_size no descriptor could have")
    item.struct_size = ctypes.sizeof(ItemDesc)

    # a caller from a later header: the fields this build cannot name are
    # ignored rather than misread
    future = FutureItemDesc()
    ctypes.memmove(ctypes.byref(future), ctypes.byref(item), ctypes.sizeof(ItemDesc))
    future.struct_size = ctypes.sizeof(FutureItemDesc)
    for i in range(4):
        future.appended[i] = 1234.5
    as_known = ctypes.cast(ctypes.byref(future), ctypes.POINTER(ItemDesc))
    if lib.clay_add_item(doc, layer, as_known, None) != 0:
        errors.append("clay_add_item rejected a struct_size from a later header")

    # a caller from the previous ABI: every 0.1.0 prim value puts a small
    # integer where struct_size now is, and none of them is a declared layout
    for prim in range(14):  # CLAY_PRIM_SPHERE .. CLAY_PRIM_PYRAMID, all of 0.1.0
        old = ItemDescV0_1_0()
        old.prim = prim
        # a sphere of radius 0 is the case a shifted read waves through: the
        # old params[0] lands where prim now is
        old.params[0] = 1.0 if prim else 0.0
        old.rotation[3] = 1.0
        old.scale = 1.0
        as_new = ctypes.cast(ctypes.byref(old), ctypes.POINTER(ItemDesc))
        if lib.clay_add_item(doc, layer, as_new, None) != 1:
            errors.append(f"clay_add_item read an ABI 0.1.0 descriptor (prim {prim}) "
                          f"instead of rejecting it")
    return errors


def declared_prims() -> list[int]:
    """clay_prim as a bindings generator parses it out of the header."""
    text = (REPO / "bindings" / "c" / "clay.h").read_text()
    body = re.search(r"typedef enum clay_prim\s*{(.*?)}\s*clay_prim;", text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [int(value) for _, value in re.findall(r"(CLAY_PRIM_\w+)\s*=\s*(\d+)", body)]


def prim_sweep(lib) -> list[str]:
    """Every primitive the header declares is one the library builds, and no
    other value is: the enum and the binary cannot drift apart unnoticed."""
    # the first two values are a unit sine and cosine, which is what the
    # angle primitives take
    probe = (ctypes.c_float * 7)(0.6, 0.8, *([0.5] * 5))
    accepted = set()
    for prim in range(64):
        for count in range(8):
            handle = lib.clay_item_create(prim, probe, count)
            if handle:
                lib.clay_item_destroy(handle)
                accepted.add(prim)
                break
    declared = set(declared_prims())
    if accepted == declared:
        return []
    return [f"clay_prim disagrees with the library: declared but rejected "
            f"{sorted(declared - accepted)}, accepted but undeclared {sorted(accepted - declared)}"]


def builder_exercise(lib, doc, layer: int) -> list[str]:
    """The item builder: a composed edit, driven the way a generated binding would."""
    errors = []
    params = (ctypes.c_float * 3)(0.4, 0.9, 0.4)
    item = lib.clay_item_create(1, params, 3)  # CLAY_PRIM_BOX
    if not item:
        return ["clay_item_create rejected a box"]
    if lib.clay_item_create(1, params, 2):
        errors.append("clay_item_create accepted the wrong parameter count")
    twist = (ctypes.c_float * 1)(0.8)
    if lib.clay_item_add_deformer(item, 0, twist, 1, 0) != 0:  # CLAY_DEFORM_TWIST
        errors.append("clay_item_add_deformer rejected a twist")
    if lib.clay_item_set_repeat_radial(item, 1, 0.0) != 1:
        errors.append("clay_item_set_repeat_radial accepted a count below 2")
    if lib.clay_item_set_repeat_radial(item, 6, 1.2) != 0:
        errors.append("clay_item_set_repeat_radial rejected a valid array")
    node = ctypes.c_uint32(0)
    if lib.clay_layer_add_item(doc, layer, item, ctypes.byref(node)) != 0 or node.value == 0:
        errors.append("clay_layer_add_item failed")
    lib.clay_item_destroy(item)

    # variable-length payload: the builder copies the caller's buffer, and
    # nothing caps its length
    stroke = lib.clay_item_create(14, None, 0)  # CLAY_PRIM_STROKE
    points = (ctypes.c_float * 8)(-0.5, 0, 0, 0.2, 0.5, 0, 0, 0.1)
    if lib.clay_item_set_stroke_points(stroke, points, 2) != 0:
        errors.append("clay_item_set_stroke_points rejected a two-point chain")
    long_chain = [v for i in range(4096)
                  for v in (-1.0 + 2.0 * i / 4095.0, 0.0, 0.0, 0.1)]
    if lib.clay_item_set_stroke_points(stroke, (ctypes.c_float * len(long_chain))(*long_chain),
                                       4096) != 0:
        errors.append("clay_item_set_stroke_points rejected a 4096-point chain")
    if lib.clay_layer_add_item(doc, layer, stroke, None) != 0:
        errors.append("clay_layer_add_item rejected a stroke")
    lib.clay_item_destroy(stroke)
    lib.clay_item_destroy(None)  # releasing a null handle is a no-op

    # the field the long chain describes: a rod of radius 0.1 along x
    at_axis = (ctypes.c_float * 3)(0.0, 0.0, 0.0)
    distance = ctypes.c_float(99.0)
    if lib.clay_eval_points(doc, None, at_axis, 1, ctypes.byref(distance), None) != 0:
        errors.append("clay_eval_points failed on the builder's document")
    elif distance.value > 0.0:
        errors.append(f"the 4096-point stroke reads {distance.value} inside itself")
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
    lib.clay_add_item.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ItemDesc),
                                  ctypes.POINTER(ctypes.c_uint32)]
    lib.clay_item_create.argtypes = [ctypes.c_int32, ctypes.POINTER(ctypes.c_float),
                                     ctypes.c_size_t]
    lib.clay_item_create.restype = ctypes.c_void_p
    lib.clay_item_destroy.argtypes = [ctypes.c_void_p]
    lib.clay_item_add_deformer.argtypes = [ctypes.c_void_p, ctypes.c_int32,
                                           ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                                           ctypes.c_int32]
    lib.clay_item_set_repeat_radial.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_float]
    lib.clay_item_set_stroke_points.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float),
                                                ctypes.c_size_t]
    lib.clay_layer_add_item.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                                        ctypes.POINTER(ctypes.c_uint32)]
    lib.clay_eval_points.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                     ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.c_float),
                                     ctypes.POINTER(ctypes.c_float)]

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
        else:
            errors += struct_size_exercise(lib, doc, layer.value)
            errors += builder_exercise(lib, doc, layer.value)
            errors += prim_sweep(lib)
        lib.clay_document_destroy(doc)
        lib.clay_document_destroy(None)  # releasing a null handle is a no-op
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

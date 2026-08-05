#!/usr/bin/env python3
"""FFI hygiene check for the C ABI (c-abi spec: bindgen-clean).

1. Lexical rules on clay.h: no variadics, no bitfields, no bare long —
   the patterns that break Swift/C#/Rust bindings generators — and a leading
   uint32_t struct_size on every public descriptor struct, so fields can be
   appended without a major bump.
2. Every function clay.h declares resolves in the shared library: a
   declaration without a definition is a link error for every generated
   binding, and nothing else in the repository compares the two.
3. A real cross-language FFI exercise: load the shared library with Python
   ctypes (a bindings generator equivalent) and drive the core flow,
   including the struct_size prefix rule and the item builder.
"""

import ctypes
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


# Structs that are array ELEMENTS rather than versioned descriptors. A
# descriptor is an input a caller fills in and may have compiled against an
# older layout, which is what struct_size negotiates. An element type is a
# fixed binary layout that a caller passes thousands of, exactly like the
# int32_t[3] a cell crosses as: a struct_size per element would be absurd, and
# changing the layout is a break rather than something to negotiate. Naming
# them here keeps that a decision rather than an oversight.
ARRAY_ELEMENT_STRUCTS = {
    "clay_stroke_sample",  # packed float[5] per sample, passed as a bare float*
    "clay_stamp",          # packed float[10] per stamp, an output buffer
}


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
        if name in ARRAY_ELEMENT_STRUCTS:
            if re.search(r"\bstruct_size\b", body):
                errors.append(f"{name} is listed as an array element type but has a "
                              f"struct_size; it is a descriptor, so drop the exemption")
            continue
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


def declared_functions() -> list[str]:
    """Every clay_* function clay.h declares, as a bindings generator reads it."""
    text = (REPO / "bindings" / "c" / "clay.h").read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return sorted(set(re.findall(r"\b(clay_\w+)\s*\(", text)))


def exports_exercise(lib) -> list[str]:
    """The header's declarations are claims about the binary, so check them.

    The parity gate reads clay.h alone, so a prototype with no definition
    satisfies it — and a rename that touches only one of the two files ships a
    header every bindings generator turns into an unresolved-symbol link
    error, with CI green. Resolving each name is what makes the header's
    claim an ABI-level one.
    """
    return [f"clay.h declares {name}, the library does not export it"
            for name in declared_functions() if not hasattr(lib, name)]


def declared_enum(name: str) -> list[int]:
    """An enumeration as a bindings generator parses it out of the header."""
    text = (REPO / "bindings" / "c" / "clay.h").read_text()
    body = re.search(r"typedef enum %s\s*{(.*?)}\s*%s;" % (name, name), text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [int(value) for _, value in re.findall(r"(CLAY_\w+)\s*=\s*(\d+)", body)]


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
    declared = set(declared_enum("clay_prim"))
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


class BrushParamsV0_11_0(ctypes.Structure):
    """clay_brush_params as it shipped before the mask field was appended.

    Kept as its own type rather than as a shorter struct_size on the current
    one: this is what a caller built against 0.11.0 actually passes, and the
    prefix rule has to zero-fill the mask for it rather than read past its end.
    """

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("size", ctypes.c_int32),
        ("shape", ctypes.c_int32),
        ("falloff", ctypes.c_int32),
        ("strength", ctypes.c_float),
        ("seed", ctypes.c_uint32),
    ]


class BrushParams(ctypes.Structure):
    """clay_brush_params as a bindings generator would emit it."""

    _fields_ = BrushParamsV0_11_0._fields_ + [("mask", ctypes.c_void_p)]


class FutureBrushParams(ctypes.Structure):
    """clay_brush_params as a caller compiled against a later header sees it.

    Appended AFTER the current layout, which is the only thing a later header
    can do. Modelling it any other way would have the boundary read a field
    the caller never wrote — and since `mask` is a pointer, that is a wild
    dereference rather than a wrong number.
    """

    _fields_ = BrushParams._fields_ + [("appended", ctypes.c_float * 2)]


def cells(*values: int):
    return (ctypes.c_int32 * len(values))(*values)


def brush(size: int, shape: int = 0, falloff: int = 0, strength: float = 1.0,
          seed: int = 0) -> BrushParams:
    p = BrushParams()
    p.struct_size = ctypes.sizeof(BrushParams)
    p.size, p.shape, p.falloff, p.strength, p.seed = size, shape, falloff, strength, seed
    return p


def brush_struct_size_exercise(lib, grid) -> list[str]:
    """The versioned-descriptor contract on the descriptor voxels added."""
    errors = []
    at = cells(0, 0, 0)
    p = brush(3)
    p.struct_size = 0  # setting it is mandatory, exactly as for clay_item_desc
    if lib.clay_voxel_set_brush(grid, at, ctypes.byref(p), 1) != 1:
        errors.append("clay_voxel_set_brush accepted a brush that declares no struct_size")
    p.struct_size = ctypes.sizeof(BrushParams)
    if lib.clay_voxel_set_brush(grid, at, ctypes.byref(p), 1) != 0:
        errors.append("clay_voxel_set_brush rejected an explicit struct_size")
    p.struct_size = 4  # shorter than the layout clay_brush_params shipped with
    if lib.clay_voxel_set_brush(grid, at, ctypes.byref(p), 1) != 1:
        errors.append("clay_voxel_set_brush accepted a struct_size below the original layout")
    p.struct_size = 0x3D23D70A  # the bits of 0.04f: not a struct size
    if lib.clay_voxel_set_brush(grid, at, ctypes.byref(p), 1) != 1:
        errors.append("clay_voxel_set_brush accepted a struct_size no descriptor could have")

    future = FutureBrushParams()
    ctypes.memmove(ctypes.byref(future), ctypes.byref(brush(3)), ctypes.sizeof(BrushParams))
    future.struct_size = ctypes.sizeof(FutureBrushParams)
    future.appended[0], future.appended[1] = 1234.5, -7.0
    as_known = ctypes.cast(ctypes.byref(future), ctypes.POINTER(BrushParams))
    if lib.clay_voxel_set_brush(grid, at, as_known, 1) != 0:
        errors.append("clay_voxel_set_brush rejected a struct_size from a later header")

    # ...and the other direction: a caller built against 0.11.0, before the
    # mask field existed. The prefix rule zero-fills it, so the stamp means
    # exactly what it always meant.
    old = BrushParamsV0_11_0()
    old.struct_size = ctypes.sizeof(BrushParamsV0_11_0)
    old.size, old.strength = 3, 1.0
    as_current = ctypes.cast(ctypes.byref(old), ctypes.POINTER(BrushParams))
    if lib.clay_voxel_set_brush(grid, at, as_current, 1) != 0:
        errors.append("clay_voxel_set_brush rejected a pre-0.12.0 brush descriptor")
    return errors


def brush_sweep(lib, grid) -> list[str]:
    """Every brush shape and falloff the header declares is one the library
    stamps, and no other value is."""
    errors = []
    at = cells(8, 8, 8)
    for enum, field in (("clay_brush_shape", "shape"), ("clay_brush_falloff", "falloff")):
        accepted = set()
        for value in range(-1, 8):
            p = brush(3)
            setattr(p, field, value)
            if lib.clay_voxel_set_brush(grid, at, ctypes.byref(p), 1) == 0:
                accepted.add(value)
        declared = set(declared_enum(enum))
        if accepted != declared:
            errors.append(f"{enum} disagrees with the library: declared but rejected "
                          f"{sorted(declared - accepted)}, accepted but undeclared "
                          f"{sorted(accepted - declared)}")
    return errors


def flood_select_exercise(lib, grid, occupied: int) -> list[str]:
    """The size-query pattern on a selection only the library can measure."""
    errors = []
    seed = cells(0, 0, 0)
    count = ctypes.c_size_t(0)
    if lib.clay_voxel_flood_select(grid, seed, 1, None, ctypes.byref(count)) != 0:
        return ["clay_voxel_flood_select size query failed"]
    if count.value != occupied:
        errors.append(f"the flood select reports {count.value} cells, the grid holds "
                      f"{occupied}")
    needed = count.value
    short = ctypes.c_size_t(needed - 1)
    buf = (ctypes.c_int32 * (needed * 3))()
    if lib.clay_voxel_flood_select(grid, seed, 1, buf, ctypes.byref(short)) != 3:
        errors.append("clay_voxel_flood_select accepted a buffer one cell too small")
    elif short.value != needed:
        errors.append(f"a short flood select reports {short.value} needed, not {needed}")
    filled = ctypes.c_size_t(needed)
    if lib.clay_voxel_flood_select(grid, seed, 1, buf, ctypes.byref(filled)) != 0:
        errors.append("clay_voxel_flood_select rejected an adequate buffer")
    elif filled.value != needed:
        errors.append(f"the filling call wrote {filled.value} cells, not {needed}")
    elif tuple(buf[0:3]) != (0, 0, 0):
        errors.append(f"the flood select starts at {tuple(buf[0:3])}, not the seed")
    index = ctypes.c_int32(0)
    for i in range(needed):
        cell = (ctypes.c_int32 * 3)(*buf[i * 3:i * 3 + 3])
        if lib.clay_voxel_get(grid, cell, ctypes.byref(index)) != 0 or index.value == 0:
            errors.append(f"the flood select returned empty cell {tuple(cell)}")
            break
    return errors


def stamp_exercise(lib, grid, sphere) -> tuple[list[str], int]:
    """A palette entry and a sphere stamp: what everything below works on."""
    errors = []
    index = ctypes.c_int32(0)
    rgb = (ctypes.c_float * 3)(0.9, 0.3, 0.2)
    if lib.clay_voxel_palette_add(grid, rgb, ctypes.byref(index)) != 0 or index.value != 1:
        errors.append(f"clay_voxel_palette_add returned index {index.value}, not 1")
    if lib.clay_voxel_set_brush(grid, cells(0, 0, 0), ctypes.byref(sphere), index) != 0:
        errors.append("clay_voxel_set_brush rejected a sphere stamp")
    occupied = ctypes.c_size_t(0)
    lib.clay_voxel_occupied_count(grid, ctypes.byref(occupied))
    if occupied.value == 0:
        errors.append("a sphere stamp left the grid empty")
    return errors, occupied.value


def sculpt_exercise(lib, grid, sphere, occupied: int) -> list[str]:
    """The bounds of the stamp, a sculpting verb over it, and its greedy mesh."""
    errors = []
    lo, hi, has = (ctypes.c_int32 * 3)(), (ctypes.c_int32 * 3)(), ctypes.c_int32(0)
    if lib.clay_voxel_bounds(grid, lo, hi, ctypes.byref(has)) != 0 or has.value != 1:
        errors.append("clay_voxel_bounds reported no bounds for a stamped grid")
    elif tuple(lo) != (-2, -2, -2) or tuple(hi) != (2, 2, 2):
        errors.append(f"a size-5 stamp at the origin spans {tuple(lo)}..{tuple(hi)}, "
                      f"not (-2,-2,-2)..(2,2,2)")

    after = ctypes.c_size_t(0)
    if lib.clay_voxel_sculpt_smooth(grid, cells(0, 0, 0), ctypes.byref(sphere)) != 0:
        errors.append("clay_voxel_sculpt_smooth rejected a valid brush")
    lib.clay_voxel_occupied_count(grid, ctypes.byref(after))
    if after.value == occupied:
        errors.append(f"smoothing a size-5 ball changed nothing (still {occupied} cells)")

    mesh = ctypes.c_void_p(0)
    if lib.clay_voxel_mesh(grid, ctypes.byref(mesh)) != 0 or not mesh.value:
        errors.append("clay_voxel_mesh failed on a stamped grid")
    elif lib.clay_mesh_vertex_count(mesh) == 0:
        errors.append("the greedy mesh of a stamped grid has no vertices")
    lib.clay_mesh_destroy(mesh)
    return errors


def voxel_exercise(lib) -> list[str]:
    """A standalone grid: palette, a brush stamp, a sculpt verb, the size-query
    selection and a greedy mesh, driven the way a generated binding would."""
    grid = lib.clay_voxel_grid_create(0.1)
    if not grid:
        return ["clay_voxel_grid_create rejected a valid voxel size"]
    errors = []
    if lib.clay_voxel_grid_create(0.0):
        errors.append("clay_voxel_grid_create accepted a voxel size of 0")

    sphere = brush(5, shape=1)  # CLAY_BRUSH_SHAPE_SPHERE
    stamped, occupied = stamp_exercise(lib, grid, sphere)
    errors += stamped
    if occupied:
        errors += flood_select_exercise(lib, grid, occupied)
        errors += sculpt_exercise(lib, grid, sphere, occupied)
    errors += brush_struct_size_exercise(lib, grid)
    errors += brush_sweep(lib, grid)
    if lib.clay_voxel_grid_destroy(grid) != 0:
        errors.append("clay_voxel_grid_destroy refused a grid the caller owns")
    if lib.clay_voxel_grid_destroy(None) != 1:
        errors.append("clay_voxel_grid_destroy accepted a null handle")
    return errors


def voxel_ownership_exercise(lib, doc) -> list[str]:
    """A grid borrowed from a document layer is the document's: destroying it
    is refused, and the document keeps working afterwards."""
    errors = []
    layer, grid = ctypes.c_uint32(0), ctypes.c_void_p(0)
    if lib.clay_document_add_voxel_layer(doc, b"clay", 0.1, ctypes.byref(layer),
                                         ctypes.byref(grid)) != 0 or not grid.value:
        return ["clay_document_add_voxel_layer failed"]
    if lib.clay_voxel_set(grid, cells(1, 2, 3), 1) != 0:
        errors.append("clay_voxel_set failed on a borrowed grid")
    if lib.clay_voxel_grid_destroy(grid) != 1:
        errors.append("clay_voxel_grid_destroy obeyed a borrowed handle instead of refusing it")

    again = ctypes.c_void_p(0)
    if lib.clay_document_voxel_layer(doc, b"clay", None, ctypes.byref(again)) != 0:
        errors.append("the voxel layer is gone after a refused destroy")
    elif again.value != grid.value:
        errors.append("looking the same voxel layer up twice returned two handles")
    index = ctypes.c_int32(0)
    if lib.clay_voxel_get(grid, cells(1, 2, 3), ctypes.byref(index)) != 0 or index.value != 1:
        errors.append("the refused destroy lost the edit that was already made")
    return errors


def ffi_exercise(lib_path: str) -> list[str]:
    lib = ctypes.CDLL(lib_path)
    errors = exports_exercise(lib)

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

    cell_p = ctypes.POINTER(ctypes.c_int32)
    brush_p = ctypes.POINTER(BrushParams)
    lib.clay_voxel_grid_create.argtypes = [ctypes.c_float]
    lib.clay_voxel_grid_create.restype = ctypes.c_void_p
    lib.clay_voxel_grid_destroy.argtypes = [ctypes.c_void_p]
    lib.clay_document_add_voxel_layer.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                                  ctypes.c_float,
                                                  ctypes.POINTER(ctypes.c_uint32),
                                                  ctypes.POINTER(ctypes.c_void_p)]
    lib.clay_document_voxel_layer.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                              ctypes.POINTER(ctypes.c_uint32),
                                              ctypes.POINTER(ctypes.c_void_p)]
    lib.clay_voxel_palette_add.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float),
                                           ctypes.POINTER(ctypes.c_int32)]
    lib.clay_voxel_get.argtypes = [ctypes.c_void_p, cell_p, ctypes.POINTER(ctypes.c_int32)]
    lib.clay_voxel_set.argtypes = [ctypes.c_void_p, cell_p, ctypes.c_int32]
    lib.clay_voxel_set_brush.argtypes = [ctypes.c_void_p, cell_p, brush_p, ctypes.c_int32]
    lib.clay_voxel_sculpt_smooth.argtypes = [ctypes.c_void_p, cell_p, brush_p]
    lib.clay_voxel_occupied_count.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_voxel_bounds.argtypes = [ctypes.c_void_p, cell_p, cell_p,
                                      ctypes.POINTER(ctypes.c_int32)]
    lib.clay_voxel_flood_select.argtypes = [ctypes.c_void_p, cell_p, ctypes.c_int32, cell_p,
                                            ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_voxel_mesh.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    lib.clay_mesh_vertex_count.argtypes = [ctypes.c_void_p]
    lib.clay_mesh_vertex_count.restype = ctypes.c_size_t
    lib.clay_mesh_destroy.argtypes = [ctypes.c_void_p]

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
            errors += voxel_exercise(lib)
            errors += voxel_ownership_exercise(lib, doc)
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

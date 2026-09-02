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
    # The same, wider: the channels a tablet reports (azimuth, velocity,
    # timestamp) beside the five that were already there. An element for the
    # same reason its narrower sibling is — a caller passes thousands of them
    # and the layout is the contract — which is also precisely why it is a
    # SECOND struct rather than three fields appended to the first: appending
    # would move every element after the first under every host already
    # compiled against it, and an element type has no struct_size to negotiate
    # that with. Widening this one later is a break in exactly the same way.
    "clay_stroke_sample_full",
    "clay_stamp",          # packed float[10] per stamp, an output buffer
    # One per changed brick of a live Smooth preview, and a dab produces
    # hundreds. Read rather than filled in, like the two brick-range elements
    # below, so there is nothing for a struct_size to negotiate: appending a
    # field would move every element after the first, which is a break either
    # way. The host reads it straight into an upload loop.
    "clay_sdf_preview_brick",
    # 44 bytes that ARE brick::BrickRequest, asserted field by field with
    # offsetof in bindings/c/clay_c.cpp. A refill hands out thousands at once
    # and the drain is a single memcpy out of the engine's own vector; a
    # struct_size per element would forbid that copy and negotiate a layout
    # whose whole point is that it is fixed.
    "clay_brick_request",
    # One per brick key in a subset mesh, and a host re-meshing a dirty set
    # receives thousands at a time. Like clay_brick_request it is a fixed
    # binary layout the caller reads rather than fills in, so there is nothing
    # for a struct_size to negotiate: appending a field would move every
    # element after the first, which is a break either way.
    "clay_brick_mesh_range",
    # The voxel side of the same thing: one per chunk key in a regional mesh,
    # read rather than filled in, and a host patching a dab's worth of chunks
    # receives one per key every frame. Same fixed layout, same reasoning.
    "clay_voxel_chunk_mesh_range",
    # Two uint32 that ARE kernel::CTapeInstr, asserted with offsetof in
    # bindings/c/clay_c.cpp. The caller's evaluator is ctape_eval compiled from
    # the header that declares it, so the layout agreeing is the contract
    # rather than a convenience, and a struct_size would negotiate a layout
    # whose whole point is that it is fixed.
    "clay_tape_instr",
    # The four chunk counters, and one chunk's record. A host draining a stroke
    # passes one clay_chunk_revisions per acknowledged chunk and reads a
    # clay_chunk_info per chunk of the surface — thousands of each, filled in
    # one bulk call — so the layout is the contract and a struct_size per
    # element would forbid the bulk fill that is the point. Appending a field to
    # either moves every element after the first, which is a break either way;
    # the versioned half of this transport is clay_chunk_readback, which is one
    # per call and carries a struct_size.
    "clay_chunk_revisions",
    "clay_chunk_info",
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


# Entry points whose output descriptor is filled through a shared helper rather
# than in the function body, so the body-scan below cannot see the bounded
# write. Each is checked by tests/unit/test_c_out_descriptors.cpp instead.
BOUNDED_VIA_HELPER = {
    "clay_layer_consolidation_cost",
    "clay_layer_consolidate",
    "clay_layer_consolidation_state",
}

# The bounded-fill helpers. Any of them appearing in a body means the function
# writes through the size the caller declared rather than this build's sizeof.
#
# A named helper is registered here rather than the whole entry point being
# added to BOUNDED_VIA_HELPER above: the gate then still requires SOME bounded
# fill in the body, which is the property it exists to check. Every helper below
# ends in write_desc.
BOUNDED_FILLS = (
    "write_desc",
    "write_preset",
    "write_cost",
    "begin_out_cost",
    "write_voxel_remesh_estimate",
    "write_voxel_remesh_report",
    # The five preflight entry points end here, so they cannot fill the
    # descriptor five different ways.
    "write_preflight",
)


def output_descriptor_fills() -> list[str]:
    """Every OUT descriptor must be filled bounded by the caller's struct_size.

    The prefix rule binds in both directions and only the reading half was ever
    enforced. Writing a whole struct through an out pointer emits sizeof as THIS
    build defines it, so the day a descriptor grows a field, every host compiled
    against the older header has its buffer overwritten past the end — silently,
    and only on the hosts the rule exists to serve, since anything rebuilt is the
    same size we are.

    A grep is not enough, and this gate is the proof of it. Three separate
    spellings of the same bug shipped: `*out = clay_thing{}`, assigning fields
    through `out->`, and `*out = local` — the last of which hid the largest
    overrun of the lot (56 bytes, clay_mesh_brush_defaults) and matched neither
    of the first two patterns. So this walks the public header for entry points
    that take a descriptor by MUTABLE pointer and requires each one's body to
    reach a bounded fill, whatever spelling it would otherwise have used.
    """
    header = (REPO / "bindings" / "c" / "clay.h").read_text()
    source = (REPO / "bindings" / "c" / "clay_c.cpp").read_text()

    # Bounded to each struct's own body. A `.*?` across the whole header spans
    # struct boundaries and calls every later struct a descriptor — which named
    # the array-element types, whose whole point is that they carry NO
    # struct_size, and would have failed the gate on correct code.
    descriptors = {
        name
        for name, body in re.findall(r"typedef struct (\w+)\s*\{([^}]*)\}", header)
        if name not in ARRAY_ELEMENT_STRUCTS
        and re.match(r"\s*uint32_t\s+struct_size\s*;", body)
    }

    errors = []
    for name, args in re.findall(r"clay_result\s+(clay_\w+)\s*\(([^;]*?)\);", header, re.S):
        if name in BOUNDED_VIA_HELPER:
            continue
        flat = " ".join(args.split())
        for desc in sorted(descriptors):
            if not re.search(r"(?<!const )\b%s\s*\*" % desc, flat):
                continue
            found = re.search(r"\bclay_result\s+%s\s*\([^)]*\)\s*\{" % name, source)
            if not found:
                continue
            tail = source[found.end():]
            tail = tail[: tail.find("\n}\n")]
            if any(fill in tail for fill in BOUNDED_FILLS):
                continue
            # An array out-parameter is a different contract — the caller sizes
            # the array, and `out[i].field = ...` is how it is filled — so only
            # a write to the descriptor ITSELF counts here.
            if re.search(r"\*\s*%s\w*\s*=|\*\s*out\w*\s*=" % "out", tail):
                errors.append(f"{name} assigns a whole {desc} through the out pointer, which "
                              f"writes this build's sizeof rather than the size the caller "
                              f"declared; use write_desc")
            elif re.search(r"\bout\w*->\w+\s*=", tail):
                errors.append(f"{name} writes {desc} fields without a bounded fill; "
                              f"use write_desc so an older caller's buffer is not overrun")
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


# Enumerators the header declares that clay_item_create deliberately refuses.
# An entry here is a claim that the VALUE is real — it appears in saved
# documents and in the tape, so a header that hid it would leave a C caller
# staring at an undocumented number — while construction has no producer yet.
# It is a narrow exemption: the value must still round-trip through load,
# evaluate and mesh, which the rest of the suite covers.
DECLARED_BUT_NOT_CONSTRUCTIBLE = {
    "CLAY_PRIM_VOLUME": "built by clay_item_volume_from_mesh, not by "
                        "clay_item_create: a volume needs samples and that entry "
                        "point has none to give, so one built there could only "
                        "ever be a silently empty item",
}


def declared_enum_named(name: str) -> list[tuple[str, int]]:
    """The same enumeration, keeping the enumerator names."""
    text = (REPO / "bindings" / "c" / "clay.h").read_text()
    body = re.search(r"typedef enum %s\s*{(.*?)}\s*%s;" % (name, name), text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [(n, int(v)) for n, v in re.findall(r"(CLAY_\w+)\s*=\s*(\d+)", body)]


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
    exempt = {v for name, v in declared_enum_named("clay_prim")
              if name in DECLARED_BUT_NOT_CONSTRUCTIBLE}
    for name in sorted(DECLARED_BUT_NOT_CONSTRUCTIBLE):
        print(f"c-abi: exempt {name} — {DECLARED_BUT_NOT_CONSTRUCTIBLE[name]}")
    stale = exempt & accepted
    if stale:
        return [f"clay_prim: {sorted(stale)} is listed as not constructible but "
                f"clay_item_create built one: drop it from "
                f"DECLARED_BUT_NOT_CONSTRUCTIBLE in tools/check_c_abi.py"]
    if accepted == declared - exempt:
        return []
    declared = declared - exempt
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


class BrickConfig(ctypes.Structure):
    """clay_brick_config as a bindings generator would emit it."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("dim", ctypes.c_int32),
        ("voxel_size", ctypes.c_float),
        ("band_voxels", ctypes.c_int32),
        ("memory_budget", ctypes.c_uint64),
        # appended after the original layout: an RGBA8 lattice beside the
        # distances, so a host can upload a colour atlas without meshing
        ("colors", ctypes.c_int32),
    ]


class BrickStats(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("tracked_bricks", ctypes.c_uint64),
        ("surface_bricks", ctypes.c_uint64),
        ("dirty_bricks", ctypes.c_uint64),
        ("memory_usage", ctypes.c_uint64),
        ("memory_budget", ctypes.c_uint64),
    ]


class BrickRequest(ctypes.Structure):
    """clay_brick_request: an array ELEMENT, so no struct_size to negotiate.

    Spelled out because this is the layout the drain memcpys into a caller's
    buffer; if it ever stops being brick::BrickRequest, the C++ side's
    offsetof assertions fail and so does the flow below.
    """

    _fields_ = [
        ("key", ctypes.c_int32 * 3),
        ("generation", ctypes.c_uint32),
        ("origin", ctypes.c_float * 3),
        ("spacing", ctypes.c_float),
        ("dims", ctypes.c_int32 * 3),
        ("band", ctypes.c_float),
    ]


class TapeInstr(ctypes.Structure):
    """clay_tape_instr: an array ELEMENT that IS kernel::CTapeInstr."""

    _fields_ = [("op", ctypes.c_uint32), ("param_offset", ctypes.c_uint32)]


class BrickMeshRange(ctypes.Structure):
    """clay_brick_mesh_range: an array ELEMENT, one per key in a subset mesh."""

    _fields_ = [
        ("key", ctypes.c_int32 * 3),
        ("vertex_first", ctypes.c_uint32),
        ("vertex_count", ctypes.c_uint32),
        ("index_first", ctypes.c_uint32),
        ("index_count", ctypes.c_uint32),
    ]


class VertexLayout(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("stride", ctypes.c_uint32),
        ("position_offset", ctypes.c_int32),
        ("normal_offset", ctypes.c_int32),
        ("color_offset", ctypes.c_int32),
        ("uv_offset", ctypes.c_int32),
    ]


class BrickMeshParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("normals", ctypes.c_int32),
        ("colors", ctypes.c_int32),
        ("gradient_eps", ctypes.c_float),
    ]


def parity_fixture_exercise(lib) -> list[str]:
    """The host parity fixture through the size-query pattern.

    Issue #51 item C: an iOS test target links the framework and cannot shell
    out to `clay parity-fixture`, so the fixture had to become reachable from
    the ABI for "the preview agrees with the field" to be a test a host runs
    rather than a property it hopes for.
    """
    errors = []
    szp = ctypes.POINTER(ctypes.c_size_t)
    lib.clay_parity_fixture_json.argtypes = [ctypes.c_char_p, szp]
    n = ctypes.c_size_t(0)
    if lib.clay_parity_fixture_json(None, ctypes.byref(n)) != 0 or n.value < 1024:
        return [f"clay_parity_fixture_json size query returned {n.value}"]
    buf = ctypes.create_string_buffer(n.value)
    cap = ctypes.c_size_t(n.value)
    if lib.clay_parity_fixture_json(buf, ctypes.byref(cap)) != 0:
        errors.append("clay_parity_fixture_json rejected an adequate buffer")
    text = buf.value.decode()
    # "march" and "rays" are schema 2: the half that says how a host must TRACE
    # the field, not just evaluate it. A build that stopped exporting them would
    # still pass every point comparison.
    for key in ('"cases"', '"instrs"', '"safe_step_scale"', '"tolerance"', '"march"', '"rays"'):
        if key not in text:
            errors.append(f"the parity fixture is missing {key}")
    # a short buffer is refused AND reports what was needed, so the retry is
    # one call rather than a doubling loop
    short = ctypes.c_size_t(n.value - 1)
    if lib.clay_parity_fixture_json(buf, ctypes.byref(short)) != 3:  # BUFFER_TOO_SMALL
        errors.append("clay_parity_fixture_json accepted a short buffer")
    elif short.value != n.value:
        errors.append("a refused parity fixture call did not report the size needed")
    return errors


def tape_export_exercise(lib) -> list[str]:
    """The compiled tape across the boundary, as a generated binding sees it.

    The values are checked in C++ (tests/unit/test_c_tape_export.cpp evaluates
    the export through ctape_eval). What this adds is the FFI shape: an opaque
    handle a generated binding releases, borrowed buffers with out-counts, and
    the lifetime rule — an edit must not invalidate an export.
    """
    errors = []
    instr_p = ctypes.POINTER(TapeInstr)
    szp = ctypes.POINTER(ctypes.c_size_t)
    fp = ctypes.POINTER(ctypes.c_float)
    lib.clay_tape_export.argtypes = [ctypes.c_void_p, fp, fp, ctypes.POINTER(ctypes.c_void_p)]
    lib.clay_tape_release.argtypes = [ctypes.c_void_p]
    lib.clay_tape_encoding_version.restype = ctypes.c_uint32
    lib.clay_tape_instrs.argtypes = [ctypes.c_void_p, szp]
    lib.clay_tape_instrs.restype = instr_p
    lib.clay_tape_params.argtypes = [ctypes.c_void_p, szp]
    lib.clay_tape_params.restype = fp
    lib.clay_tape_blob.argtypes = [ctypes.c_void_p, szp]
    lib.clay_tape_blob.restype = fp
    lib.clay_tape_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32), fp, fp,
                                   fp, fp, ctypes.POINTER(ctypes.c_uint64)]

    doc = lib.clay_document_create()
    layer = ctypes.c_uint32(0)
    if lib.clay_add_sdf_layer(doc, b"tape", ctypes.byref(layer)) != 0:
        lib.clay_document_destroy(doc)
        return ["clay_add_sdf_layer failed for the tape exercise"]
    item = ItemDesc()
    item.struct_size = ctypes.sizeof(ItemDesc)
    item.prim = 0  # CLAY_PRIM_SPHERE
    item.params[0] = 0.4
    node = ctypes.c_uint32(0)
    if lib.clay_add_item(doc, layer.value, ctypes.byref(item), ctypes.byref(node)) != 0:
        lib.clay_document_destroy(doc)
        return ["clay_add_item failed for the tape exercise"]

    tape = ctypes.c_void_p(0)
    if lib.clay_tape_export(doc, None, None, ctypes.byref(tape)) != 0 or not tape.value:
        lib.clay_document_destroy(doc)
        return ["clay_tape_export failed on a plain document"]
    n = ctypes.c_size_t(0)
    if not lib.clay_tape_instrs(tape, ctypes.byref(n)) or n.value == 0:
        errors.append("clay_tape_instrs returned nothing for a non-empty document")
    if lib.clay_tape_params(tape, ctypes.byref(n)) is None or n.value == 0:
        errors.append("clay_tape_params returned nothing for a non-empty document")
    lib.clay_tape_blob(tape, ctypes.byref(n))  # 0 is legitimate: no out-of-line payload

    exact, lip, step = ctypes.c_int32(-1), ctypes.c_float(0), ctypes.c_float(0)
    lo, hi = (ctypes.c_float * 3)(), (ctypes.c_float * 3)()
    rev = ctypes.c_uint64(0)
    if lib.clay_tape_info(tape, ctypes.byref(exact), ctypes.byref(lip), ctypes.byref(step),
                          lo, hi, ctypes.byref(rev)) != 0:
        errors.append("clay_tape_info failed")
    elif step.value <= 0.0 or step.value > 1.0 or rev.value == 0:
        errors.append(f"clay_tape_info reports step={step.value} revision={rev.value}")
    if lib.clay_tape_encoding_version() == 0:
        errors.append("clay_tape_encoding_version returned 0")

    # the lifetime rule: an edit installs a new tape and leaves this one alone
    before = ctypes.c_size_t(0)
    lib.clay_tape_instrs(tape, ctypes.byref(before))
    for _ in range(4):
        lib.clay_add_item(doc, layer.value, ctypes.byref(item), ctypes.byref(node))
    after_edit = ctypes.c_size_t(0)
    if not lib.clay_tape_instrs(tape, ctypes.byref(after_edit)):
        errors.append("an export stopped being readable after an edit")
    elif after_edit.value != before.value:
        errors.append("an export changed under an edit: it is not a snapshot")
    fresh = ctypes.c_void_p(0)
    if lib.clay_tape_export(doc, None, None, ctypes.byref(fresh)) == 0:
        grown = ctypes.c_size_t(0)
        lib.clay_tape_instrs(fresh, ctypes.byref(grown))
        if grown.value <= before.value:
            errors.append("a re-export after four added items did not grow the tape")
        new_rev = ctypes.c_uint64(0)
        lib.clay_tape_info(fresh, None, None, None, None, None, ctypes.byref(new_rev))
        if new_rev.value == rev.value:
            errors.append("the revision did not change across an edit")
    lib.clay_tape_release(fresh)

    # the cull region follows clay_eval_grid's rules
    region_lo = (ctypes.c_float * 3)(-1, -1, -1)
    region_hi = (ctypes.c_float * 3)(1, 1, 1)
    culled = ctypes.c_void_p(0)
    if lib.clay_tape_export(doc, region_lo, region_hi, ctypes.byref(culled)) != 0:
        errors.append("clay_tape_export rejected a valid cull region")
    lib.clay_tape_release(culled)
    if lib.clay_tape_export(doc, region_lo, None, ctypes.byref(culled)) != 1:
        errors.append("clay_tape_export accepted one bound without the other")

    lib.clay_tape_release(tape)
    lib.clay_tape_release(None)  # releasing a null handle is a no-op
    lib.clay_document_destroy(doc)
    return errors


def brick_types(lib) -> None:
    """argtypes for the brick surface, as a generated binding would emit."""
    u32p = ctypes.POINTER(ctypes.c_uint32)
    szp = ctypes.POINTER(ctypes.c_size_t)
    i32p = ctypes.POINTER(ctypes.c_int32)
    fp = ctypes.POINTER(ctypes.c_float)
    lib.clay_brick_config_defaults.argtypes = [ctypes.POINTER(BrickConfig)]
    lib.clay_brick_cache_create.argtypes = [ctypes.POINTER(BrickConfig)]
    lib.clay_brick_cache_create.restype = ctypes.c_void_p
    lib.clay_brick_cache_destroy.argtypes = [ctypes.c_void_p]
    lib.clay_brick_cache_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(BrickStats)]
    lib.clay_brick_cache_config.argtypes = [ctypes.c_void_p, ctypes.POINTER(BrickConfig)]
    lib.clay_brick_cache_mark_dirty.argtypes = [ctypes.c_void_p, fp, fp]
    lib.clay_brick_cache_mark_dirty_layer.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                                      ctypes.c_uint32]
    lib.clay_brick_cache_mark_dirty_nodes.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                                      ctypes.c_uint32, u32p, ctypes.c_size_t,
                                                      szp]
    lib.clay_brick_cache_take_dirty.argtypes = [ctypes.c_void_p, ctypes.POINTER(BrickRequest),
                                                szp, szp]
    lib.clay_brick_cache_eval_requests.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                                   ctypes.POINTER(BrickRequest),
                                                   ctypes.c_size_t, fp, ctypes.c_size_t,
                                                   fp, ctypes.c_size_t]
    lib.clay_brick_cache_submit.argtypes = [ctypes.c_void_p, ctypes.POINTER(BrickRequest),
                                            ctypes.c_size_t, fp, ctypes.c_size_t,
                                            fp, ctypes.c_size_t, i32p, szp]
    lib.clay_brick_cache_surface_bricks.argtypes = [ctypes.c_void_p, i32p, szp]
    lib.clay_brick_cache_read_bricks.argtypes = [ctypes.c_void_p, ctypes.c_int32, i32p,
                                                 ctypes.c_size_t, ctypes.c_int32, i32p,
                                                 ctypes.POINTER(ctypes.c_uint16),
                                                 ctypes.c_size_t,
                                                 ctypes.POINTER(ctypes.c_uint8),
                                                 ctypes.c_size_t]
    lib.clay_brick_cache_mesh.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                          ctypes.POINTER(BrickMeshParams), i32p, ctypes.c_size_t,
                                          ctypes.POINTER(BrickMeshRange),
                                          ctypes.POINTER(ctypes.c_void_p)]
    lib.clay_brick_cache_raycast_many.argtypes = [ctypes.c_void_p, fp, ctypes.c_size_t, i32p,
                                                  fp, fp, fp]
    lib.clay_mesh_copy_vertices.argtypes = [ctypes.c_void_p, ctypes.POINTER(VertexLayout),
                                            ctypes.c_void_p, ctypes.c_size_t]
    lib.clay_mesh_copy_indices.argtypes = [ctypes.c_void_p, u32p, ctypes.c_size_t]
    lib.clay_layer_node_influence_bound.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                    ctypes.c_uint32, fp, fp, i32p, i32p]


def brick_cache_exercise(lib) -> list[str]:
    """The refill loop an iPad app runs, driven across the FFI boundary.

    A sphere, a cache, and the one path the header documents: mark ->
    take_dirty -> eval_requests -> submit. This is the flow the whole section
    exists for, so a generated binding that cannot walk it is a broken ABI
    whatever the individual symbols resolve to.
    """
    errors = []
    brick_types(lib)
    doc = lib.clay_document_create()
    layer = ctypes.c_uint32(0)
    if lib.clay_add_sdf_layer(doc, b"bricks", ctypes.byref(layer)) != 0:
        lib.clay_document_destroy(doc)
        return ["clay_add_sdf_layer failed for the brick exercise"]
    item = ItemDesc()
    item.struct_size = ctypes.sizeof(ItemDesc)
    item.prim = 0  # CLAY_PRIM_SPHERE
    item.params[0] = 0.4
    item.rotation[3] = 1.0
    item.scale = 1.0
    node = ctypes.c_uint32(0)
    if lib.clay_add_item(doc, layer.value, ctypes.byref(item), ctypes.byref(node)) != 0:
        lib.clay_document_destroy(doc)
        return ["clay_add_item failed for the brick exercise"]

    # A defaults call is an output descriptor, so the caller declares its size
    # going IN (ABI 0.35.0). Before that it set struct_size itself, which left
    # nothing to bound the fill against: clay_brick_config had already grown a
    # field, so a host built against the older layout got 8 bytes written past
    # the end of its struct.
    undeclared = BrickConfig()  # struct_size 0, as a caller predating the rule leaves it
    if lib.clay_brick_config_defaults(ctypes.byref(undeclared)) == 0:
        errors.append("clay_brick_config_defaults accepted a descriptor declaring no "
                      "struct_size — it cannot bound its fill without one")

    config = BrickConfig()
    config.struct_size = ctypes.sizeof(BrickConfig)
    if lib.clay_brick_config_defaults(ctypes.byref(config)) != 0:
        lib.clay_document_destroy(doc)
        return ["clay_brick_config_defaults failed"]
    if config.struct_size != ctypes.sizeof(BrickConfig):
        errors.append(f"clay_brick_config_defaults returned struct_size {config.struct_size}, "
                      f"not the {ctypes.sizeof(BrickConfig)} the caller declared")
    zeroed = BrickConfig()  # setting struct_size is mandatory here too
    if lib.clay_brick_cache_create(ctypes.byref(zeroed)):
        errors.append("clay_brick_cache_create accepted a zeroed descriptor")
    # A COLOUR cache, so the GPU-atlas path is walked by a generated-binding
    # equivalent and not only from C++.
    config.colors = 1
    cache = lib.clay_brick_cache_create(ctypes.byref(config))
    if not cache:
        lib.clay_document_destroy(doc)
        return errors + ["clay_brick_cache_create rejected the engine's own defaults"]
    per = config.dim ** 3

    # the influence bound is what a host dirties, and it is finite here
    lo, hi = (ctypes.c_float * 3)(), (ctypes.c_float * 3)()
    has, infinite = ctypes.c_int32(0), ctypes.c_int32(0)
    if lib.clay_layer_node_influence_bound(doc, layer.value, node.value, lo, hi,
                                           ctypes.byref(has), ctypes.byref(infinite)) != 0:
        errors.append("clay_layer_node_influence_bound failed on a plain sphere")
    elif has.value != 1 or infinite.value != 0:
        errors.append(f"a sphere's influence bound reports has={has.value} "
                      f"infinite={infinite.value}")

    stats = BrickStats()
    stats.struct_size = ctypes.sizeof(BrickStats)
    if lib.clay_brick_cache_mark_dirty_layer(cache, doc, layer.value) != 0:
        errors.append("clay_brick_cache_mark_dirty_layer failed")
    lib.clay_brick_cache_stats(cache, ctypes.byref(stats))
    pending = stats.dirty_bricks
    if pending == 0:
        errors.append("marking a sphere's layer dirtied no bricks")

    # a region three floats cannot honestly name is refused before anything is
    # inserted: the guard the whole section rests on
    big_lo = (ctypes.c_float * 3)(-1e6, -1e6, -1e6)
    big_hi = (ctypes.c_float * 3)(1e6, 1e6, 1e6)
    if lib.clay_brick_cache_mark_dirty(cache, big_lo, big_hi) != 1:
        errors.append("clay_brick_cache_mark_dirty accepted a region spanning 1e19 bricks")
    if lib.clay_brick_cache_mark_dirty(cache, big_lo, None) != 1:
        errors.append("clay_brick_cache_mark_dirty accepted one bound without the other")
    lib.clay_brick_cache_stats(cache, ctypes.byref(stats))
    if stats.dirty_bricks != pending:
        errors.append("a refused mark_dirty still changed the cache")

    # the drain is capacity-in/count-out and never a size query
    chunk = 16
    reqs = (BrickRequest * chunk)()
    values = (ctypes.c_float * (chunk * per))()
    colors = (ctypes.c_float * (chunk * per * 3))()
    results = (ctypes.c_int32 * chunk)()
    count, remaining = ctypes.c_size_t(chunk), ctypes.c_size_t(0)
    if lib.clay_brick_cache_take_dirty(cache, None, ctypes.byref(count),
                                        ctypes.byref(remaining)) != 1:
        errors.append("clay_brick_cache_take_dirty read a NULL buffer as a size query")
    taken = accepted_total = 0
    while True:
        count.value = chunk
        if lib.clay_brick_cache_take_dirty(cache, reqs, ctypes.byref(count),
                                           ctypes.byref(remaining)) != 0:
            errors.append("clay_brick_cache_take_dirty failed")
            break
        if count.value == 0:
            break
        n = count.value
        if reqs[0].dims[0] != config.dim or reqs[0].spacing != config.voxel_size:
            errors.append("a request's lattice does not match the cache's configuration")
        # float32 on both sides: compare through ctypes rather than in Python's
        # double, or the product differs in the last bits and reads as a fault
        want_band = ctypes.c_float(config.voxel_size * config.band_voxels).value
        if reqs[0].band != want_band:
            errors.append(
                f"a request carries band {reqs[0].band}, expected {want_band}")
        if lib.clay_brick_cache_eval_requests(doc, None, reqs, n, values, n * per,
                                              colors, n * per * 3) != 0:
            errors.append("clay_brick_cache_eval_requests failed")
            break
        got = ctypes.c_size_t(0)
        if lib.clay_brick_cache_submit(cache, reqs, n, values, n * per, colors, n * per * 3,
                                       results, ctypes.byref(got)) != 0:
            errors.append("clay_brick_cache_submit failed")
            break
        taken += n
        accepted_total += got.value
        if remaining.value == 0:
            break
    if taken != pending:
        errors.append(f"the drain handed out {taken} requests for {pending} dirty bricks")
    if accepted_total != taken:
        errors.append(f"{taken - accepted_total} of {taken} submissions were not accepted")

    # a count that is not exactly count * dim^3 is refused rather than trusted
    if lib.clay_brick_cache_submit(cache, reqs, 1, values, per - 1, colors, per * 3,
                                   results, None) != 1:
        errors.append("clay_brick_cache_submit accepted a short value buffer")
    # colours are required on a cache configured for them, not optional
    if lib.clay_brick_cache_submit(cache, reqs, 1, values, per, None, 0, results, None) != 1:
        errors.append("clay_brick_cache_submit accepted a colour cache without colours")

    lib.clay_brick_cache_stats(cache, ctypes.byref(stats))
    if stats.dirty_bricks != 0 or stats.surface_bricks == 0:
        errors.append(f"after a full refill: dirty={stats.dirty_bricks} "
                      f"surface={stats.surface_bricks}")
    if stats.surface_bricks >= stats.tracked_bricks:
        errors.append("every tracked brick stores a lattice: empty space is not free")
    # 2 bytes of fp16 distance plus 4 of RGBA8 per sample: the budget bounds a
    # whole brick, not half of one
    if stats.memory_usage != stats.surface_bricks * per * 6:
        errors.append(f"memory_usage {stats.memory_usage} is not {stats.surface_bricks} "
                      f"bricks of {per} halves and colours")

    # the section's one size query, then a zero-copy read at a fixed stride
    keys_count = ctypes.c_size_t(0)
    if lib.clay_brick_cache_surface_bricks(cache, None, ctypes.byref(keys_count)) != 0:
        errors.append("clay_brick_cache_surface_bricks size query failed")
    elif keys_count.value != stats.surface_bricks:
        errors.append("surface_bricks and the stats disagree on the count")
    n = keys_count.value
    keys = (ctypes.c_int32 * (n * 3))()
    filled = ctypes.c_size_t(n)
    if lib.clay_brick_cache_surface_bricks(cache, keys, ctypes.byref(filled)) != 0:
        errors.append("clay_brick_cache_surface_bricks rejected an adequate buffer")
    states = (ctypes.c_int32 * n)()
    halves = (ctypes.c_uint16 * (n * per))()
    rgba = (ctypes.c_uint8 * (n * per * 4))()
    if lib.clay_brick_cache_read_bricks(cache, 0, keys, n, 0, states, halves, n * per,
                                        rgba, n * per * 4) != 0:
        errors.append("clay_brick_cache_read_bricks failed")
    elif any(s != 2 for s in states):  # CLAY_BRICK_SURFACE
        errors.append("a brick listed as a surface brick did not read back as one")
    elif all(a != 255 for a in rgba[3:n * per * 4:4]):
        errors.append("the colour lattice's reserved alpha is not 255")
    if lib.clay_brick_cache_read_bricks(cache, 2, keys, 1, 0, states, None, 0, None, 0) != 1:
        errors.append("clay_brick_cache_read_bricks accepted an lod above 1")

    # the apron: a padded, directly filterable tile at a fixed stride, which is
    # the property this call exists for
    padded = (config.dim + 2) ** 3
    tile = (ctypes.c_uint16 * (n * padded))()
    tile_rgba = (ctypes.c_uint8 * (n * padded * 4))()
    if lib.clay_brick_cache_read_bricks(cache, 0, keys, n, 1, states, tile, n * padded,
                                        tile_rgba, n * padded * 4) != 0:
        errors.append("clay_brick_cache_read_bricks failed with an apron")
    if lib.clay_brick_cache_read_bricks(cache, 0, keys, n, 1, states, tile, n * per,
                                        None, 0) != 1:
        errors.append("an apron readback accepted the unpadded capacity")
    if lib.clay_brick_cache_read_bricks(cache, 0, keys, 1, config.dim + 1, states, None, 0,
                                        None, 0) != 1:
        errors.append("clay_brick_cache_read_bricks accepted an apron wider than the brick")

    mesh_params = BrickMeshParams()
    mesh_params.struct_size = ctypes.sizeof(BrickMeshParams)
    mesh_params.normals = 2  # CLAY_NORMAL_GRADIENT
    mesh_params.colors = 1
    mesh = ctypes.c_void_p(0)
    if lib.clay_brick_cache_mesh(cache, doc, ctypes.byref(mesh_params), None, 0, None,
                                 ctypes.byref(mesh)) != 0 or not mesh.value:
        errors.append("clay_brick_cache_mesh failed on a filled cache")
    elif lib.clay_mesh_vertex_count(mesh) == 0:
        errors.append("the brick mesh of a filled cache has no vertices")
    else:
        # one interleaved vertex buffer, in a layout the caller chose
        vl = VertexLayout()
        vl.struct_size = ctypes.sizeof(VertexLayout)
        vl.stride = 0
        vl.position_offset, vl.normal_offset, vl.color_offset, vl.uv_offset = 0, 12, 24, -1
        verts = lib.clay_mesh_vertex_count(mesh)
        buf = (ctypes.c_uint8 * (verts * 36))()
        if lib.clay_mesh_copy_vertices(mesh, ctypes.byref(vl), buf, verts * 36) != 0:
            errors.append("clay_mesh_copy_vertices failed on an interleaved layout")
        if lib.clay_mesh_copy_vertices(mesh, ctypes.byref(vl), buf, verts * 36 - 1) != 1:
            errors.append("clay_mesh_copy_vertices accepted a short destination")
        idx_n = lib.clay_mesh_index_count(mesh)
        idx = (ctypes.c_uint32 * idx_n)()
        if lib.clay_mesh_copy_indices(mesh, idx, idx_n) != 0:
            errors.append("clay_mesh_copy_indices failed")
    lib.clay_mesh_destroy(mesh)

    # subset meshing: the dirty set's keys, and the ranges a host patches with
    subset = ctypes.c_void_p(0)
    ranges = (BrickMeshRange * n)()
    if lib.clay_brick_cache_mesh(cache, doc, ctypes.byref(mesh_params), keys, n, ranges,
                                 ctypes.byref(subset)) != 0 or not subset.value:
        errors.append("clay_brick_cache_mesh failed on an explicit key list")
    else:
        if ranges[0].vertex_first != 0 or ranges[0].index_first != 0:
            errors.append("the first key's ranges do not start at zero")
        end_v = ranges[n - 1].vertex_first + ranges[n - 1].vertex_count
        end_i = ranges[n - 1].index_first + ranges[n - 1].index_count
        if end_v != lib.clay_mesh_vertex_count(subset) or \
                end_i != lib.clay_mesh_index_count(subset):
            errors.append("the per-key ranges do not partition the subset mesh")
    lib.clay_mesh_destroy(subset)
    # ranges without a key list: nothing would have sized them
    if lib.clay_brick_cache_mesh(cache, doc, ctypes.byref(mesh_params), None, 0, ranges,
                                 ctypes.byref(subset)) != 1:
        errors.append("clay_brick_cache_mesh accepted out_ranges with no key list")

    # the batched raycast agrees with the single-ray path
    rays = (ctypes.c_float * 12)(-2.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                 -2.0, 9.0, 0.0, 1.0, 0.0, 0.0)
    many_hits = (ctypes.c_int32 * 2)()
    if lib.clay_brick_cache_raycast_many(cache, rays, 2, many_hits, None, None, None) != 0:
        errors.append("clay_brick_cache_raycast_many failed")
    elif many_hits[0] != 1 or many_hits[1] != 0:
        errors.append(f"the batched brick raycast reports {many_hits[0]}/{many_hits[1]}, "
                      "expected a hit through the origin and a miss well outside")

    lib.clay_brick_cache_destroy(cache)
    lib.clay_brick_cache_destroy(None)  # releasing a null handle is a no-op
    lib.clay_document_destroy(doc)
    return errors


# Entry points that say "layer" and mean the DOCUMENT's layer — the scene
# graph's, which clay_layer_id names. They are the reason the rule below is a
# whitelist rather than a blanket ban: this vocabulary shipped first and is not
# going anywhere.
#
# Prefixes rather than a list of the ninety names, so adding a document-layer
# call does not mean editing this gate; the rule only has to be sharp enough to
# catch a NEW artist-channel entry point spelled `layer`.
DOCUMENT_LAYER_PREFIXES = ("clay_document_", "clay_layer_")
DOCUMENT_LAYER_CALLS = {
    "clay_add_sdf_layer",
    "clay_set_layer_mirror",
    "clay_set_layer_radial",
    "clay_voxel_to_layer",
    "clay_brick_cache_mark_dirty_layer",
}


def sculpt_layer_naming() -> list[str]:
    """`layer` means three things in this library, so the ABI never says it bare.

    D1 of add-mesh-sculpt-layers. CLAY_MESH_BRUSH_LAYER is a brush ALGORITHM
    (deposit to a ceiling above the stroke-start surface), clay_layer_id is a
    DOCUMENT layer, and a sculpt layer is an artist's CHANNEL. Renaming the
    brush enumerator was the obvious way out and was rejected: it ships in
    clay.h, in the Swift enum and in every host's serialized preset, so
    renaming it would break all three to fix a documentation problem.

    Which leaves the new vocabulary carrying the whole burden — and a rule
    nobody checks is a rule that lasts one contributor. So: an entry point
    whose name contains `layer` must say `sculpt_layer` (an artist channel, on
    either the voxel or the multires stack), `mesh_layer` (a document layer
    holding a mesh), or be one of the document-layer calls above.

    The cost of documentation alone is the support burden the proposal names: a
    host author reading "layer strength" has no way to know which of three
    things it dials.
    """
    errors = []
    for name in declared_functions():
        if "layer" not in name:
            continue
        if "sculpt_layer" in name or "mesh_layer" in name:
            continue
        if name in DOCUMENT_LAYER_CALLS:
            continue
        if any(name.startswith(prefix) for prefix in DOCUMENT_LAYER_PREFIXES):
            continue
        errors.append(f"{name} says 'layer' without saying which one. An artist's channel is "
                      f"spelled sculpt_layer, a document layer holding a mesh is mesh_layer, "
                      f"and a document-layer call belongs in DOCUMENT_LAYER_CALLS in "
                      f"tools/check_c_abi.py with a reason")
    return errors


class MultiresDesc(ctypes.Structure):
    """clay_multires_desc as a bindings generator would emit it."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("rule", ctypes.c_int32),
        ("weld_epsilon", ctypes.c_float),
        ("memory_budget", ctypes.c_uint64),
    ]


class SculptLayerInfo(ctypes.Structure):
    """clay_sculpt_layer_info, ABI 0.76.0."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("id", ctypes.c_uint64),
        ("index", ctypes.c_uint32),
        ("kind", ctypes.c_int32),
        ("strength", ctypes.c_float),
        ("visible", ctypes.c_int32),
        ("locked", ctypes.c_int32),
        ("name_bytes", ctypes.c_uint32),
        ("bytes", ctypes.c_uint64),
        ("coverage_vertices", ctypes.c_uint64),
    ]


# The refusals clay_multires_error names for a sculpt layer, so the assertions
# below read as sentences rather than as three integers.
NO_SUCH_SCULPT_LAYER = 12
SCULPT_LAYER_LOCKED = 13
SCULPT_LAYER_STROKE_OPEN = 14


def sculpt_layer_argtypes(lib) -> None:
    """Pointer-width argument types, which ctypes gets wrong by default.

    An unset argtypes truncates every pointer to int on 64-bit, so a handle
    reaches the library as garbage. Declared here rather than inline so the
    exercise below reads as the flow it is testing.
    """
    p = ctypes.c_void_p
    u64p = ctypes.POINTER(ctypes.c_uint64)
    i32p = ctypes.POINTER(ctypes.c_int32)
    lib.clay_mesh_from_triangles.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
                                             ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t,
                                             ctypes.POINTER(p)]
    lib.clay_multires_defaults.argtypes = [ctypes.POINTER(MultiresDesc)]
    lib.clay_multires_from_mesh.argtypes = [p, ctypes.POINTER(MultiresDesc), ctypes.POINTER(p),
                                            i32p]
    lib.clay_multires_add_level.argtypes = [p, p, i32p]
    lib.clay_multires_destroy.argtypes = [p]
    lib.clay_multires_add_sculpt_layer.argtypes = [p, ctypes.c_char_p, u64p, i32p]
    lib.clay_multires_sculpt_layer_count.argtypes = [p, ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_multires_sculpt_layer_id_at.argtypes = [p, ctypes.c_size_t, u64p]
    lib.clay_multires_sculpt_layer_info.argtypes = [p, ctypes.c_uint64,
                                                    ctypes.POINTER(SculptLayerInfo)]
    lib.clay_multires_sculpt_layer_name.argtypes = [p, ctypes.c_uint64, ctypes.c_char_p,
                                                    ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_multires_move_sculpt_layer.argtypes = [p, ctypes.c_uint64, ctypes.c_size_t, i32p]
    lib.clay_multires_set_sculpt_layer_strength.argtypes = [p, ctypes.c_uint64, ctypes.c_float,
                                                            i32p]
    lib.clay_multires_set_sculpt_layer_locked.argtypes = [p, ctypes.c_uint64, ctypes.c_int32,
                                                          i32p]
    lib.clay_multires_set_sculpt_layer_detail.argtypes = [p, ctypes.c_uint64, ctypes.c_uint32,
                                                          ctypes.c_uint32,
                                                          ctypes.POINTER(ctypes.c_float), i32p]
    lib.clay_multires_rename_sculpt_layer.argtypes = [p, ctypes.c_uint64, ctypes.c_char_p, i32p]
    lib.clay_multires_active_sculpt_layer.argtypes = [p, u64p]
    lib.clay_multires_sculpt_layer_stroke_create.argtypes = [p, ctypes.POINTER(p)]
    lib.clay_multires_sculpt_layer_stroke_begin.argtypes = [p, i32p]
    lib.clay_multires_sculpt_layer_stroke_target_layer.argtypes = [p, u64p]
    lib.clay_multires_sculpt_layer_stroke_commit.argtypes = [p, ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_multires_sculpt_layer_stroke_destroy.argtypes = [p]


def sculpt_layer_exercise(lib) -> list[str]:
    """The sculpt layer stack driven as a bindings generator's consumer would.

    Two properties the spec delta names and nothing else in the repository
    checks across the boundary: a layer stays the SAME layer through a reorder,
    because a host holds an id and not an index; and a name arrives in a buffer
    the CALLER owns, because a pointer into an engine-owned string is freed by
    the next rename.
    """
    sculpt_layer_argtypes(lib)
    errors = []
    positions = (ctypes.c_float * 24)(-1, -1, -1,  1, -1, -1,  1, 1, -1,  -1, 1, -1,
                                      -1, -1,  1,  1, -1,  1,  1, 1,  1,  -1, 1,  1)
    indices = (ctypes.c_uint32 * 36)(0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,
                                     0, 1, 5, 0, 5, 4,  2, 3, 7, 2, 7, 6,
                                     1, 2, 6, 1, 6, 5,  0, 4, 7, 0, 7, 3)
    mesh = ctypes.c_void_p(0)
    if lib.clay_mesh_from_triangles(positions, 8, indices, 36, ctypes.byref(mesh)) != 0:
        return ["clay_mesh_from_triangles failed on a box"]

    desc = MultiresDesc()
    desc.struct_size = ctypes.sizeof(MultiresDesc)
    lib.clay_multires_defaults(ctypes.byref(desc))
    surface = ctypes.c_void_p(0)
    build_error = ctypes.c_int32(-1)
    if lib.clay_multires_from_mesh(mesh, ctypes.byref(desc), ctypes.byref(surface),
                                   ctypes.byref(build_error)) != 0:
        lib.clay_mesh_destroy(mesh)
        return ["clay_multires_from_mesh failed on a box"]
    lib.clay_mesh_destroy(mesh)
    add_error = ctypes.c_int32(-1)
    lib.clay_multires_add_level(surface, None, ctypes.byref(add_error))

    error = ctypes.c_int32(-1)
    ids = []
    for name in (b"form", b"wrinkles", b"pores"):
        layer = ctypes.c_uint64(0)
        if lib.clay_multires_add_sculpt_layer(surface, name, ctypes.byref(layer),
                                              ctypes.byref(error)) != 0:
            errors.append(f"clay_multires_add_sculpt_layer refused {name!r}")
        ids.append(layer.value)
    if len(set(ids)) != 3 or 0 in ids:
        errors.append(f"three added layers report ids {ids}, which are not three distinct "
                      f"non-zero identities")

    count = ctypes.c_size_t(0)
    if lib.clay_multires_sculpt_layer_count(surface, ctypes.byref(count)) != 0 or \
            count.value != 3:
        errors.append(f"the stack reports {count.value} layers, not 3")

    # The name crosses into the CALLER's buffer, with the size query first.
    size = ctypes.c_size_t(0)
    if lib.clay_multires_sculpt_layer_name(surface, ids[1], None, ctypes.byref(size)) != 0:
        errors.append("the sculpt layer name size query failed")
    if size.value != len(b"wrinkles") + 1:
        errors.append(f"the name query asks for {size.value} bytes, not {len(b'wrinkles') + 1}")
    short = ctypes.c_size_t(2)
    if lib.clay_multires_sculpt_layer_name(surface, ids[1], ctypes.create_string_buffer(2),
                                           ctypes.byref(short)) != 3:  # BUFFER_TOO_SMALL
        errors.append("a two-byte buffer took a nine-byte name")
    buf = ctypes.create_string_buffer(size.value)
    if lib.clay_multires_sculpt_layer_name(surface, ids[1], buf, ctypes.byref(size)) != 0 or \
            buf.value != b"wrinkles":
        errors.append(f"the name came back as {buf.value!r}, not b'wrinkles'")

    # THE SCENARIO: a host stores an identity, reorders, and dials THAT layer.
    info = SculptLayerInfo()
    info.struct_size = ctypes.sizeof(SculptLayerInfo)
    if lib.clay_multires_sculpt_layer_info(surface, ids[1], ctypes.byref(info)) != 0 or \
            info.index != 1:
        errors.append(f"'wrinkles' starts at index {info.index}, not 1")
    if lib.clay_multires_move_sculpt_layer(surface, ids[2], 0, ctypes.byref(error)) != 0:
        errors.append("clay_multires_move_sculpt_layer refused a valid position")
    if lib.clay_multires_set_sculpt_layer_strength(surface, ids[1], 0.25,
                                                   ctypes.byref(error)) != 0:
        errors.append("clay_multires_set_sculpt_layer_strength refused after a reorder")
    info = SculptLayerInfo()
    info.struct_size = ctypes.sizeof(SculptLayerInfo)
    lib.clay_multires_sculpt_layer_info(surface, ids[1], ctypes.byref(info))
    if info.index != 2 or abs(info.strength - 0.25) > 1e-6 or info.name_bytes != size.value:
        errors.append(f"after the reorder the stored id names index {info.index} at strength "
                      f"{info.strength}; the identity did not survive the move")

    # An unknown id is NOT_FOUND with the typed reason beside it, never a
    # zeroed descriptor: a zeroed one is indistinguishable from a real layer
    # sitting at strength 0.
    error.value = -1
    if lib.clay_multires_set_sculpt_layer_strength(surface, 999999, 0.5,
                                                   ctypes.byref(error)) != 2 or \
            error.value != NO_SUCH_SCULPT_LAYER:
        errors.append("an unknown sculpt layer id is not reported as NOT_FOUND / no such layer")

    # A lock refuses a COEFFICIENT WRITE and permits every property change.
    lib.clay_multires_set_sculpt_layer_locked(surface, ids[0], 1, ctypes.byref(error))
    tbn = (ctypes.c_float * 3)(0.1, 0.0, 0.0)
    error.value = -1
    if lib.clay_multires_set_sculpt_layer_detail(surface, ids[0], 1, 0, tbn,
                                                 ctypes.byref(error)) == 0 or \
            error.value != SCULPT_LAYER_LOCKED:
        errors.append("a locked sculpt layer accepted a coefficient write")
    if lib.clay_multires_rename_sculpt_layer(surface, ids[0], b"base pass",
                                             ctypes.byref(error)) != 0:
        errors.append("a locked sculpt layer refused a rename, which a lock must permit")
    lib.clay_multires_set_sculpt_layer_locked(surface, ids[0], 0, ctypes.byref(error))

    # An open gesture HOLDS the composition, so a slider refuses rather than
    # authoring one stroke against two different surfaces.
    sculptor = ctypes.c_void_p(0)
    if lib.clay_multires_sculpt_layer_stroke_create(surface, ctypes.byref(sculptor)) != 0:
        errors.append("clay_multires_sculpt_layer_stroke_create failed")
    else:
        error.value = -1
        if lib.clay_multires_sculpt_layer_stroke_begin(sculptor, ctypes.byref(error)) != 0:
            errors.append("clay_multires_sculpt_layer_stroke_begin refused a valid surface")
        target = ctypes.c_uint64(0)
        active = ctypes.c_uint64(0)
        lib.clay_multires_sculpt_layer_stroke_target_layer(sculptor, ctypes.byref(target))
        lib.clay_multires_active_sculpt_layer(surface, ctypes.byref(active))
        if target.value != active.value:
            errors.append(f"the stroke targets layer {target.value} and the stack's active "
                          f"layer is {active.value}")
        error.value = -1
        if lib.clay_multires_set_sculpt_layer_strength(surface, ids[1], 0.9,
                                                       ctypes.byref(error)) == 0 or \
                error.value != SCULPT_LAYER_STROKE_OPEN:
            errors.append("a strength change was accepted while a stroke held the composition")
        entries = ctypes.c_size_t(999)
        if lib.clay_multires_sculpt_layer_stroke_commit(sculptor, ctypes.byref(entries)) != 0 or \
                entries.value != 0:
            errors.append(f"a gesture with no stamps committed {entries.value} entries, not 0")
        if lib.clay_multires_set_sculpt_layer_strength(surface, ids[1], 0.9,
                                                       ctypes.byref(error)) != 0:
            errors.append("the composition was still held after the gesture committed")
        lib.clay_multires_sculpt_layer_stroke_destroy(sculptor)
        lib.clay_multires_sculpt_layer_stroke_destroy(None)  # releasing a null handle is a no-op

    lib.clay_multires_destroy(surface)
    lib.clay_multires_destroy(None)
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
            errors += brick_cache_exercise(lib)
            errors += tape_export_exercise(lib)
            errors += parity_fixture_exercise(lib)
            errors += sculpt_layer_exercise(lib)
        lib.clay_document_destroy(doc)
        lib.clay_document_destroy(None)  # releasing a null handle is a no-op
    return errors


def main() -> int:
    errors = hygiene()
    errors += sculpt_layer_naming()
    errors += output_descriptor_fills()
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

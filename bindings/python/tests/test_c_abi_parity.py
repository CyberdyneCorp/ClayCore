"""C ABI <-> pyclay parity (c-abi spec scenario "Composed edit from C").

The same edit is authored twice: once through the C item builder driven over
ctypes (a stand-in for any bindings generator), once through pyclay. The two
documents must evaluate identically — that is the standard the spec states,
that a C consumer reaches every authoring capability pyclay reaches.

Skipped when the shared library is not next to the built pyclay module.
"""

import ctypes
from pathlib import Path

import numpy as np
import pytest

import pyclay as clay

CLAY_PRIM_BOX = 1
CLAY_PRIM_STROKE = 14
CLAY_PRIM_EXTRUDE = 15
CLAY_DEFORM_TWIST = 0
CLAY_DEFORM_BEND = 1
CLAY_DEFORM_TAPER = 2
TAPER_EASE = 5  # in-out quad, so the index is not the identity curve


def find_shared_library():
    start = Path(clay.__file__).resolve().parent
    names = ("libclay_shared.so", "libclay_shared.dylib", "clay_shared.dll")
    for directory in (start, *start.parents[:4]):
        for name in names:
            if (directory / name).exists():
                return directory / name
    return None


@pytest.fixture(scope="module")
def lib():
    path = find_shared_library()
    if path is None:
        pytest.skip("libclay_shared not built")
    lib = ctypes.CDLL(str(path))
    f32 = ctypes.POINTER(ctypes.c_float)
    lib.clay_document_create.restype = ctypes.c_void_p
    lib.clay_document_destroy.argtypes = [ctypes.c_void_p]
    lib.clay_document_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.clay_add_sdf_layer.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                       ctypes.POINTER(ctypes.c_uint32)]
    lib.clay_item_create.argtypes = [ctypes.c_int32, f32, ctypes.c_size_t]
    lib.clay_item_create.restype = ctypes.c_void_p
    lib.clay_item_destroy.argtypes = [ctypes.c_void_p]
    lib.clay_item_set_position.argtypes = [ctypes.c_void_p, f32]
    lib.clay_item_set_rotation.argtypes = [ctypes.c_void_p, f32, ctypes.c_float]
    lib.clay_item_set_scale.argtypes = [ctypes.c_void_p, ctypes.c_float]
    lib.clay_item_set_blend.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_float]
    lib.clay_item_set_rounding.argtypes = [ctypes.c_void_p, ctypes.c_float]
    lib.clay_item_set_color.argtypes = [ctypes.c_void_p, f32]
    lib.clay_item_add_deformer.argtypes = [ctypes.c_void_p, ctypes.c_int32, f32,
                                           ctypes.c_size_t, ctypes.c_int32]
    lib.clay_item_set_repeat_radial.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_float]
    lib.clay_item_set_stroke_points.argtypes = [ctypes.c_void_p, f32, ctypes.c_size_t]
    lib.clay_item_set_stroke_blend_k.argtypes = [ctypes.c_void_p, ctypes.c_float]
    lib.clay_item_set_profile_polygon.argtypes = [ctypes.c_void_p, f32, ctypes.c_size_t]
    lib.clay_layer_add_item.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                                        ctypes.POINTER(ctypes.c_uint32)]
    return lib


def floats(*values):
    return (ctypes.c_float * len(values))(*values)


STROKE_POINTS = [(-0.8, 0.0, 0.0, 0.20), (-0.2, 0.4, 0.0, 0.15),
                 (0.3, 0.1, 0.0, 0.18), (0.9, 0.5, 0.0, 0.10)]
POLYGON = [(-0.5, -0.5), (0.5, -0.5), (0.6, 0.2), (0.0, 0.7), (-0.6, 0.3)]


def build_through_c(lib, path):
    doc = lib.clay_document_create()
    layer = ctypes.c_uint32(0)
    assert lib.clay_add_sdf_layer(doc, b"body", ctypes.byref(layer)) == 0

    # half-extents at the C boundary; pyclay's Box takes full side lengths
    item = lib.clay_item_create(CLAY_PRIM_BOX, floats(0.4, 0.9, 0.4), 3)
    assert item
    assert lib.clay_item_set_position(item, floats(0.1, 0.2, -0.3)) == 0
    assert lib.clay_item_set_rotation(item, floats(0.0, 1.0, 0.0), 0.6) == 0
    assert lib.clay_item_set_scale(item, 1.2) == 0
    assert lib.clay_item_set_color(item, floats(0.2, 0.4, 0.6)) == 0
    assert lib.clay_item_set_rounding(item, 0.05) == 0
    assert lib.clay_item_set_blend(item, 1, 0.1) == 0  # CLAY_BLEND_QUADRATIC
    assert lib.clay_item_add_deformer(item, CLAY_DEFORM_TWIST, floats(0.7), 1, 0) == 0
    assert lib.clay_item_add_deformer(item, CLAY_DEFORM_BEND, floats(0.4), 1, 0) == 0
    # a non-linear easing curve: the index means the same on both sides
    assert lib.clay_item_add_deformer(item, CLAY_DEFORM_TAPER,
                                      floats(-1.0, 1.0, 1.0, 0.5), 4, TAPER_EASE) == 0
    assert lib.clay_item_set_repeat_radial(item, 6, 1.2) == 0
    assert lib.clay_layer_add_item(doc, layer.value, item, None) == 0
    lib.clay_item_destroy(item)

    stroke = lib.clay_item_create(CLAY_PRIM_STROKE, None, 0)
    flat = [v for point in STROKE_POINTS for v in point]
    assert lib.clay_item_set_stroke_points(stroke, floats(*flat), len(STROKE_POINTS)) == 0
    assert lib.clay_item_set_stroke_blend_k(stroke, 0.05) == 0
    assert lib.clay_layer_add_item(doc, layer.value, stroke, None) == 0
    lib.clay_item_destroy(stroke)

    lift = lib.clay_item_create(CLAY_PRIM_EXTRUDE, floats(0.25), 1)
    outline = [v for vertex in POLYGON for v in vertex]
    assert lib.clay_item_set_profile_polygon(lift, floats(*outline), len(POLYGON)) == 0
    assert lib.clay_item_set_position(lift, floats(1.4, 0.0, 0.0)) == 0
    assert lib.clay_layer_add_item(doc, layer.value, lift, None) == 0
    lib.clay_item_destroy(lift)

    assert lib.clay_document_save(doc, str(path).encode()) == 0
    lib.clay_document_destroy(doc)


def build_through_pyclay():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    box = clay.Box(size=(0.8, 1.8, 0.8), position=(0.1, 0.2, -0.3),
                   rotation_axis_angle=((0, 1, 0), 0.6), scale=1.2)
    box.twist(0.7).bend(0.4).taper(-1.0, 1.0, 1.0, 0.5, ease=TAPER_EASE).repeat_radial(6, 1.2)
    layer.add(box, blend=clay.Smooth(0.1), color=(0.2, 0.4, 0.6), rounding=0.05)
    layer.add(clay.Stroke(points=STROKE_POINTS, blend_k=0.05))
    layer.add(clay.Extrude(clay.Profile.polygon(POLYGON), half_depth=0.25,
                           position=(1.4, 0, 0)))
    return doc


def sample_points():
    rng = np.random.default_rng(7)
    return rng.uniform(-2.5, 2.5, size=(400, 3)).astype(np.float32)


def report(got, want, what):
    """By how much, and how widely, two sampled fields disagree."""
    gap = np.abs(got - want)
    i = int(np.argmax(gap))
    return (f"{what}: widest gap {gap.flat[i]:g} ({got.flat[i]:g} from C, "
            f"{want.flat[i]:g} from pyclay), {int((gap > 1e-6).sum())} of {gap.size} values differ")


def test_composed_c_edit_matches_pyclay(lib, tmp_path):
    path = tmp_path / "from_c.clayspace"
    build_through_c(lib, path)
    from_c = clay.load(str(path))
    from_python = build_through_pyclay()

    pts = sample_points()
    c_d, py_d = from_c.eval(pts), from_python.eval(pts)
    assert np.allclose(c_d, py_d, atol=1e-6), report(c_d, py_d, "distance")
    c_rgb, py_rgb = from_c.colors(pts), from_python.colors(pts)
    assert np.allclose(c_rgb, py_rgb, atol=1e-6), report(c_rgb, py_rgb, "colour")


def build_order_through_c(lib, path, twist_first):
    doc = lib.clay_document_create()
    layer = ctypes.c_uint32(0)
    assert lib.clay_add_sdf_layer(doc, b"body", ctypes.byref(layer)) == 0
    item = lib.clay_item_create(CLAY_PRIM_BOX, floats(0.3, 1.0, 0.3), 3)
    assert item
    chain = [(CLAY_DEFORM_TWIST, 1.1), (CLAY_DEFORM_BEND, 0.7)]
    if not twist_first:
        chain.reverse()
    for kind, k in chain:
        assert lib.clay_item_add_deformer(item, kind, floats(k), 1, 0) == 0
    assert lib.clay_layer_add_item(doc, layer.value, item, None) == 0
    lib.clay_item_destroy(item)
    assert lib.clay_document_save(doc, str(path).encode()) == 0
    lib.clay_document_destroy(doc)


def build_order_through_pyclay(twist_first):
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    box = clay.Box(size=(0.6, 2.0, 0.6))
    if twist_first:
        box.twist(1.1).bend(0.7)
    else:
        box.bend(0.7).twist(1.1)
    layer.add(box)
    return doc


def test_deformer_order_matches_pyclay(lib, tmp_path):
    """Warps do not commute, and the C chain applies in the order it was
    called — the same order pyclay applies."""
    pts = sample_points()
    fields = {}
    for twist_first in (True, False):
        path = tmp_path / f"order_{int(twist_first)}.clayspace"
        build_order_through_c(lib, path, twist_first)
        from_c = clay.load(str(path)).eval(pts)
        from_python = build_order_through_pyclay(twist_first).eval(pts)
        order = "twist then bend" if twist_first else "bend then twist"
        assert np.allclose(from_c, from_python, atol=1e-6), report(from_c, from_python, order)
        fields[twist_first] = from_c

    gap = np.abs(fields[True] - fields[False])
    assert gap.max() > 1e-3, f"the two orders agree everywhere (widest gap {gap.max():g})"

"""C ABI <-> pyclay parity (c-abi spec scenarios).

The same work is done twice: once through the C ABI driven over ctypes (a
stand-in for any bindings generator), once through pyclay. The two must agree
— that is the standard the spec states, that a C consumer reaches every
capability pyclay reaches, and reaches it with the same answer.

Covered here: a composed SDF edit, deformer order, a voxel sculpting sequence
compared cell by cell and mesh by mesh, a dithered falloff brush, the
flood-select size query, selection bounds, the borrowed voxel-layer handle,
and the experimental mesher's gate.

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

CLAY_OK = 0
CLAY_ERROR_INVALID_ARGUMENT = 1
CLAY_ERROR_BUFFER_TOO_SMALL = 3
CLAY_BRUSH_SHAPE_CUBE = 0
CLAY_BRUSH_SHAPE_SPHERE = 1
CLAY_BRUSH_FALLOFF_LINEAR = 1
CLAY_BRUSH_FALLOFF_GAUSSIAN = 3
CLAY_MESHER_MARCHING = 0
CLAY_MESHER_DUAL_CONTOURING = 2
VOXEL_SIZE = 0.1


class BrushParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("size", ctypes.c_int32),
        ("shape", ctypes.c_int32),
        ("falloff", ctypes.c_int32),
        ("strength", ctypes.c_float),
        ("seed", ctypes.c_uint32),
    ]


class MeshParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("voxel_size", ctypes.c_float),
        ("resolution", ctypes.c_int32),
        ("decimate", ctypes.c_int32),
        ("decimate_ratio", ctypes.c_float),
        ("mesher", ctypes.c_int32),
        ("experimental", ctypes.c_int32),
    ]


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
    lib.clay_item_set_op.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    # the group surface: the two float arguments make argtypes mandatory here,
    # since ctypes would otherwise promote them to double
    lib.clay_layer_add_group.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
                                         ctypes.c_float, ctypes.c_float,
                                         ctypes.POINTER(ctypes.c_uint32)]
    lib.clay_layer_add_item_in_group.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                 ctypes.c_uint32, ctypes.c_int32,
                                                 ctypes.c_void_p,
                                                 ctypes.POINTER(ctypes.c_uint32)]
    lib.clay_layer_children.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                        ctypes.POINTER(ctypes.c_uint32),
                                        ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_last_error.restype = ctypes.c_char_p

    i32 = ctypes.POINTER(ctypes.c_int32)
    brush_p = ctypes.POINTER(BrushParams)
    lib.clay_layer_selection_bounds.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t,
                                                f32, f32, i32]
    lib.clay_document_mesh.argtypes = [ctypes.c_void_p, ctypes.POINTER(MeshParams),
                                       ctypes.POINTER(ctypes.c_void_p)]
    lib.clay_mesh_destroy.argtypes = [ctypes.c_void_p]
    for name in ("clay_mesh_vertex_count", "clay_mesh_index_count"):
        getattr(lib, name).argtypes = [ctypes.c_void_p]
        getattr(lib, name).restype = ctypes.c_size_t
    for name in ("clay_mesh_positions", "clay_mesh_normals", "clay_mesh_colors"):
        getattr(lib, name).argtypes = [ctypes.c_void_p]
        getattr(lib, name).restype = f32
    lib.clay_mesh_indices.argtypes = [ctypes.c_void_p]
    lib.clay_mesh_indices.restype = ctypes.POINTER(ctypes.c_uint32)

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
    lib.clay_voxel_palette_add.argtypes = [ctypes.c_void_p, f32, i32]
    lib.clay_voxel_get.argtypes = [ctypes.c_void_p, i32, i32]
    lib.clay_voxel_set.argtypes = [ctypes.c_void_p, i32, ctypes.c_int32]
    lib.clay_voxel_set_brush.argtypes = [ctypes.c_void_p, i32, brush_p, ctypes.c_int32]
    lib.clay_voxel_erase_brush.argtypes = [ctypes.c_void_p, i32, brush_p]
    lib.clay_voxel_sculpt_smooth.argtypes = [ctypes.c_void_p, i32, brush_p]
    lib.clay_voxel_sculpt_inflate.argtypes = [ctypes.c_void_p, i32, brush_p, ctypes.c_int32]
    lib.clay_voxel_occupied_count.argtypes = [ctypes.c_void_p,
                                              ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_voxel_bounds.argtypes = [ctypes.c_void_p, i32, i32, i32]
    lib.clay_voxel_flood_select.argtypes = [ctypes.c_void_p, i32, ctypes.c_int32, i32,
                                            ctypes.POINTER(ctypes.c_size_t)]
    lib.clay_voxel_mesh.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    return lib


def floats(*values):
    return (ctypes.c_float * len(values))(*values)


def cell(*values):
    return (ctypes.c_int32 * len(values))(*values)


def brush(size, shape=CLAY_BRUSH_SHAPE_CUBE, falloff=0, strength=1.0, seed=0):
    p = BrushParams()
    p.struct_size = ctypes.sizeof(BrushParams)
    p.size, p.shape, p.falloff, p.strength, p.seed = size, shape, falloff, strength, seed
    return p


def hex_rgb(text):
    """The float triple pyclay's '#rrggbb' parses to, so both sides of a
    comparison put the very same numbers in the palette."""
    return tuple(int(text[i:i + 2], 16) / 255.0 for i in (1, 3, 5))


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


# -- voxels ----------------------------------------------------------------------

ORANGE, BLUE = "#e65a33", "#3380e6"


def c_voxel_cells(lib, grid):
    """Every occupied cell of a C grid as {(x, y, z): palette index}."""
    lo, hi, has = cell(0, 0, 0), cell(0, 0, 0), cell(0)
    assert lib.clay_voxel_bounds(grid, lo, hi, has) == CLAY_OK
    if not has[0]:
        return {}
    out, index = {}, ctypes.c_int32(0)
    for x in range(lo[0], hi[0] + 1):
        for y in range(lo[1], hi[1] + 1):
            for z in range(lo[2], hi[2] + 1):
                assert lib.clay_voxel_get(grid, cell(x, y, z), ctypes.byref(index)) == CLAY_OK
                if index.value:
                    out[(x, y, z)] = index.value
    return out


def py_voxel_cells(grid):
    """The same reading of a pyclay grid."""
    box = grid.bounds()
    if box is None:
        return {}
    (x0, y0, z0), (x1, y1, z1) = box
    return {(x, y, z): grid.get((x, y, z))
            for x in range(x0, x1 + 1)
            for y in range(y0, y1 + 1)
            for z in range(z0, z1 + 1)
            if grid.get((x, y, z))}


def report_cells(got, want):
    """Which cells the two sides disagree about, and how."""
    keys = sorted(set(got) | set(want))
    bad = [k for k in keys if got.get(k, 0) != want.get(k, 0)]
    lines = [f"{len(bad)} of {len(keys)} cells differ (C has {len(got)} occupied, "
             f"pyclay {len(want)})"]
    lines += [f"  {k}: {got.get(k, 0)} from C, {want.get(k, 0)} from pyclay" for k in bad[:8]]
    return "\n".join(lines)


def c_mesh_arrays(lib, mesh):
    """A C mesh's buffers as numpy arrays, copied off the borrowed pointers."""
    vertices, indices = lib.clay_mesh_vertex_count(mesh), lib.clay_mesh_index_count(mesh)
    out = {}
    for name, getter in (("positions", lib.clay_mesh_positions),
                         ("normals", lib.clay_mesh_normals),
                         ("colors", lib.clay_mesh_colors)):
        ptr = getter(mesh)
        out[name] = (np.ctypeslib.as_array(ptr, shape=(vertices, 3)).copy() if ptr
                     else np.empty((0, 3), dtype=np.float32))
    ptr = lib.clay_mesh_indices(mesh)
    out["indices"] = (np.ctypeslib.as_array(ptr, shape=(indices // 3, 3)).copy() if ptr
                      else np.empty((0, 3), dtype=np.uint32))
    return out


def sculpt_through_c(lib, grid):
    """The spec's sequence: palette entries, a sphere stamp, a sculpt verb."""
    orange, blue = ctypes.c_int32(0), ctypes.c_int32(0)
    assert lib.clay_voxel_palette_add(grid, floats(*hex_rgb(ORANGE)),
                                      ctypes.byref(orange)) == CLAY_OK
    assert lib.clay_voxel_palette_add(grid, floats(*hex_rgb(BLUE)),
                                      ctypes.byref(blue)) == CLAY_OK
    ball = brush(7, shape=CLAY_BRUSH_SHAPE_SPHERE)
    assert lib.clay_voxel_set_brush(grid, cell(0, 0, 0), ctypes.byref(ball), orange) == CLAY_OK
    block = brush(5)
    assert lib.clay_voxel_set_brush(grid, cell(5, 1, 0), ctypes.byref(block), blue) == CLAY_OK
    assert lib.clay_voxel_sculpt_smooth(grid, cell(3, 0, 0), ctypes.byref(ball)) == CLAY_OK
    wide = brush(11, shape=CLAY_BRUSH_SHAPE_SPHERE)
    assert lib.clay_voxel_sculpt_inflate(grid, cell(0, 0, 0), ctypes.byref(wide), -1) == CLAY_OK


def sculpt_through_pyclay():
    grid = clay.VoxelGrid(VOXEL_SIZE)
    orange, blue = grid.palette_add(ORANGE), grid.palette_add(BLUE)
    grid.set_brush((0, 0, 0), 7, orange, shape="sphere")
    grid.set_brush((5, 1, 0), 5, blue, shape="cube")
    grid.sculpt_smooth((3, 0, 0), 7, shape="sphere")
    grid.sculpt_inflate((0, 0, 0), 11, amount=-1, shape="sphere")
    return grid


def test_voxel_sculpting_matches_pyclay(lib):
    """Spec: "the mesh matches the same sequence performed through pyclay" —
    checked on the occupied cells first, so a divergence names the cell, and
    then on every buffer of the greedy mesh."""
    grid = lib.clay_voxel_grid_create(VOXEL_SIZE)
    assert grid
    try:
        sculpt_through_c(lib, grid)
        from_python = sculpt_through_pyclay()
        from_c = c_voxel_cells(lib, grid)
        assert from_c, "the C sequence left the grid empty"
        assert from_c == py_voxel_cells(from_python), report_cells(from_c,
                                                                  py_voxel_cells(from_python))

        mesh = ctypes.c_void_p(0)
        assert lib.clay_voxel_mesh(grid, ctypes.byref(mesh)) == CLAY_OK
        try:
            c_mesh = c_mesh_arrays(lib, mesh)
            py_mesh = from_python.mesh()
            assert len(c_mesh["positions"]) == len(py_mesh.positions), (
                f"the greedy mesh has {len(c_mesh['positions'])} vertices from C and "
                f"{len(py_mesh.positions)} from pyclay")
            for name, want in (("positions", py_mesh.positions), ("normals", py_mesh.normals),
                               ("colors", py_mesh.colors), ("indices", py_mesh.indices)):
                got = c_mesh[name]
                assert np.array_equal(got, want), report(got.astype(np.float64),
                                                         want.astype(np.float64), name)
        finally:
            lib.clay_mesh_destroy(mesh)
    finally:
        lib.clay_voxel_grid_destroy(grid)


def test_falloff_dither_matches_pyclay(lib):
    """Spec: a falloff brush with a given seed affects exactly the same cells.

    Coverage below 1 is resolved by dithering against a hash of the cell and
    the seed, so this only holds if both bindings hash the same thing.
    """
    seed, strength, size = 0x5EED1234, 0.55, 11
    grid = lib.clay_voxel_grid_create(VOXEL_SIZE)
    assert grid
    try:
        index = ctypes.c_int32(0)
        assert lib.clay_voxel_palette_add(grid, floats(*hex_rgb(ORANGE)),
                                          ctypes.byref(index)) == CLAY_OK
        soft = brush(size, shape=CLAY_BRUSH_SHAPE_SPHERE,
                     falloff=CLAY_BRUSH_FALLOFF_GAUSSIAN, strength=strength, seed=seed)
        assert lib.clay_voxel_set_brush(grid, cell(2, -1, 3), ctypes.byref(soft),
                                        index) == CLAY_OK
        from_c = c_voxel_cells(lib, grid)

        from_python = clay.VoxelGrid(VOXEL_SIZE)
        colour = from_python.palette_add(ORANGE)
        from_python.set_brush((2, -1, 3), size, colour, shape="sphere", falloff="gaussian",
                              strength=strength, seed=seed)
        want = py_voxel_cells(from_python)

        assert 0 < len(from_c) < size ** 3, (
            f"a strength-{strength} stamp filled {len(from_c)} cells: the dither did nothing")
        assert from_c == want, report_cells(from_c, want)
    finally:
        lib.clay_voxel_grid_destroy(grid)


def test_brush_strength_is_passed_through_and_zero_is_refused(lib):
    """A strength the C side reinterprets is a strength that means something
    else through C than through pyclay.

    It did: 0 (and any negative) was read as 1, so a C brush at the 0 end of a
    strength slider applied at FULL coverage where pyclay applies to nothing —
    and clay_voxel_erase_brush took the whole footprint away. Every strength
    the C boundary accepts now reaches the engine untouched, and the values it
    does not accept are refused rather than reinterpreted.
    """
    seed, size = 0x51DE, 9
    grid = lib.clay_voxel_grid_create(VOXEL_SIZE)
    assert grid
    try:
        index = ctypes.c_int32(0)
        assert lib.clay_voxel_palette_add(grid, floats(*hex_rgb(ORANGE)),
                                          ctypes.byref(index)) == CLAY_OK
        for strength in (0.05, 0.4, 0.9, 1.0, 2.5):
            assert lib.clay_voxel_erase_brush(
                grid, cell(0, 0, 0),
                ctypes.byref(brush(size, shape=CLAY_BRUSH_SHAPE_SPHERE))) == CLAY_OK
            soft = brush(size, shape=CLAY_BRUSH_SHAPE_SPHERE,
                         falloff=CLAY_BRUSH_FALLOFF_LINEAR, strength=strength, seed=seed)
            assert lib.clay_voxel_set_brush(grid, cell(0, 0, 0), ctypes.byref(soft),
                                            index) == CLAY_OK
            from_c = c_voxel_cells(lib, grid)

            from_python = clay.VoxelGrid(VOXEL_SIZE)
            colour = from_python.palette_add(ORANGE)
            from_python.set_brush((0, 0, 0), size, colour, shape="sphere", falloff="linear",
                                  strength=strength, seed=seed)
            want = py_voxel_cells(from_python)
            assert from_c == want, f"strength {strength}: " + report_cells(from_c, want)

        # A strength that covers nothing is a zero-initialized descriptor, not
        # a request, so it is refused — and the refusal leaves the grid alone,
        # which is what makes the destructive verb safe.
        before = c_voxel_cells(lib, grid)
        assert before
        for strength in (0.0, -1.0, float("nan")):
            bad = brush(size, shape=CLAY_BRUSH_SHAPE_SPHERE, strength=strength)
            assert lib.clay_voxel_set_brush(grid, cell(0, 0, 0), ctypes.byref(bad),
                                            index) == CLAY_ERROR_INVALID_ARGUMENT
            assert b"strength" in lib.clay_last_error()
            assert lib.clay_voxel_erase_brush(grid, cell(0, 0, 0),
                                              ctypes.byref(bad)) == CLAY_ERROR_INVALID_ARGUMENT
        assert c_voxel_cells(lib, grid) == before
    finally:
        lib.clay_voxel_grid_destroy(grid)


def test_flood_select_size_query_then_fill(lib):
    """Spec: "flood select with a null buffer reports the required cell count,
    and a second call with an adequate buffer fills it" — with the same cells,
    in the same order, that pyclay reports."""
    grid = lib.clay_voxel_grid_create(VOXEL_SIZE)
    assert grid
    try:
        index = ctypes.c_int32(0)
        lib.clay_voxel_palette_add(grid, floats(*hex_rgb(BLUE)), ctypes.byref(index))
        ball = brush(9, shape=CLAY_BRUSH_SHAPE_SPHERE)
        assert lib.clay_voxel_set_brush(grid, cell(0, 0, 0), ctypes.byref(ball),
                                        index) == CLAY_OK

        count = ctypes.c_size_t(0)
        assert lib.clay_voxel_flood_select(grid, cell(0, 0, 0), 1, None,
                                           ctypes.byref(count)) == CLAY_OK
        needed = count.value
        occupied = ctypes.c_size_t(0)
        lib.clay_voxel_occupied_count(grid, ctypes.byref(occupied))
        assert needed == occupied.value, (
            f"the size query reports {needed} cells, the grid holds {occupied.value}")

        short = ctypes.c_size_t(needed - 1)
        buf = (ctypes.c_int32 * (needed * 3))()
        assert lib.clay_voxel_flood_select(grid, cell(0, 0, 0), 1, buf,
                                           ctypes.byref(short)) == CLAY_ERROR_BUFFER_TOO_SMALL
        assert short.value == needed, f"a short call asks for {short.value}, not {needed}"

        filled = ctypes.c_size_t(needed)
        assert lib.clay_voxel_flood_select(grid, cell(0, 0, 0), 1, buf,
                                           ctypes.byref(filled)) == CLAY_OK
        assert filled.value == needed
        from_c = np.ctypeslib.as_array(buf).reshape(needed, 3)

        from_python = clay.VoxelGrid(VOXEL_SIZE)
        colour = from_python.palette_add(BLUE)
        from_python.set_brush((0, 0, 0), 9, colour, shape="sphere")
        want = from_python.flood_select((0, 0, 0))
        assert np.array_equal(from_c, want), (
            f"the selections differ: {len(from_c)} cells from C, {len(want)} from pyclay, "
            f"first mismatch at "
            f"{int(np.argmax((from_c != want).any(axis=1))) if len(from_c) == len(want) else 0}")
    finally:
        lib.clay_voxel_grid_destroy(grid)


def test_borrowed_voxel_layer_refuses_destroy(lib, tmp_path):
    """Spec: destroying a borrowed handle returns an invalid-argument error and
    the document is unaffected — here proved by saving it and reading the edits
    back through pyclay."""
    doc = lib.clay_document_create()
    try:
        layer, grid = ctypes.c_uint32(0), ctypes.c_void_p(0)
        assert lib.clay_document_add_voxel_layer(doc, b"clay", VOXEL_SIZE,
                                                 ctypes.byref(layer),
                                                 ctypes.byref(grid)) == CLAY_OK
        index = ctypes.c_int32(0)
        lib.clay_voxel_palette_add(grid, floats(*hex_rgb(ORANGE)), ctypes.byref(index))
        block = brush(3)
        assert lib.clay_voxel_set_brush(grid, cell(1, 2, 3), ctypes.byref(block),
                                        index) == CLAY_OK

        assert lib.clay_voxel_grid_destroy(grid) == CLAY_ERROR_INVALID_ARGUMENT, (
            "destroying a borrowed handle was obeyed")
        detail = lib.clay_last_error().decode()
        assert "document" in detail, f"the refusal says {detail!r} and never mentions the owner"

        again = ctypes.c_void_p(0)
        assert lib.clay_document_voxel_layer(doc, b"clay", None,
                                             ctypes.byref(again)) == CLAY_OK
        assert again.value == grid.value, "the layer handed out a second, different handle"
        assert c_voxel_cells(lib, grid), "the refused destroy emptied the layer"

        path = tmp_path / "borrowed.clayspace"
        assert lib.clay_document_save(doc, str(path).encode()) == CLAY_OK
        from_python = clay.load(str(path)).voxel_layer("clay")
        assert from_python is not None, "the voxel layer did not survive the round trip"
        assert py_voxel_cells(from_python) == c_voxel_cells(lib, grid), report_cells(
            c_voxel_cells(lib, grid), py_voxel_cells(from_python))
    finally:
        lib.clay_document_destroy(doc)


# -- picking and meshing ---------------------------------------------------------

SELECTION_BOXES = [((0.3, 0.3, 0.3), (-1.2, 0.0, 0.4)),
                   ((0.5, 0.2, 0.5), (0.0, 0.6, -0.3)),
                   ((0.2, 0.7, 0.2), (1.1, -0.4, 0.2))]


def selection_document_through_c(lib):
    doc = lib.clay_document_create()
    layer = ctypes.c_uint32(0)
    assert lib.clay_add_sdf_layer(doc, b"body", ctypes.byref(layer)) == 0
    nodes = []
    for half, position in SELECTION_BOXES:
        item = lib.clay_item_create(CLAY_PRIM_BOX, floats(*half), 3)
        assert item
        assert lib.clay_item_set_position(item, floats(*position)) == 0
        node = ctypes.c_uint32(0)
        assert lib.clay_layer_add_item(doc, layer.value, item, ctypes.byref(node)) == 0
        lib.clay_item_destroy(item)
        nodes.append(node.value)
    return doc, layer.value, nodes


def selection_document_through_pyclay():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    nodes = [layer.add(clay.Box(size=tuple(2 * h for h in half), position=position))
             for half, position in SELECTION_BOXES]
    return doc, layer, nodes


def c_selection_bounds(lib, doc, layer, nodes):
    ids = (ctypes.c_uint32 * len(nodes))(*nodes)
    lo, hi, has = floats(0, 0, 0), floats(0, 0, 0), ctypes.c_int32(0)
    assert lib.clay_layer_selection_bounds(doc, layer, ids, len(nodes), lo, hi,
                                           ctypes.byref(has)) == CLAY_OK
    if not has.value:
        return None
    return (tuple(lo), tuple(hi))


def test_selection_bounds_match_pyclay(lib):
    """Spec: "it receives the same AABB the Python bindings report for that
    selection" — for every subset, so a wrong id or a dropped node shows."""
    doc, layer, c_nodes = selection_document_through_c(lib)
    try:
        py_doc, py_layer, py_nodes = selection_document_through_pyclay()
        subsets = [(0,), (2,), (0, 2), (0, 1, 2), ()]
        for subset in subsets:
            got = c_selection_bounds(lib, doc, layer, [c_nodes[i] for i in subset])
            want = py_layer.selection_bounds([py_nodes[i] for i in subset])
            if want is None:
                assert got is None, f"subset {subset}: C reports {got}, pyclay reports nothing"
                continue
            assert got is not None, f"subset {subset}: C reports no bounds, pyclay {want}"
            assert np.allclose(got, want, atol=1e-6), (
                f"subset {subset}: C reports {got}, pyclay {want}")
        # a node id the layer does not hold contributes nothing rather than failing
        assert c_selection_bounds(lib, doc, layer, [9999]) is None
    finally:
        lib.clay_document_destroy(doc)


def mesh_params(mesher, experimental):
    p = MeshParams()
    p.struct_size = ctypes.sizeof(MeshParams)
    p.resolution, p.mesher, p.experimental = 32, mesher, int(experimental)
    return p


def test_experimental_mesher_is_gated_on_both_sides(lib):
    """Spec: "the call returns an error rather than silently meshing" — and
    pyclay refuses the same request in the same place."""
    doc, _, _ = selection_document_through_c(lib)
    try:
        mesh = ctypes.c_void_p(0xDEAD)
        params = mesh_params(CLAY_MESHER_DUAL_CONTOURING, experimental=False)
        assert lib.clay_document_mesh(doc, ctypes.byref(params),
                                      ctypes.byref(mesh)) == CLAY_ERROR_INVALID_ARGUMENT
        detail = lib.clay_last_error().decode()
        assert "experimental" in detail, f"the refusal says {detail!r}"
        assert mesh.value == 0xDEAD, "a refused mesh call still wrote an out handle"

        params.experimental = 1
        assert lib.clay_document_mesh(doc, ctypes.byref(params),
                                      ctypes.byref(mesh)) == CLAY_OK
        assert lib.clay_mesh_vertex_count(mesh) > 0
        lib.clay_mesh_destroy(mesh)

        py_doc, _, _ = selection_document_through_pyclay()
        with pytest.raises(ValueError, match="experimental"):
            py_doc.mesh(resolution=32, mesher="dual_contouring")
        assert py_doc.mesh(resolution=32, mesher="dual_contouring",
                           experimental=True).triangle_count > 0
    finally:
        lib.clay_document_destroy(doc)


def test_marching_mesher_matches_pyclay(lib):
    """The default mesher is the same mesher, at the same resolution."""
    doc, _, _ = selection_document_through_c(lib)
    try:
        mesh = ctypes.c_void_p(0)
        params = mesh_params(CLAY_MESHER_MARCHING, experimental=False)
        assert lib.clay_document_mesh(doc, ctypes.byref(params),
                                      ctypes.byref(mesh)) == CLAY_OK
        try:
            py_doc, _, _ = selection_document_through_pyclay()
            py_mesh = py_doc.mesh(resolution=32, mesher="marching")
            got = c_mesh_arrays(lib, mesh)
            assert np.allclose(got["positions"], py_mesh.positions, atol=1e-6), report(
                got["positions"], py_mesh.positions, "vertex")
            assert np.array_equal(got["indices"], py_mesh.indices)
        finally:
            lib.clay_mesh_destroy(mesh)
    finally:
        lib.clay_document_destroy(doc)


# -- groups --------------------------------------------------------------------

CLAY_OP_ADD = 0
CLAY_OP_SUBTRACT = 1
CLAY_OP_INTERSECT = 2
CLAY_OP_INLINE = 255
CLAY_BLEND_HARD = 0
CLAY_BLEND_QUADRATIC = 1
CLAY_PRIM_SPHERE = 0


def group_document_through_c(lib, path):
    """(shell INTERSECT cutter) UNION another sphere, plus an inline group —
    the construction that needs a group, built the way a C host builds it."""
    doc = lib.clay_document_create()
    layer = ctypes.c_uint32(0)
    assert lib.clay_add_sdf_layer(doc, b"body", ctypes.byref(layer)) == 0
    plate = ctypes.c_uint32(0)
    assert lib.clay_layer_add_group(doc, layer.value, 0, -1, CLAY_OP_ADD, CLAY_BLEND_QUADRATIC,
                                    0.08, 0.0, ctypes.byref(plate)) == 0

    def add(parent, prim_params, position, op):
        item = lib.clay_item_create(CLAY_PRIM_SPHERE, floats(*prim_params), 1)
        assert item
        assert lib.clay_item_set_position(item, floats(*position)) == 0
        assert lib.clay_item_set_op(item, op) == 0
        assert lib.clay_layer_add_item_in_group(doc, layer.value, parent, -1, item, None) == 0
        lib.clay_item_destroy(item)

    add(plate.value, (0.9,), (0.0, 0.0, 0.0), CLAY_OP_ADD)
    add(plate.value, (0.7,), (0.55, 0.0, 0.0), CLAY_OP_INTERSECT)
    # an inline group nested inside it: its children apply to the plate's chain
    inner = ctypes.c_uint32(0)
    assert lib.clay_layer_add_group(doc, layer.value, plate.value, -1, CLAY_OP_INLINE,
                                    CLAY_BLEND_HARD, 0.0, 0.0, ctypes.byref(inner)) == 0
    add(inner.value, (0.25,), (0.55, 0.5, 0.0), CLAY_OP_SUBTRACT)

    item = lib.clay_item_create(CLAY_PRIM_SPHERE, floats(0.4), 1)
    assert lib.clay_item_set_position(item, floats(-1.5, 0.0, 0.0)) == 0
    assert lib.clay_layer_add_item(doc, layer.value, item, None) == 0
    lib.clay_item_destroy(item)

    assert lib.clay_document_save(doc, str(path).encode()) == 0
    lib.clay_document_destroy(doc)


def group_document_through_pyclay():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    plate = layer.add_group(op=clay.Op.ADD, blend=clay.Smooth(0.08))
    layer.add(clay.Sphere(r=0.9), parent=plate)
    layer.add(clay.Sphere(r=0.7, position=(0.55, 0, 0)), op=clay.Op.INTERSECT, parent=plate)
    inner = layer.add_group(op=clay.Op.INLINE, parent=plate)
    layer.add(clay.Sphere(r=0.25, position=(0.55, 0.5, 0)), op=clay.Op.SUBTRACT, parent=inner)
    layer.add(clay.Sphere(r=0.4, position=(-1.5, 0, 0)))
    return doc


def test_grouped_construction_matches_pyclay(lib, tmp_path):
    """The python-bindings scenario: the same grouped construction, both ways."""
    path = tmp_path / "groups_from_c.clayspace"
    group_document_through_c(lib, path)
    from_c = clay.load(str(path))
    from_python = group_document_through_pyclay()

    pts = sample_points()
    c_d, py_d = from_c.eval(pts), from_python.eval(pts)
    assert np.allclose(c_d, py_d, atol=1e-6), report(c_d, py_d, "distance")

    # and the intersect stayed inside the group, which is the whole point
    outside = np.array([[-1.5, 0.0, 0.0]], dtype=np.float32)
    assert from_c.eval(outside)[0] < 0.0


def test_group_children_read_back_through_both(lib):
    """The size-query enumeration, over ctypes and in pyclay, on the same tree.

    Built twice rather than saved and reloaded: pyclay has no accessor that
    hands back an SDF Layer of a document it did not just build, so a reloaded
    document can be evaluated from Python but not walked. That gap is older
    than groups and is not one this surface can close.
    """
    doc = lib.clay_document_create()
    try:
        layer = ctypes.c_uint32(0)
        assert lib.clay_add_sdf_layer(doc, b"body", ctypes.byref(layer)) == 0
        plate = ctypes.c_uint32(0)
        assert lib.clay_layer_add_group(doc, layer.value, 0, -1, CLAY_OP_ADD, CLAY_BLEND_HARD,
                                        0.0, 0.0, ctypes.byref(plate)) == 0
        ids = []
        for radius in (0.9, 0.7):
            item = lib.clay_item_create(CLAY_PRIM_SPHERE, floats(radius), 1)
            node = ctypes.c_uint32(0)
            assert lib.clay_layer_add_item_in_group(doc, layer.value, plate.value, -1, item,
                                                    ctypes.byref(node)) == 0
            lib.clay_item_destroy(item)
            ids.append(node.value)

        count = ctypes.c_size_t(0)
        assert lib.clay_layer_children(doc, layer.value, plate.value, None,
                                       ctypes.byref(count)) == CLAY_OK
        assert count.value == 2
        buf = (ctypes.c_uint32 * 2)()
        assert lib.clay_layer_children(doc, layer.value, plate.value, buf,
                                       ctypes.byref(count)) == CLAY_OK
        assert list(buf) == ids
        count = ctypes.c_size_t(1)
        assert lib.clay_layer_children(doc, layer.value, plate.value, buf,
                                       ctypes.byref(count)) == CLAY_ERROR_BUFFER_TOO_SMALL
        assert count.value == 2
        assert lib.clay_layer_children(doc, layer.value, ids[0], None,
                                       ctypes.byref(count)) == CLAY_ERROR_INVALID_ARGUMENT
    finally:
        lib.clay_document_destroy(doc)

    py_doc = clay.Document()
    py_layer = py_doc.add_sdf_layer("body")
    py_plate = py_layer.add_group()
    py_ids = [py_layer.add(clay.Sphere(r=r), parent=py_plate) for r in (0.9, 0.7)]
    assert py_layer.children(py_plate) == py_ids
    with pytest.raises(ValueError, match="not a group"):
        py_layer.children(py_ids[0])

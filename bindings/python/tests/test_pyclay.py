# pyclay end-to-end suite (python-bindings spec + task 11.4): the golden
# body scene from docs/05 section 10, evaluation at known points, meshing
# gates, .clayspace round trip, mesh export, backend errors, and vectorized
# property tests (conservative stepping, gradient normalization).

import numpy as np
import pytest

import pyclay as clay


def build_body():
    """The spec-sample body scene (docs/05 section 10, tape-supported subset)."""
    doc = clay.Document()
    body = doc.add_sdf_layer("body", resolution=256)
    body.add(clay.Sphere(r=1.0), blend=clay.Smooth(0.2), color="#38a6cf")
    body.add(
        clay.Capsule(a=(0, 0.8, 0), b=(0, 1.6, 0), r=0.35),
        op=clay.Op.ADD,
        blend=clay.Chamfer(0.1),
    )
    body.add(clay.Box(size=(0.4, 0.4, 0.4), position=(0, 0, 0.95)), op=clay.Op.SUBTRACT)
    body.add(
        clay.RoundCone(r1=0.28, r2=0.12, h=0.7,
                       position=(0.7, 0.2, 0),
                       rotation_axis_angle=((0, 0, 1), -1.2)),
        blend=clay.Smooth(0.12),
        color=(0.9, 0.7, 0.3),
        mirror=True,
    )
    body.mirror(axis="x", blend=0.15)
    return doc, body


def test_sphere_known_distances():
    doc = clay.Document()
    layer = doc.add_sdf_layer("s")
    layer.add(clay.Sphere(r=1.0))
    pts = np.array([[0, 0, 0], [2, 0, 0], [0, 1, 0]], dtype=np.float32)
    d = layer.eval(pts)
    assert d.dtype == np.float32 and d.shape == (3,)
    assert d == pytest.approx([-1.0, 1.0, 0.0], abs=1e-6)
    # float64 input is accepted (converted) and evaluates identically
    d64 = layer.eval(pts.astype(np.float64))
    assert np.array_equal(d, d64)
    # non-contiguous input is accepted too
    dt = layer.eval(np.ascontiguousarray(pts.T).T)
    assert np.array_equal(d, dt)


def test_prim_placement_and_at():
    doc = clay.Document()
    layer = doc.add_sdf_layer("s")
    layer.add(clay.Sphere(r=0.5).at((2, 0, 0)))
    layer.add(clay.Box(size=(1, 1, 1), position=(-2, 0, 0)), color=(1, 0, 0))
    d = doc.eval(np.array([[2, 0, 0], [-2, 0, 0], [0, 0, 0]], np.float32))
    assert d[0] == pytest.approx(-0.5, abs=1e-6)
    assert d[1] == pytest.approx(-0.5, abs=1e-6)  # box side 1 -> half extent 0.5
    assert d[2] > 0


def test_body_scene_eval():
    doc, body = build_body()
    pts = np.array(
        [[0, 0, 0], [5, 5, 5], [0.7, 0.2, 0], [-0.7, 0.2, 0]], dtype=np.float32
    )
    d = doc.eval(pts)
    assert d[0] < 0  # inside the sphere
    assert d[1] > 4  # far outside
    assert d[2] < 0  # inside the arm
    assert d[3] < 0  # inside the MIRRORED arm
    # the whole scene is x-symmetric: mirrored evaluation matches
    rng = np.random.default_rng(7)
    p = rng.uniform(-2, 2, size=(512, 3)).astype(np.float32)
    q = p * np.array([-1, 1, 1], dtype=np.float32)
    assert doc.eval(p) == pytest.approx(doc.eval(q), abs=1e-5)


def test_layer_colors_and_document_colors():
    doc, body = build_body()
    c = doc.colors(np.array([[0, 0, 0]], np.float32))
    assert c.shape == (1, 3) and c.dtype == np.float32
    # sphere color "#38a6cf"
    assert c[0] == pytest.approx([0x38 / 255, 0xA6 / 255, 0xCF / 255], abs=0.05)
    cl = body.colors(np.array([[0, 0, 0]], np.float32))
    assert cl.shape == (1, 3)


def test_mesh_watertight_manifold():
    doc, _ = build_body()
    mesh = doc.mesh(resolution=96)
    assert mesh.triangle_count > 0
    assert mesh.positions.shape[1] == 3 and mesh.positions.dtype == np.float32
    assert mesh.indices.shape[1] == 3 and mesh.indices.dtype == np.uint32
    assert mesh.normals.shape == mesh.positions.shape
    assert mesh.colors.shape == mesh.positions.shape
    assert mesh.is_watertight()
    assert mesh.is_manifold()
    # decimation keeps the gates green and reduces triangles
    small = doc.mesh(resolution=96, decimate=0.5)
    assert 0 < small.triangle_count < mesh.triangle_count
    assert small.is_watertight() and small.is_manifold()


def test_clayspace_round_trip(tmp_path):
    doc, _ = build_body()
    path = str(tmp_path / "body.clayspace")
    doc.save(path)
    loaded = clay.load(path)
    rng = np.random.default_rng(11)
    pts = rng.uniform(-2, 2, size=(1024, 3)).astype(np.float32)
    # identical evaluation, bit for bit (Authoring flow scenario)
    assert np.array_equal(doc.eval(pts), loaded.eval(pts))
    assert np.array_equal(doc.colors(pts), loaded.colors(pts))


def test_mesh_export(tmp_path):
    doc, _ = build_body()
    mesh = doc.mesh(resolution=64)
    for ext in ("obj", "ply", "fbx", "glb"):
        out = tmp_path / f"body.{ext}"
        mesh.save(str(out))
        assert out.is_file() and out.stat().st_size > 0
    with pytest.raises(ValueError, match="extension"):
        mesh.save(str(tmp_path / "body.unsupported"))


def test_backend_listing_and_unknown_backend():
    names = clay.backends()
    assert "cpu" in names
    doc, body = build_body()
    pts = np.zeros((1, 3), np.float32)
    with pytest.raises(ValueError) as err:
        doc.eval(pts, backend="not-a-backend")
    assert "not-a-backend" in str(err.value)
    for name in names:  # the error lists every available backend
        assert name in str(err.value)
    with pytest.raises(ValueError):
        body.gradients(pts, backend="nope")
    with pytest.raises(ValueError):
        doc.mesh(backend="nope")


def test_every_registered_backend_matches_cpu():
    """Backend availability changes speed, never results (evaluation-backends
    spec). Runs against whatever is registered in this build: cuda on an
    NVIDIA machine, opencl where a device exists, metal on Apple."""
    doc, _ = build_body()
    rng = np.random.default_rng(20260803)
    pts = rng.uniform(-2.5, 2.5, size=(4096, 3)).astype(np.float32)
    reference = doc.eval(pts, backend="cpu")
    for name in clay.backends():
        if name == "cpu":
            continue
        got = doc.eval(pts, backend=name)
        scale = np.maximum(np.maximum(np.abs(reference), np.abs(got)), 1.0)
        worst = float(np.max(np.abs(reference - got) / scale))
        assert worst <= 1e-4, f"{name} deviates from the cpu reference by {worst}"


def test_conservative_steps_property():
    # Sphere tracing safety: stepping by distance * safe_step_scale from an
    # outside point can never cross the surface (vectorized numpy sampling).
    doc, _ = build_body()
    scale = doc.safe_step_scale()
    assert 0 < scale <= 1.0
    rng = np.random.default_rng(3)
    p = rng.uniform(-2.5, 2.5, size=(4096, 3)).astype(np.float32)
    d = doc.eval(p)
    outside = d > 1e-3
    assert outside.sum() > 100
    dirs = rng.normal(size=(4096, 3)).astype(np.float32)
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    step = 0.99 * scale * d[outside, None]
    d2 = doc.eval(p[outside] + dirs[outside] * step)
    assert (d2 >= -1e-4).all()


def test_gradient_normalization():
    doc, _ = build_body()
    rng = np.random.default_rng(5)
    p = rng.uniform(-2, 2, size=(2048, 3)).astype(np.float32)
    g = doc.gradients(p)
    assert g.shape == (2048, 3) and g.dtype == np.float32
    norms = np.linalg.norm(g, axis=1)
    assert norms == pytest.approx(np.ones(2048), abs=1e-3)
    # direction sanity on an isolated sphere: gradient points radially out
    sdoc = clay.Document()
    sdoc.add_sdf_layer("s").add(clay.Sphere(r=1.0))
    q = np.array([[2, 0, 0], [0, -3, 0], [0, 0, 1.5]], np.float32)
    gs = sdoc.gradients(q)
    expected = q / np.linalg.norm(q, axis=1, keepdims=True)
    assert gs == pytest.approx(expected, abs=1e-3)


def test_all_primitives_mesh():
    # every bound primitive evaluates and meshes watertight on its own
    prims = [
        clay.Sphere(r=0.8),
        clay.Box(size=(1, 0.8, 0.6)),
        clay.RoundBox(size=(1, 1, 1), r=0.1),
        clay.Torus(R=0.7, r=0.2),
        clay.Capsule(a=(0, -0.4, 0), b=(0, 0.4, 0), r=0.3),
        clay.Cylinder(r=0.4, h=0.5),
        clay.Cone(h=0.5, r1=0.5, r2=0.2),
        clay.RoundCone(r1=0.4, r2=0.15, h=0.7),
        clay.Ellipsoid(r=(0.8, 0.5, 0.4)),
        clay.Octahedron(s=0.8),
        clay.HexPrism(hx=0.5, hy=0.4),
        clay.Pyramid(h=0.9),
    ]
    for prim in prims:
        doc = clay.Document()
        doc.add_sdf_layer("p").add(prim)
        assert doc.eval(np.array([[4, 4, 4]], np.float32))[0] > 0
        mesh = doc.mesh(resolution=48)
        assert mesh.is_watertight() and mesh.is_manifold(), type(prim).__name__


# --- widened surface: strokes, extended ops, voxels, meshers, picking --------


def test_stroke_is_one_item_and_matches_field():
    doc = clay.Document()
    body = doc.add_sdf_layer("body")
    stroke = clay.Stroke(points=[(-1.0, 0.0, 0.0, 0.30),
                                 (0.0, 0.5, 0.0, 0.22),
                                 (1.0, 0.0, 0.0, 0.30)], blend_k=0.05)
    node = body.add(stroke)
    assert isinstance(node, int)
    assert stroke.point_count == 3
    assert stroke.points.shape == (3, 4)

    # inside the chain near its middle point, outside far away
    pts = np.array([[0.0, 0.5, 0.0], [0.0, 3.0, 0.0]], dtype=np.float32)
    d = doc.eval(pts)
    assert d[0] < 0 < d[1]

    # incremental authoring appends to the same item
    tail = clay.Stroke().add_point((2.0, 0.0, 0.0), 0.25).add_point((3.0, 0.0, 0.0), 0.2)
    assert tail.point_count == 2
    body.add(tail)
    assert doc.eval(np.array([[2.5, 0.0, 0.0]], dtype=np.float32))[0] < 0


def test_stroke_numpy_batch_form():
    pts = np.array([[0, 0, 0, 0.3], [1, 0, 0, 0.3]], dtype=np.float32)
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Stroke(points=pts))
    assert doc.eval(np.array([[0.5, 0, 0]], dtype=np.float32))[0] < 0
    with pytest.raises(ValueError, match=r"\(N, 4\)"):
        clay.Stroke(points=np.zeros((2, 3), np.float32))


@pytest.mark.parametrize("op", ["GROOVE", "TONGUE", "PIPE", "ENGRAVE",
                                "EMBOSS", "INSET", "SHELL", "REPLACE",
                                "RELIEF", "INCISE"])
def test_extended_ops_evaluate_and_round_trip(op, tmp_path):
    doc = clay.Document()
    body = doc.add_sdf_layer("body")
    body.add(clay.Sphere(r=1.0))
    body.add(clay.Box(size=(0.6, 0.6, 3.0), position=(0.7, 0, 0)),
             op=getattr(clay.Op, op), blend=clay.Smooth(0.12), rounding=0.05)

    pts = np.array([[0, 0, 0], [0.7, 0, 0], [4, 0, 0]], dtype=np.float32)
    before = doc.eval(pts)
    assert np.all(np.isfinite(before))
    assert before[2] > 0  # far outside stays outside for every mode

    path = tmp_path / f"{op.lower()}.clayspace"
    doc.save(str(path))
    after = clay.load(str(path)).eval(pts)
    assert np.array_equal(before, after)  # op survives the document round trip


def test_extended_ops_differ_from_each_other():
    # These modes deviate only where the second operand's SURFACE crosses the
    # accumulated one, so compare over a cloud around that crossing curve
    # rather than at hand-picked points.
    rng = np.random.default_rng(11)
    cloud = rng.uniform(-1.4, 1.4, size=(4096, 3)).astype(np.float32)

    def field(op):
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Sphere(r=1.0))
        layer.add(clay.Box(size=(0.5, 0.5, 3.0), position=(0.9, 0, 0)),
                  op=op, blend=clay.Smooth(0.15), rounding=0.08)
        return doc.eval(cloud)

    # baseline is the ACCUMULATED field (the sphere alone): engrave/emboss
    # deviate from that, not from a plain union with the second operand
    base_doc = clay.Document()
    base_doc.add_sdf_layer("l").add(clay.Sphere(r=1.0))
    base = base_doc.eval(cloud)

    engrave = field(clay.Op.ENGRAVE)
    emboss = field(clay.Op.EMBOSS)
    groove = field(clay.Op.GROOVE)

    assert not np.allclose(groove, base)
    assert not np.allclose(engrave, emboss)
    # engrave only carves (field rises), emboss only adds (field falls)
    assert np.all(engrave >= base - 1e-5)
    assert np.all(emboss <= base + 1e-5)


def test_relief_and_incise_are_a_region_not_a_shape():
    # The distinction that makes these two ops different from every other one:
    # the item is a REGION selecting where the already-accumulated surface moves,
    # not geometry that is itself added or subtracted.
    def ball(op=None):
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Sphere(r=0.7))
        if op is not None:
            layer.add(clay.Sphere(r=0.35, position=(0, 0.7, 0)),
                      op=op, blend=clay.Smooth(0.12), rounding=0.25)
        return doc

    def top(doc):
        ys = np.arange(0.2, 1.4, 0.001, dtype=np.float32)
        pts = np.stack([np.zeros_like(ys), ys, np.zeros_like(ys)], axis=1)
        return float(ys[np.nonzero(doc.eval(pts) > 0.0)[0][0]])

    base = top(ball())
    up = top(ball(clay.Op.RELIEF))
    down = top(ball(clay.Op.INCISE))
    # Offsetting a distance field moves its isosurface along its own gradient by
    # exactly the offset, so both directions land the full amplitude away.
    assert up - base == pytest.approx(0.12, abs=0.01)
    assert base - down == pytest.approx(0.12, abs=0.01)

    # A relief item on its own has nothing to displace, so it is not a surface.
    solo = clay.Document()
    solo.add_sdf_layer("l").add(clay.Sphere(r=0.35), op=clay.Op.RELIEF,
                                blend=clay.Smooth(0.12), rounding=0.25)
    rng = np.random.default_rng(4)
    assert np.all(solo.eval(rng.uniform(-1, 1, size=(2000, 3)).astype(np.float32)) > 0)


def test_relief_support_is_finite_and_costs_the_marcher():
    def ball(op=None, amplitude=0.12, width=0.25):
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Sphere(r=0.7))
        if op is not None:
            layer.add(clay.Sphere(r=0.35, position=(0, 0.7, 0)),
                      op=op, blend=clay.Smooth(amplitude), rounding=width)
        return doc

    rng = np.random.default_rng(9)
    probes = rng.uniform(-1.5, 1.5, size=(8000, 3)).astype(np.float32)
    # Radius + rounding + falloff: the rounding does double duty here exactly as
    # it does for groove and tongue — it rounds the region's own field AND is the
    # falloff width, so the taper sits outside the rounded surface.
    outside = np.linalg.norm(probes - np.array([0, 0.7, 0], np.float32), axis=1) > 0.85
    delta = np.abs(ball(clay.Op.RELIEF).eval(probes[outside])
                   - ball().eval(probes[outside]))
    assert delta.max() < 1e-5, "influence bounds and brick culling trust this"

    # Offsetting the distance is not distance preserving, and the tape says so.
    assert ball().safe_step_scale() > ball(clay.Op.RELIEF).safe_step_scale()
    steep = ball(clay.Op.RELIEF, amplitude=0.2, width=0.1)
    assert steep.safe_step_scale() < ball(clay.Op.RELIEF).safe_step_scale()


def _bump_and_dent():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.8))
    layer.add(clay.Sphere(r=0.28, position=(0.25, 0.72, 0)))
    layer.add(clay.Sphere(r=0.26, position=(-0.3, 0.66, 0)), op=clay.Op.SUBTRACT)
    return doc


def _flat_top(doc, x):
    ys = np.arange(1.4, -0.2, -0.002, dtype=np.float32)
    pts = np.stack([np.full_like(ys, x), ys, np.zeros_like(ys)], axis=1)
    out = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(ys[out[0]]) if len(out) else float("nan")


def _flattened(src, mode):
    vol = clay.Volume.flattened_from(src, plane_point=(0, 0.78, 0), plane_normal=(0, 1, 0),
                                     strength=1.0, centre=(0, 0.78, 0), region_radius=0.9,
                                     falloff=0.25, cell=0.01, band=0.12, mode=mode)
    doc = clay.Document()
    doc.add_sdf_layer("f").add(vol)
    return doc


def test_flatten_cut_only_is_hpolish():
    """Cutting WITHOUT filling is the whole hard-surface brush.

    It is what leaves a crisp facet against untouched surface; filling the
    hollows beside a facet is what a polish must not do.
    """
    src = _bump_and_dent()
    dent_x, bump_x, plane = -0.30, 0.25, 0.778

    cut = _flattened(src, "cut")
    assert _flat_top(cut, dent_x) == pytest.approx(_flat_top(src, dent_x), abs=1e-3)
    assert _flat_top(cut, bump_x) == pytest.approx(plane, abs=0.01)


def test_flatten_fill_only_is_the_dual():
    src = _bump_and_dent()
    dent_x, bump_x, plane = -0.30, 0.25, 0.778

    fill = _flattened(src, "fill")
    assert _flat_top(fill, dent_x) == pytest.approx(plane, abs=0.01)
    assert _flat_top(fill, bump_x) == pytest.approx(_flat_top(src, bump_x), abs=1e-3)


def test_flatten_defaults_to_two_sided():
    src = _bump_and_dent()
    rng = np.random.default_rng(3)
    probes = rng.uniform(-1.0, 1.2, size=(3000, 3)).astype(np.float32)

    implicit = clay.Volume.flattened_from(src, plane_point=(0, 0.78, 0),
                                          plane_normal=(0, 1, 0), strength=1.0,
                                          centre=(0, 0.78, 0), region_radius=0.9,
                                          falloff=0.25, cell=0.01, band=0.12)
    doc = clay.Document(); doc.add_sdf_layer("f").add(implicit)
    assert np.array_equal(doc.eval(probes), _flattened(src, "two_sided").eval(probes))
    # ...and both sides really do move, which is what makes it two-sided.
    two = _flattened(src, "two_sided")
    assert _flat_top(two, -0.30) > _flat_top(src, -0.30) + 0.2     # hollow filled
    assert _flat_top(two, 0.25) < _flat_top(src, 0.25) - 0.1       # bump cut


def test_flatten_refuses_an_unknown_mode():
    src = _bump_and_dent()
    with pytest.raises(ValueError, match="two_sided"):
        clay.Volume.flattened_from(src, plane_point=(0, 0.78, 0), plane_normal=(0, 1, 0),
                                   region_radius=0.9, falloff=0.25, cell=0.02,
                                   mode="polish")


def test_voxel_grid_edits_and_queries():
    g = clay.VoxelGrid(voxel_size=0.5)
    red = g.palette_add("#ff0000")
    blue = g.palette_add((0.0, 0.0, 1.0))
    assert red != blue and g.palette_size >= 3

    g.set((0, 0, 0), red)
    assert g.get((0, 0, 0)) == red
    assert g.occupied_count == 1

    g.set_brush((5, 5, 5), 3, blue)
    assert g.occupied_count == 1 + 27

    g.fill_box((10, 0, 0), (12, 1, 0), red)
    assert g.get((11, 1, 0)) == red
    g.fill_line((20, 0, 0), (25, 0, 0), red)
    assert g.get((23, 0, 0)) == red

    g.erase((0, 0, 0))
    assert g.get((0, 0, 0)) == 0

    # paint only affects occupied cells; palette recolor leaves data alone
    g.paint((99, 99, 99), blue)
    assert g.get((99, 99, 99)) == 0
    g.palette_set(red, "#00ff00")
    assert g.palette_color(red)[1] > 0.9
    assert g.get((23, 0, 0)) == red

    lo, hi = g.bounds()
    assert lo[0] <= 4 and hi[0] >= 25


def test_voxel_batch_coordinate_form():
    g = clay.VoxelGrid(0.25)
    idx = g.palette_add("#123456")
    cells = np.array([[0, 0, 0], [1, 0, 0], [2, 0, 0], [-3, 4, -5]], dtype=np.int32)
    g.set_many(cells, idx)
    assert g.occupied_count == 4
    assert g.get((-3, 4, -5)) == idx
    g.erase_many(cells[:2])
    assert g.occupied_count == 2


def test_voxel_mirror_and_flood_select():
    g = clay.VoxelGrid(0.5)
    idx = g.palette_add("#ffffff")
    g.set_mirrored((3, 2, 1), idx, axes="xz")
    assert g.occupied_count == 4
    assert g.get((-4, 2, -2)) == idx

    g2 = clay.VoxelGrid(0.5)
    a = g2.palette_add("#ff0000")
    b = g2.palette_add("#0000ff")
    g2.fill_box((0, 0, 0), (3, 0, 0), a)
    g2.set((4, 0, 0), b)
    g2.fill_box((5, 0, 0), (8, 0, 0), a)
    assert g2.flood_select((0, 0, 0), same_color=True).shape[0] == 4
    assert g2.flood_select((0, 0, 0), same_color=False).shape[0] == 9


def test_voxel_greedy_mesh_and_step_field():
    g = clay.VoxelGrid(1.0)
    idx = g.palette_add("#3355ff")
    g.fill_box((0, 0, 0), (3, 3, 3), idx)
    m = g.mesh()
    assert m.triangle_count > 0
    # a solid 4^3 block exposes 6*16 unit faces; greedy merging beats that
    assert m.triangle_count < 6 * 16 * 2
    assert m.colors.shape == m.positions.shape

    inside = g.sample_step_field(np.array([[0.5, 0.5, 0.5]], dtype=np.float32))
    outside = g.sample_step_field(np.array([[-5.0, 0.5, 0.5]], dtype=np.float32))
    assert inside[0] < 0 < outside[0]


def test_voxel_layer_in_document_round_trip(tmp_path):
    doc = clay.Document()
    grid = doc.add_voxel_layer("blocks", voxel_size=0.2)
    idx = grid.palette_add("#ff8800")
    grid.fill_box((0, 0, 0), (4, 2, 1), idx)
    assert grid.occupied_count == 5 * 3 * 2

    path = tmp_path / "voxels.clayspace"
    doc.save(str(path))
    loaded = clay.load(str(path))
    back = loaded.voxel_layer("blocks")
    assert back is not None
    assert back.occupied_count == grid.occupied_count
    assert back.get((2, 1, 0)) == idx
    assert back.palette_color(idx)[0] > 0.9


def test_sdf_rasterized_into_voxels():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.5), color="#ff0000")
    g = clay.VoxelGrid(0.1)
    g.rasterize(doc)
    assert g.occupied_count > 0
    volume = g.occupied_count * 0.1 ** 3
    assert volume == pytest.approx(4.0 / 3.0 * np.pi * 0.5 ** 3, rel=0.2)


def test_mesher_selection():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0))
    layer.add(clay.Box(size=(1.0, 1.0, 1.0), position=(0.8, 0, 0)), blend=clay.Smooth(0.15))

    marching = doc.mesh(resolution=48)
    nets = doc.mesh(resolution=48, mesher="nets")
    assert marching.triangle_count > 0 and nets.triangle_count > 0
    assert nets.triangle_count < marching.triangle_count  # the preview claim

    with pytest.raises(ValueError, match="experimental"):
        doc.mesh(resolution=32, mesher="dual_contouring")
    dc = doc.mesh(resolution=32, mesher="dual_contouring", experimental=True)
    assert dc.triangle_count > 0

    with pytest.raises(ValueError, match="mesher"):
        doc.mesh(resolution=32, mesher="nope")


def test_scene_picking_attributes_hits():
    doc = clay.Document()
    body = doc.add_sdf_layer("body")
    sphere = body.add(clay.Sphere(r=1.0))
    other = doc.add_sdf_layer("other")
    box = other.add(clay.Box(size=(0.8, 0.8, 0.8), position=(4, 0, 0)))

    hit = doc.raycast((0, 0, -5), (0, 0, 1))
    assert hit is not None
    assert hit["t"] == pytest.approx(4.0, abs=1e-2)
    assert hit["item"] == sphere
    assert hit["position"][2] == pytest.approx(-1.0, abs=1e-2)
    assert hit["normal"][2] < -0.9

    far = doc.raycast((4, 0, -5), (0, 0, 1))
    assert far is not None and far["item"] == box and far["layer"] != hit["layer"]
    assert doc.raycast((0, 9, -5), (0, 0, 1)) is None


def test_batch_raycast_matches_scalar():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=1.0))
    rays = np.array([[0, 0, -5, 0, 0, 1],
                     [0, 0, 5, 0, 0, -1],
                     [0, 9, -5, 0, 0, 1]], dtype=np.float32)
    out = doc.raycast_many(rays)
    assert out["hit"].tolist() == [True, True, False]
    assert out["t"][0] == pytest.approx(4.0, abs=1e-2)
    assert out["position"].shape == (3, 3) and out["normal"].shape == (3, 3)
    scalar = doc.raycast((0, 0, -5), (0, 0, 1))
    assert out["t"][0] == pytest.approx(scalar["t"], abs=1e-4)


def test_snap_to_surface_batch():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=1.0))
    rng = np.random.default_rng(7)
    dirs = rng.normal(size=(64, 3))
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    starts = (dirs * rng.uniform(0.85, 1.15, size=(64, 1))).astype(np.float32)

    snapped = doc.snap_to_surface(starts)
    on_surface = np.abs(doc.eval(snapped["position"]))
    assert np.all(on_surface < 1e-3)
    # outward normals point along the radius for a sphere
    assert np.all(np.sum(snapped["normal"] * dirs, axis=1) > 0.99)


def test_voxel_picking_cell_and_face():
    g = clay.VoxelGrid(0.5)
    idx = g.palette_add("#ffffff")
    g.fill_box((0, 0, 0), (2, 2, 2), idx)

    hit = g.raycast((5, 0.75, 0.75), (-1, 0, 0))
    assert hit is not None
    assert hit["cell"] == (2, 1, 1)
    assert hit["adjacent"] == (3, 1, 1)
    assert g.get(hit["adjacent"]) == 0  # placement target is empty

    assert g.raycast((5, 5, 5), (1, 0, 0)) is None
    assert g.build_plane_pick((0.6, 4, 0.6), (0, -1, 0), 0) == (1, 0, 1)


def test_selection_and_layer_bounds():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    a = layer.add(clay.Sphere(r=0.5, position=(-1, 0, 0)))
    b = layer.add(clay.Box(size=(0.6, 0.6, 0.6), position=(2, 0, 0)),
                  blend=clay.Smooth(0.4))

    sel = layer.selection_bounds([a])
    assert sel[0][0] == pytest.approx(-1.5) and sel[1][0] == pytest.approx(-0.5)

    both = layer.selection_bounds([a, b])
    assert both[1][0] == pytest.approx(2.3)
    # tight bounds: no blend-support dilation
    assert both[1][0] < 2.3 + 1e-3
    assert layer.bounds()[1][0] == pytest.approx(both[1][0])


# --- deformers (add-tape-deformers) -----------------------------------------


def test_docs_sample_twist_line_works():
    # the exact construct from docs/05 section 10 that had no tape opcode
    doc = clay.Document()
    body = doc.add_sdf_layer("body")
    body.add(clay.Sphere(r=1.0), color="#38a6cf")
    body.add(clay.Box(size=(0.4, 0.4, 0.4)).twist(1.2), op=clay.Op.SUBTRACT)

    pts = np.array([[0, 0, 0], [3, 0, 0]], dtype=np.float32)
    d = doc.eval(pts)
    assert d[1] > 0
    mesh = doc.mesh(resolution=48)
    assert mesh.is_watertight()


def test_deformer_chain_inspection_and_order():
    prim = clay.Box(size=(0.8, 2.0, 0.8)).twist(1.2).bend(0.7)
    chain = prim.deformers
    assert [d["type"] for d in chain] == ["twist", "bend"]
    assert chain[0]["k"] == pytest.approx(1.2)

    def field(p):
        doc = clay.Document()
        doc.add_sdf_layer("l").add(p)
        rng = np.random.default_rng(3)
        pts = rng.uniform(-1.5, 1.5, size=(512, 3)).astype(np.float32)
        return doc.eval(pts)

    forward = field(clay.Box(size=(0.8, 2.0, 0.8)).twist(1.2).bend(0.7))
    reverse = field(clay.Box(size=(0.8, 2.0, 0.8)).bend(0.7).twist(1.2))
    assert not np.allclose(forward, reverse)  # twist and bend do not commute


def test_taper_and_displace_from_python():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Cylinder(r=0.6, h=1.0).taper(-1.0, 1.0, 1.0, 0.35))
    # the tapered top is narrower than the base
    wide_bottom = doc.eval(np.array([[0.5, -0.9, 0.0]], dtype=np.float32))[0]
    same_at_top = doc.eval(np.array([[0.5, 0.9, 0.0]], dtype=np.float32))[0]
    assert wide_bottom < 0 < same_at_top

    bumpy = clay.Document()
    bumpy.add_sdf_layer("l").add(clay.Sphere(r=1.0).displace(0.08, 6.0))
    assert bumpy.safe_step_scale() < 1.0  # tracked Lipschitz drops

    with pytest.raises(ValueError, match="taper scales"):
        clay.Sphere(r=1.0).taper(-1.0, 1.0, 0.0, 1.0)


def test_deformers_survive_clayspace_round_trip(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(0.8, 2.0, 0.8)).twist(0.9).taper(-1.0, 1.0, 1.0, 0.5))

    rng = np.random.default_rng(5)
    pts = rng.uniform(-2, 2, size=(256, 3)).astype(np.float32)
    before = doc.eval(pts)

    path = tmp_path / "deformed.clayspace"
    doc.save(str(path))
    after = clay.load(str(path)).eval(pts)
    assert np.array_equal(before, after)


def test_wrap_around_needs_a_non_degenerate_interval():
    # The interval fixes the cylinder radius, so x0 == x1 has no meaning.
    with pytest.raises(ValueError, match="x0 != x1"):
        clay.Sphere(r=1.0).wrap_around(1.0, 1.0)


# --- transition morphs (add-transition-morphs) ------------------------------


def test_transition_linear_morphs_between_shapes():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.8), color="#ff0000")
    layer.add(clay.Box(size=(1.2, 1.2, 1.2)), op=clay.Op.TRANSITION_LINEAR,
              transition=clay.TransitionLinear(a=(0, -2, 0), b=(0, 2, 0)), color="#0000ff")

    # at the segment start the sphere wins outright, at the end the box does
    start = np.array([[0.3, -2.0, 0.0]], dtype=np.float32)
    end = np.array([[0.3, 2.0, 0.0]], dtype=np.float32)
    sphere_only = clay.Document()
    sphere_only.add_sdf_layer("s").add(clay.Sphere(r=0.8))
    box_only = clay.Document()
    box_only.add_sdf_layer("b").add(clay.Box(size=(1.2, 1.2, 1.2)))

    assert doc.eval(start)[0] == pytest.approx(sphere_only.eval(start)[0], abs=1e-5)
    assert doc.eval(end)[0] == pytest.approx(box_only.eval(end)[0], abs=1e-5)
    # colors follow the same weight
    assert doc.colors(start)[0][0] > 0.99
    assert doc.colors(end)[0][2] > 0.99


def test_transition_radial_and_easing():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.7))
    layer.add(clay.Box(size=(1.4, 1.4, 1.4)), op=clay.Op.TRANSITION_RADIAL,
              transition=clay.TransitionRadial(r0=0.5, r1=2.0, ease=1))
    pts = np.array([[0.0, 0.0, 0.0], [3.0, 0.0, 0.0]], dtype=np.float32)
    assert np.all(np.isfinite(doc.eval(pts)))
    assert doc.safe_step_scale() < 1.0  # a lerp of fields is not a distance


def test_transition_requires_its_parameters():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0))
    with pytest.raises(ValueError, match="transition"):
        layer.add(clay.Box(size=(1, 1, 1)), op=clay.Op.TRANSITION_LINEAR)
    with pytest.raises(ValueError, match="TransitionLinear parameters"):
        layer.add(clay.Box(size=(1, 1, 1)), op=clay.Op.TRANSITION_LINEAR,
                  transition=clay.TransitionRadial(r0=0.0, r1=1.0))
    with pytest.raises(ValueError, match="only applies to transition ops"):
        layer.add(clay.Box(size=(1, 1, 1)), transition=clay.TransitionRadial(r0=0.0, r1=1.0))


def test_transition_round_trip(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.9))
    layer.add(clay.Torus(R=1.0, r=0.25), op=clay.Op.TRANSITION_LINEAR,
              transition=clay.TransitionLinear(a=(-1, 0, 0), b=(1.5, 0.5, 0), ease=2))
    rng = np.random.default_rng(21)
    pts = rng.uniform(-2, 2, size=(256, 3)).astype(np.float32)
    before = doc.eval(pts)

    path = tmp_path / "morph.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(pts))


# --- profiles and lifts (add-profile-lifts) ---------------------------------


def test_extruded_polygon_from_numpy(tmp_path):
    # an L-shaped outline: the kind of thing text and SVG produce
    outline = np.array([[-1, -1], [1, -1], [1, 0], [0, 0], [0, 1], [-1, 1]], dtype=np.float32)
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Extrude(clay.Profile.polygon(outline), half_depth=0.3), color="#c0d020")

    pts = np.array([[-0.5, -0.5, 0.0],   # solid arm
                    [0.5, 0.5, 0.0],     # the notch -> outside
                    [0.0, 0.0, 2.0]],    # past the depth
                   dtype=np.float32)
    d = doc.eval(pts)
    assert d[0] < 0 < d[1] and d[2] > 0

    mesh = doc.mesh(resolution=64)
    assert mesh.is_watertight() and mesh.is_manifold()
    mesh.save(str(tmp_path / "extruded.obj"))


def test_revolved_circle_matches_torus():
    revolved = clay.Document()
    revolved.add_sdf_layer("l").add(clay.Revolve(clay.Profile.circle(0.3), offset=1.1))
    torus = clay.Document()
    torus.add_sdf_layer("l").add(clay.Torus(R=1.1, r=0.3))

    rng = np.random.default_rng(31)
    pts = rng.uniform(-2.5, 2.5, size=(512, 3)).astype(np.float32)
    assert np.allclose(revolved.eval(pts), torus.eval(pts), atol=1e-5)


def test_all_profile_kinds_lift():
    profiles = [
        clay.Profile.circle(0.5),
        clay.Profile.box(0.4, 0.3),
        clay.Profile.hexagon(0.45),
        clay.Profile.triangle(0.5),
        clay.Profile.trapezoid(0.5, 0.25, 0.35),
        clay.Profile.vesica(0.6, 0.25),
        clay.Profile.polygon([(-0.4, -0.4), (0.4, -0.4), (0.0, 0.5)]),
    ]
    for profile in profiles:
        doc = clay.Document()
        doc.add_sdf_layer("l").add(clay.Extrude(profile, half_depth=0.25))
        assert doc.eval(np.array([[0.0, 0.0, 0.0]], dtype=np.float32))[0] < 0
        assert doc.eval(np.array([[5.0, 0.0, 0.0]], dtype=np.float32))[0] > 0
        assert doc.mesh(resolution=40).is_watertight()


def test_polygon_input_validation():
    with pytest.raises(ValueError, match=r"\(N, 2\)"):
        clay.Profile.polygon(np.zeros((4, 3), np.float32))
    with pytest.raises(ValueError, match="at least 3"):
        clay.Profile.polygon([(0, 0), (1, 1)])
    assert clay.Profile.polygon([(0, 0), (1, 0), (0, 1)]).point_count == 3


def test_lifts_round_trip_and_compose(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Revolve(clay.Profile.polygon([(-0.2, -0.3), (0.2, -0.3), (0.25, 0.3),
                                                 (-0.25, 0.3)]), offset=0.9))
    # lifts compose with everything else: blends, ops, deformers
    layer.add(clay.Extrude(clay.Profile.hexagon(0.35), half_depth=0.6).twist(0.8),
              op=clay.Op.SUBTRACT, blend=clay.Smooth(0.05))

    rng = np.random.default_rng(33)
    pts = rng.uniform(-2, 2, size=(256, 3)).astype(np.float32)
    before = doc.eval(pts)

    path = tmp_path / "lathe.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(pts))


# --- repetition (add-repetition) --------------------------------------------


def test_finite_grid_array():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.2).repeat_grid(spacing=1.0, counts=(2, 0, 0)))
    # copies at x = -2, -1, 0, 1, 2 and nothing at x = 3
    inside = np.array([[-2, 0, 0], [0, 0, 0], [2, 0, 0]], dtype=np.float32)
    assert np.all(doc.eval(inside) < 0)
    assert doc.eval(np.array([[3.0, 0, 0]], dtype=np.float32))[0] > 0


def test_infinite_grid_is_periodic():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.3).repeat_grid(spacing=2.0))
    rng = np.random.default_rng(41)
    pts = rng.uniform(-3, 3, size=(128, 3)).astype(np.float32)
    shifted = pts + np.array([2.0, 0.0, -4.0], dtype=np.float32)
    assert np.allclose(doc.eval(pts), doc.eval(shifted), atol=1e-4)


def test_radial_array_periodicity():
    count = 6
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.25).repeat_radial(count=count, offset=1.0))
    rng = np.random.default_rng(43)
    pts = rng.uniform(-2, 2, size=(128, 3)).astype(np.float32)
    angle = 2 * np.pi / count
    c, s = np.cos(angle), np.sin(angle)
    rotated = np.stack([c * pts[:, 0] - s * pts[:, 2], pts[:, 1],
                        s * pts[:, 0] + c * pts[:, 2]], axis=1).astype(np.float32)
    assert np.allclose(doc.eval(pts), doc.eval(rotated), atol=2e-4)


def test_repeat_inspection_and_validation():
    prim = clay.Sphere(r=0.2).repeat_grid(spacing=1.5, counts=(1, 1, 0))
    assert prim.repeat["type"] == "grid_finite"
    assert clay.Sphere(r=0.2).repeat_grid(spacing=2.0).repeat["type"] == "grid_infinite"
    assert clay.Sphere(r=0.2).repeat is None

    with pytest.raises(ValueError, match="spacing"):
        clay.Sphere(r=0.2).repeat_grid(spacing=0.0)
    with pytest.raises(ValueError, match="count"):
        clay.Sphere(r=0.2).repeat_radial(count=1)


def test_repetition_round_trip_and_composition(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(0.3, 0.8, 0.3)).twist(0.9).repeat_radial(count=5, offset=1.2))
    rng = np.random.default_rng(45)
    pts = rng.uniform(-2.5, 2.5, size=(256, 3)).astype(np.float32)
    before = doc.eval(pts)
    path = tmp_path / "array.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(pts))


# --- primitive backfill (add-primitive-backfill) ----------------------------


def _all_primitive_instances():
    """One instance of every primitive class the module exposes."""
    return {
        "Sphere": clay.Sphere(r=0.6),
        "Box": clay.Box(size=(0.8, 0.8, 0.8)),
        "RoundBox": clay.RoundBox(size=(0.8, 0.8, 0.8), r=0.1),
        "Torus": clay.Torus(R=0.7, r=0.2),
        "Capsule": clay.Capsule(a=(-0.3, 0, 0), b=(0.3, 0, 0), r=0.25),
        "Cylinder": clay.Cylinder(r=0.5, h=0.6),
        "Cone": clay.Cone(h=0.6, r1=0.5, r2=0.1),
        "RoundCone": clay.RoundCone(r1=0.4, r2=0.15, h=0.7),
        "Ellipsoid": clay.Ellipsoid(r=(0.7, 0.4, 0.5)),
        "Octahedron": clay.Octahedron(s=0.7),
        "HexPrism": clay.HexPrism(hx=0.5, hy=0.4),
        "Pyramid": clay.Pyramid(h=0.9),
        "CappedTorus": clay.CappedTorus(aperture=1.0, ra=0.7, rb=0.2),
        "Link": clay.Link(length=0.3, r1=0.5, r2=0.15),
        "ExactCone": clay.ExactCone(half_angle=0.5, h=0.9),
        "CutSphere": clay.CutSphere(r=0.8, h=0.2),
        "CutHollowSphere": clay.CutHollowSphere(r=0.8, h=0.2, t=0.07),
        "SolidAngle": clay.SolidAngle(angle=0.7, ra=0.8),
        "Tetrahedron": clay.Tetrahedron(r=0.7),
        "Dodecahedron": clay.Dodecahedron(r=0.6),
        "Icosahedron": clay.Icosahedron(r=0.6),
        "TriPrism": clay.TriPrism(hx=0.6, hy=0.4),
        "OctahedronCheap": clay.OctahedronCheap(s=0.7),
        "LNormSphere": clay.LNormSphere(r=0.7, n=4.0),
    }


@pytest.mark.parametrize("name", sorted(_all_primitive_instances()))
def test_every_primitive_evaluates_meshes_and_round_trips(name, tmp_path):
    prim = _all_primitive_instances()[name]
    doc = clay.Document()
    doc.add_sdf_layer("l").add(prim)

    pts = np.array([[0.0, 0.0, 0.0], [9.0, 9.0, 9.0]], dtype=np.float32)
    d = doc.eval(pts)
    assert np.all(np.isfinite(d))
    assert d[1] > 0  # far outside every bounded shape

    mesh = doc.mesh(resolution=40)
    assert mesh.triangle_count > 0

    path = tmp_path / f"{name}.clayspace"
    doc.save(str(path))
    assert np.array_equal(d, clay.load(str(path)).eval(pts))


def test_unbounded_primitives_are_usable_and_marked():
    # a plane carves a half-space out of a sphere
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0))
    layer.add(clay.Plane(normal=(0, 1, 0), offset=0.0), op=clay.Op.SUBTRACT)
    # subtracting a half-space removes the side where the plane field is
    # negative (y < 0 here), leaving the upper half of the sphere
    below = doc.eval(np.array([[0.0, -0.5, 0.0]], dtype=np.float32))[0]
    above = doc.eval(np.array([[0.0, 0.5, 0.0]], dtype=np.float32))[0]
    assert above < 0 < below

    infinite = clay.Document()
    infinite.add_sdf_layer("l").add(clay.CylinderInfinite(r=0.5))
    far = infinite.eval(np.array([[0.0, 50.0, 0.0]], dtype=np.float32))[0]
    assert far < 0  # still solid far along the axis


def test_bound_primitives_lower_the_step_scale():
    for prim in (clay.TriPrism(hx=0.6, hy=0.4), clay.OctahedronCheap(s=0.7),
                 clay.LNormSphere(r=0.7, n=4.0), clay.Ellipsoid(r=(0.8, 0.4, 0.6))):
        doc = clay.Document()
        doc.add_sdf_layer("l").add(prim)
        assert doc.safe_step_scale() <= 1.0
    exact = clay.Document()
    exact.add_sdf_layer("l").add(clay.Tetrahedron(r=0.7))
    assert exact.safe_step_scale() == pytest.approx(1.0)


# --- brush shapes and the paint brush (add-brush-shapes) --------------------


def _brush(size, shape, index=None):
    g = clay.VoxelGrid(voxel_size=0.1)
    i = index if index is not None else g.palette_add("#ffffff")
    g.set_brush((0, 0, 0), size, i, shape=shape)
    return g


def test_sphere_brush_is_a_subset_of_the_cube():
    for size in (3, 4, 5, 7, 9):
        cube = _brush(size, "cube")
        sphere = _brush(size, "sphere")
        assert 0 < sphere.occupied_count <= cube.occupied_count

        lo, hi = -((size - 1) // 2), size // 2
        mid = lo + hi
        for z in range(lo, hi + 1):
            for y in range(lo, hi + 1):
                for x in range(lo, hi + 1):
                    if sphere.get((x, y, z)) != 0:
                        assert cube.get((x, y, z)) != 0
                        dx, dy, dz = 2 * x - mid, 2 * y - mid, 2 * z - mid
                        assert dx * dx + dy * dy + dz * dz <= size * size
        if size >= 3:
            assert sphere.get((hi, hi, hi)) == 0  # cube corner outside the ball


def test_brush_shape_defaults_to_cube_and_rejects_unknown():
    g = clay.VoxelGrid(voxel_size=0.1)
    i = g.palette_add("#ffffff")
    g.set_brush((0, 0, 0), 5, i)
    assert g.occupied_count == 125

    with pytest.raises(ValueError, match="cube.*sphere"):
        g.set_brush((0, 0, 0), 3, i, shape="blob")


def test_brush_size_n_covers_n_cells_per_axis():
    # Regression: even sizes used to collapse onto the odd size below, because
    # the footprint was radius (n-1)/2 over -r..r.
    for size in range(1, 9):
        assert _brush(size, "cube").occupied_count == size ** 3

    for shape in ("cube", "sphere"):
        counts = [_brush(size, shape).occupied_count for size in range(1, 9)]
        assert counts == sorted(counts)
        assert all(b > a for a, b in zip(counts, counts[1:])), counts


def test_sphere_brush_is_non_degenerate_at_size_two():
    # radius n/2 = 1 covers every cell centre of the 2x2x2 footprint
    assert _brush(2, "sphere").occupied_count == 8


def test_erase_brush_honours_shape():
    g = clay.VoxelGrid(voxel_size=0.1)
    i = g.palette_add("#ffffff")
    g.set_brush((0, 0, 0), 5, i)
    g.erase_brush((0, 0, 0), 5, shape="sphere")
    assert g.get((0, 0, 0)) == 0     # scooped
    assert g.get((2, 2, 2)) == i     # corner survives
    assert g.occupied_count == 125 - _brush(5, "sphere").occupied_count


def test_paint_brush_recolors_without_creating_voxels():
    g = clay.VoxelGrid(voxel_size=0.1)
    a = g.palette_add("#ff0000")
    b = g.palette_add("#00ff00")
    g.set((0, 0, 0), a)
    g.set((1, 0, 0), a)
    before = g.occupied_count

    g.paint_brush((0, 0, 0), 5, b)
    assert g.occupied_count == before          # no new cells
    assert g.get((0, 0, 0)) == b
    assert g.get((1, 0, 0)) == b
    assert g.get((2, 0, 0)) == 0               # still empty


def test_paint_brush_sphere_leaves_cube_corners():
    g = clay.VoxelGrid(voxel_size=0.1)
    a = g.palette_add("#ff0000")
    b = g.palette_add("#00ff00")
    g.set_brush((0, 0, 0), 5, a)
    g.paint_brush((0, 0, 0), 5, b, shape="sphere")
    assert g.get((0, 0, 0)) == b
    assert g.get((2, 2, 2)) == a
    assert g.occupied_count == 125


def test_paint_mirrored_recolors_both_sides():
    g = clay.VoxelGrid(voxel_size=0.1)
    a = g.palette_add("#ff0000")
    b = g.palette_add("#00ff00")
    g.set_mirrored((3, 1, 2), a, axes="x")
    assert g.occupied_count == 2

    g.paint_mirrored((3, 1, 2), b, axes="x")
    assert g.occupied_count == 2
    assert g.get((3, 1, 2)) == b
    assert g.get((-4, 1, 2)) == b     # mirror of x=3 is -1-3


# --- falloff brushes and sculpting verbs (add-sculpt-brushes) ---------------


def _soft(size, falloff, strength=1.0, seed=0, shape="sphere"):
    g = clay.VoxelGrid(voxel_size=0.1)
    i = g.palette_add("#ffffff")
    g.set_brush((0, 0, 0), size, i, shape=shape, falloff=falloff,
                strength=strength, seed=seed)
    return g


def _slab():
    g = clay.VoxelGrid(voxel_size=0.1)
    c = g.palette_add("#8899aa")
    g.fill_box((-6, -2, -6), (6, 0, 6), c)
    return g, c


def test_constant_falloff_matches_the_hard_brush():
    for size in (5, 8, 9):
        hard = clay.VoxelGrid(voxel_size=0.1)
        i = hard.palette_add("#ffffff")
        hard.set_brush((0, 0, 0), size, i, shape="sphere")
        assert _soft(size, "constant").occupied_count == hard.occupied_count


def test_softer_falloff_covers_less():
    counts = [_soft(15, f).occupied_count
              for f in ("constant", "linear", "smooth", "gaussian")]
    assert counts == sorted(counts, reverse=True), counts
    assert counts[-1] > 0


def test_strength_scales_coverage():
    full = _soft(15, "smooth", strength=1.0).occupied_count
    half = _soft(15, "smooth", strength=0.5).occupied_count
    assert 0 < half < full
    assert _soft(15, "smooth", strength=0.0).occupied_count == 0


def test_falloff_dither_is_deterministic():
    a = _soft(11, "linear", seed=7)
    b = _soft(11, "linear", seed=7)
    assert a.occupied_count == b.occupied_count
    for z in range(-6, 7):
        for y in range(-6, 7):
            for x in range(-6, 7):
                assert a.get((x, y, z)) == b.get((x, y, z))


def test_unknown_falloff_is_rejected():
    g = clay.VoxelGrid(voxel_size=0.1)
    i = g.palette_add("#ffffff")
    with pytest.raises(ValueError, match="constant.*linear.*smooth.*gaussian"):
        g.set_brush((0, 0, 0), 5, i, falloff="wobble")


def test_sculpt_smooth_dissolves_a_spur():
    g, c = _slab()
    g.set((0, 1, 0), c)
    g.set((0, 2, 0), c)
    before = g.occupied_count
    g.sculpt_smooth((0, 1, 0), 9)
    assert g.get((0, 1, 0)) == 0
    assert g.get((0, 2, 0)) == 0
    assert g.get((0, -1, 0)) == c        # slab interior survives
    assert g.occupied_count < before


def test_sculpt_inflate_grows_and_erodes():
    g, _ = _slab()
    base = g.occupied_count
    g.sculpt_inflate((0, 0, 0), 9, amount=1)
    grown = g.occupied_count
    assert grown > base
    g.sculpt_inflate((0, 0, 0), 9, amount=-1)
    assert g.occupied_count < grown


def test_sculpt_flatten_clears_above_the_plane():
    g, c = _slab()
    g.set_brush((0, 2, 0), 3, c)          # a bump proud of the slab
    g.sculpt_flatten((0, 0, 0), 11, normal=(0, 1, 0))
    above = sum(
        1
        for y in range(1, 4)
        for x in range(-6, 7)
        for z in range(-6, 7)
        if g.get((x, y, z)) != 0
    )
    assert above == 0


def test_sculpt_pinch_leaves_the_outside_alone():
    g, _ = _slab()
    reference, _ = _slab()
    g.sculpt_pinch((0, 0, 0), 7)
    assert g.occupied_count != reference.occupied_count
    for y in range(-4, 5):
        for x in range(-9, 10):
            for z in range(-9, 10):
                if x * x + y * y + z * z > 49:
                    assert g.get((x, y, z)) == reference.get((x, y, z))


# --- editing an existing document (add-edit-commands) -----------------------


def _sphere_doc(r=0.5):
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    node = layer.add(clay.Sphere(r=r))
    return doc, layer, node


def _at(doc, point):
    return float(doc.eval(np.array([point], dtype=np.float32))[0])


def test_moving_a_placed_item_keeps_its_id():
    doc, layer, node = _sphere_doc()
    assert _at(doc, (0, 0, 0)) < 0 < _at(doc, (2, 0, 0))

    layer.set_transform(node, position=(2, 0, 0))
    assert _at(doc, (2, 0, 0)) < 0 < _at(doc, (0, 0, 0))
    assert layer.bounds() is not None
    # the id survives the edit, which is what lets a UI hold a selection
    layer.set_color(node, "#ff0000")
    assert np.allclose(doc.colors(np.array([[2, 0, 0]], np.float32))[0], [1, 0, 0])


def test_set_transform_keeps_unspecified_components():
    doc, layer, node = _sphere_doc()
    layer.set_transform(node, position=(1, 0, 0), scale=2.0)
    layer.set_transform(node, position=(0, 1, 0))       # scale must persist
    assert _at(doc, (0, 1, 0)) == pytest.approx(-1.0, abs=1e-4)  # r=0.5 scaled by 2


def test_set_prim_keeps_the_modifiers():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Sphere(r=0.3).repeat_radial(count=6, offset=1.0))
    before = doc.eval(np.array([[0.0, 0.0, 0.0]], np.float32))[0]

    layer.set_prim(node, clay.Box(size=(0.4, 0.4, 0.4)))
    after = doc.eval(np.array([[0.0, 0.0, 0.0]], np.float32))[0]
    assert after != before                      # the primitive really changed
    # the radial array is a property of the node, not the builder, so it stays:
    # a lone box at the origin would read -0.2 here, the array does not
    assert after == pytest.approx(-0.2, abs=1e-4)


def test_set_op_blend_changes_how_a_node_combines():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0))
    node = layer.add(clay.Sphere(r=0.6, position=(0.8, 0, 0)))
    merged = _at(doc, (1.2, 0, 0))
    assert merged < 0

    layer.set_op_blend(node, op=clay.Op.SUBTRACT)
    assert _at(doc, (1.2, 0, 0)) > 0            # that lobe is carved away now


def test_removing_a_node_leaves_the_others_alone():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    keep = layer.add(clay.Sphere(r=0.5))
    drop = layer.add(clay.Sphere(r=0.5, position=(3, 0, 0)))
    assert _at(doc, (3, 0, 0)) < 0

    layer.remove(drop)
    assert _at(doc, (3, 0, 0)) > 0
    assert _at(doc, (0, 0, 0)) < 0              # the survivor is untouched
    layer.set_color(keep, "#00ff00")            # and its id still resolves


def test_layer_visibility_round_trips_exactly():
    doc = clay.Document()
    a = doc.add_sdf_layer("a")
    a.add(clay.Sphere(r=1.0))
    b = doc.add_sdf_layer("b")
    b.add(clay.Box(size=(1, 1, 1), position=(3, 0, 0)))

    probes = np.random.default_rng(3).uniform(-4, 4, size=(512, 3)).astype(np.float32)
    before = doc.eval(probes)

    doc.set_layer_visible(a.id, False)
    hidden = doc.eval(probes)
    assert not np.array_equal(before, hidden)

    doc.set_layer_visible(a.id, True)
    assert np.array_equal(before, doc.eval(probes))   # exactly, not approximately


def test_layer_transform_and_removal():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.5))
    doc.set_layer_transform(layer.id, position=(0, 4, 0))
    assert _at(doc, (0, 4, 0)) < 0

    doc.remove_layer(layer.id)
    assert _at(doc, (0, 4, 0)) > 0


def test_layer_reorder():
    doc = clay.Document()
    a = doc.add_sdf_layer("a")
    b = doc.add_sdf_layer("b")
    doc.move_layer(b.id, 0)
    # both layers still evaluate; reorder changes stacking, not membership
    a.add(clay.Sphere(r=0.5))
    b.add(clay.Sphere(r=0.5, position=(2, 0, 0)))
    assert _at(doc, (0, 0, 0)) < 0 and _at(doc, (2, 0, 0)) < 0


def test_editing_a_stroke_in_place():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Stroke(points=[(-1, 0, 0, 0.3), (0, 0, 0, 0.3)]))
    assert _at(doc, (1, 0, 0)) > 0

    layer.append_stroke(node, [(1.0, 0.0, 0.0, 0.3)])
    assert _at(doc, (1, 0, 0)) < 0              # the new point is solid

    layer.trim_stroke(node, 1)
    assert _at(doc, (1, 0, 0)) > 0              # and trimming takes it back

    # the trimmed stroke matches one authored with only the surviving points
    reference = clay.Document()
    reference.add_sdf_layer("l").add(
        clay.Stroke(points=[(-1, 0, 0, 0.3), (0, 0, 0, 0.3)]))
    probes = np.random.default_rng(5).uniform(-2, 2, size=(256, 3)).astype(np.float32)
    assert np.allclose(doc.eval(probes), reference.eval(probes), atol=1e-5)


@pytest.mark.parametrize("call", [
    lambda doc, layer: layer.set_transform(9999, position=(0, 0, 0)),
    lambda doc, layer: layer.set_color(9999, "#ffffff"),
    lambda doc, layer: layer.set_op_blend(9999, op=clay.Op.ADD),
    lambda doc, layer: layer.remove(9999),
    lambda doc, layer: layer.move(9999),
    lambda doc, layer: layer.trim_stroke(9999, 1),
    lambda doc, layer: doc.remove_layer(9999),
    lambda doc, layer: doc.set_layer_visible(9999, False),
    lambda doc, layer: doc.set_layer_transform(9999, position=(0, 0, 0)),
])
def test_unknown_id_is_refused_and_changes_nothing(call):
    doc, layer, node = _sphere_doc()
    probes = np.random.default_rng(11).uniform(-2, 2, size=(128, 3)).astype(np.float32)
    before = doc.eval(probes)

    with pytest.raises(ValueError):
        call(doc, layer)

    assert np.array_equal(before, doc.eval(probes))


# --- undo and redo (add-undo-stack) -----------------------------------------


def _undo_doc():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    node = layer.add(clay.Sphere(r=0.5))
    doc.enable_undo()
    return doc, layer, node


def _snapshot(doc, tmp_path, name):
    """Serialized bytes — the strict form of 'the document is unchanged'."""
    path = tmp_path / name
    doc.save(str(path))
    return path.read_bytes()


def test_undo_is_off_by_default_and_costs_nothing():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Sphere(r=0.5))
    assert doc.undo_enabled is False
    assert doc.undo_depth == 0 and doc.redo_depth == 0

    layer.set_color(node, "#ff0000")     # edits still work, just unrecorded
    assert doc.undo_depth == 0
    with pytest.raises(RuntimeError, match="not enabled"):
        doc.undo()


def test_undo_restores_the_previous_state_bit_identically(tmp_path):
    doc, layer, node = _undo_doc()
    before = _snapshot(doc, tmp_path, "before.clayspace")

    layer.set_transform(node, position=(2, 0, 0))
    assert doc.undo_depth == 1
    assert _snapshot(doc, tmp_path, "edited.clayspace") != before

    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "undone.clayspace") == before
    assert doc.undo_depth == 0 and doc.redo_depth == 1


def test_redo_reapplies(tmp_path):
    doc, layer, node = _undo_doc()
    layer.set_transform(node, position=(2, 0, 0))
    edited = _snapshot(doc, tmp_path, "edited.clayspace")

    doc.undo()
    assert doc.redo() is True
    assert _snapshot(doc, tmp_path, "redone.clayspace") == edited
    assert doc.redo_depth == 0


def test_undo_on_an_empty_stack_reports_rather_than_raises():
    doc, layer, node = _undo_doc()
    assert doc.undo() is False
    assert doc.redo() is False


def test_a_new_edit_clears_the_redo_stack():
    doc, layer, node = _undo_doc()
    layer.set_transform(node, position=(2, 0, 0))
    doc.undo()
    assert doc.redo_depth == 1

    layer.set_color(node, "#00ff00")
    assert doc.redo_depth == 0


def test_a_stroke_undoes_as_one_step(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Stroke(points=[(-1, 0, 0, 0.3)]))
    doc.enable_undo()
    before = _snapshot(doc, tmp_path, "before.clayspace")

    for i in range(6):
        layer.append_stroke(node, [(-1 + 0.3 * (i + 1), 0.0, 0.0, 0.3)])
    assert doc.undo_depth == 1, "consecutive stroke appends must coalesce"

    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "after.clayspace") == before


def test_grouped_edits_undo_together(tmp_path):
    doc, layer, node = _undo_doc()
    before = _snapshot(doc, tmp_path, "before.clayspace")

    doc.begin_undo_group()
    layer.set_transform(node, position=(2, 0, 0))
    layer.set_color(node, "#ff0000")
    layer.set_op_blend(node, blend=clay.Smooth(0.3))
    doc.end_undo_group()
    assert doc.undo_depth == 1

    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "after.clayspace") == before


def test_every_edit_kind_is_undoable(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Sphere(r=0.5))
    stroke = layer.add(clay.Stroke(points=[(-1, 0, 0, 0.3), (0, 0, 0, 0.3)]))
    other = doc.add_sdf_layer("other")
    doc.enable_undo()

    edits = [
        lambda: layer.set_transform(node, position=(1, 0, 0)),
        lambda: layer.set_prim(node, clay.Box(size=(1, 1, 1))),
        lambda: layer.set_color(node, "#123456"),
        lambda: layer.set_op_blend(node, op=clay.Op.SUBTRACT),
        lambda: layer.move(node, index=0),
        lambda: layer.append_stroke(stroke, [(1.0, 0.0, 0.0, 0.3)]),
        lambda: layer.trim_stroke(stroke, 1),
        lambda: layer.remove(node),
        lambda: doc.set_layer_visible(other.id, False),
        lambda: doc.set_layer_transform(other.id, position=(0, 3, 0)),
        lambda: doc.remove_layer(other.id),
        # regression (fix/adds-escape-undo): adds bypassed the undo stack
        lambda: layer.add(clay.Sphere(r=0.2)),
        lambda: doc.add_sdf_layer("added"),
    ]
    for i, edit in enumerate(edits):
        before = _snapshot(doc, tmp_path, f"before_{i}.clayspace")
        depth = doc.undo_depth
        edit()
        assert doc.undo_depth == depth + 1, f"edit {i} was not recorded"
        assert doc.undo() is True
        assert _snapshot(doc, tmp_path, f"after_{i}.clayspace") == before, f"edit {i}"
        doc.redo()


def test_adding_records_undo_and_redo_preserves_ids(tmp_path):
    # Regression (fix/adds-escape-undo): layer and node adds inserted
    # directly instead of routing through the command vocabulary, so they
    # escaped an enabled undo stack.
    doc = clay.Document()
    doc.enable_undo()
    empty = _snapshot(doc, tmp_path, "empty.clayspace")

    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Sphere(r=0.5))
    assert doc.undo_depth == 2

    assert doc.undo() is True
    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "unwound.clayspace") == empty

    assert doc.redo() is True
    assert doc.redo() is True
    layer.set_color(node, "#ff0000")  # the id survived the redo


def test_wrap_around_bends_the_interval_around_the_axis():
    import math
    x0, x1 = -math.pi, math.pi          # per = 2pi -> radius 1
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(2 * math.pi, 0.4, 1.0)).wrap_around(x0, x1))

    at = lambda p: float(doc.eval(np.array([p], dtype=np.float32))[0])
    assert at((1, 0, 0)) < 0          # on the cylinder, inside the slab
    assert at((0, 1, 0)) < 0
    assert at((0, 0, 0)) == pytest.approx(0.8, abs=1e-3)   # axis to inner face
    assert doc.safe_step_scale() == pytest.approx(1 / 1.2, abs=1e-3)

    lo, hi = layer.bounds()
    assert hi[0] == pytest.approx(1.2, abs=1e-3)           # r + half thickness


def test_wrap_around_round_trips(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Box(size=(6.0, 0.4, 1.0)).wrap_around(-3.0, 3.0))
    probes = np.random.default_rng(21).uniform(-3, 3, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)

    path = tmp_path / "wrapped.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


def test_elongate_stretches_a_sphere_into_a_capsule():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.5).elongate((1.0, 0.0, 0.0)))

    at = lambda p: float(doc.eval(np.array([p], dtype=np.float32))[0])
    assert at((0, 0, 0)) == pytest.approx(-0.5, abs=1e-4)      # flat section
    assert at((0.9, 0, 0)) == pytest.approx(-0.5, abs=1e-4)    # still flat
    assert at((1.5, 0, 0)) == pytest.approx(0.0, abs=1e-3)     # undistorted cap
    assert at((0, 0.5, 0)) == pytest.approx(0.0, abs=1e-3)

    lo, hi = layer.bounds()
    assert hi[0] == pytest.approx(1.5, abs=1e-3)
    assert hi[1] == pytest.approx(0.5, abs=1e-3)
    # non-expansive, so tracing is not slowed
    assert doc.safe_step_scale() == pytest.approx(1.0)


def test_elongate_refuses_negative_extents():
    with pytest.raises(ValueError, match=">= 0"):
        clay.Sphere(r=1.0).elongate((-1.0, 0.0, 0.0))


def test_elongate_round_trips(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Box(size=(0.6, 0.6, 0.6)).elongate((0.7, 0.2, 0.0)))
    probes = np.random.default_rng(33).uniform(-3, 3, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "elongated.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


def test_bend_linear_ramps_across_its_span():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(0.6, 2.0, 0.6)).bend_linear(a=(0, -1, 0), b=(0, 1, 0),
                                                         v=(1.0, 0, 0)))
    at = lambda p: float(doc.eval(np.array([p], dtype=np.float32))[0])
    # bottom of the ramp: undisplaced, so the face is at x = 0.3
    assert at((0.3, -1, 0)) == pytest.approx(0.0, abs=1e-3)
    # top: displaced by the whole vector
    assert at((1.3, 1, 0)) == pytest.approx(0.0, abs=1e-3)
    # slope = |v| / span
    assert doc.safe_step_scale() == pytest.approx(1 / 1.5, abs=1e-3)


def test_bend_radial_lifts_the_rim():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Cylinder(r=1.2, h=0.15).bend_radial(r0=0.2, r1=1.2, dz=0.6))
    at = lambda p: float(doc.eval(np.array([p], dtype=np.float32))[0])
    assert at((0, 0, 0)) < 0          # centre unmoved
    assert at((1.1, 0.6, 0)) < 0      # rim lifted by dz


@pytest.mark.parametrize("call", [
    lambda: clay.Sphere(r=1.0).bend_linear((0, 0, 0), (0, 0, 0), (1, 0, 0)),
    lambda: clay.Sphere(r=1.0).bend_radial(1.0, 1.0, 0.5),
])
def test_degenerate_ramp_span_is_refused(call):
    with pytest.raises(ValueError, match="!="):
        call()


def test_bend_deformers_round_trip(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Box(size=(0.5, 1.5, 0.5))
        .bend_linear(a=(0, -0.7, 0), b=(0, 0.7, 0), v=(0.4, 0.0, 0.2), ease=4)
        .bend_radial(r0=0.1, r1=0.9, dz=0.3, ease=2))
    probes = np.random.default_rng(77).uniform(-2, 2, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "bends.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


def test_elongate_axis_stretches_an_asymmetric_primitive():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Cone(h=0.6, r1=0.5, r2=0.1).elongate_axis((0.8, 0.0, 0.0)))

    at = lambda p: float(doc.eval(np.array([p], dtype=np.float32))[0])
    centre = at((0, 0, 0))
    assert centre < 0
    assert at((0.8, 0, 0)) == pytest.approx(centre, abs=1e-4)   # flat plateau
    assert doc.safe_step_scale() == pytest.approx(1.0)          # non-expansive

    lo, hi = layer.bounds()
    assert hi[0] == pytest.approx(1.3, abs=1e-3)                # 0.5 + 0.8


def test_elongate_axis_refuses_negative_extents():
    with pytest.raises(ValueError, match=">= 0"):
        clay.Sphere(r=1.0).elongate_axis((0.0, -1.0, 0.0))


def test_elongate_axis_round_trips(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Pyramid(h=0.9).elongate_axis((0.4, 0.0, 0.6)))
    probes = np.random.default_rng(88).uniform(-3, 3, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "ea.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


# --- region deformers: grab and pose (add-region-deformers) -----------------


def test_grab_moves_only_its_region():
    grabbed = clay.Document()
    grabbed.add_sdf_layer("l").add(
        clay.Sphere(r=1.0).grab(center=(1.0, 0, 0), radius=0.8, displacement=(0.5, 0, 0)))
    plain = clay.Document()
    plain.add_sdf_layer("l").add(clay.Sphere(r=1.0))

    rng = np.random.default_rng(4)
    probes = rng.uniform(-3, 3, size=(4096, 3)).astype(np.float32)
    a, b = grabbed.eval(probes), plain.eval(probes)

    outside = np.linalg.norm(probes - np.array([1.0, 0, 0], np.float32), axis=1) > 0.8
    assert np.array_equal(a[outside], b[outside])   # finite support, exactly
    assert not np.array_equal(a[~outside], b[~outside])


def test_grab_pulls_the_surface_toward_the_displacement():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Sphere(r=1.0).grab(center=(1.0, 0, 0), radius=0.8, displacement=(0.5, 0, 0)))
    at = lambda p: float(doc.eval(np.array([p], dtype=np.float32))[0])
    assert at((1.0, 0, 0)) < 0        # the old tip is now interior
    assert doc.safe_step_scale() < 1.0


def test_grab_front_only_leaves_the_far_side():
    def build(front):
        d = clay.Document()
        d.add_sdf_layer("l").add(clay.Sphere(r=1.0).grab(
            center=(0, 0, 0), radius=2.0, displacement=(0.6, 0, 0), front_only=front))
        return d
    plain = clay.Document(); plain.add_sdf_layer("l").add(clay.Sphere(r=1.0))
    behind = np.array([[-1.0, 0, 0]], np.float32)
    assert build(True).eval(behind)[0] == pytest.approx(float(plain.eval(behind)[0]), abs=1e-3)
    assert build(False).eval(behind)[0] != pytest.approx(float(plain.eval(behind)[0]), abs=1e-3)


def test_pose_rotates_a_region():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Cylinder(r=0.3, h=1.0).pose(
        center=(0, 0.8, 0), radius=1.0, axis=(0, 0, 1), angle=0.7))
    assert doc.safe_step_scale() < 1.0
    # far below the region the shape is where it always was
    at = lambda p: float(doc.eval(np.array([p], dtype=np.float32))[0])
    assert at((0.0, -0.9, 0.0)) < 0


@pytest.mark.parametrize("call", [
    lambda: clay.Sphere(r=1.0).grab(center=(0, 0, 0), radius=0.0, displacement=(1, 0, 0)),
    lambda: clay.Sphere(r=1.0).pose(center=(0, 0, 0), radius=-1.0, axis=(0, 1, 0), angle=1.0),
])
def test_region_deformers_refuse_a_non_positive_radius(call):
    with pytest.raises(ValueError, match="> 0"):
        call()


def test_voxel_grab_moves_material():
    g = clay.VoxelGrid(voxel_size=0.1)
    i = g.palette_add("#3399ee")
    g.set_brush((0, 0, 0), 11, i, shape="sphere")
    assert g.get((6, 0, 0)) == 0
    g.sculpt_grab((0, 0, 0), 15, displacement=(0.29, 0.0, 0.0), shape="sphere",
                  falloff="constant")
    assert g.get((6, 0, 0)) == i        # leading face advanced
    assert g.get((-5, 0, 0)) == 0       # trailing face vacated


def test_region_deformers_round_trip(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Sphere(r=1.0)
        .grab(center=(0.8, 0, 0), radius=0.9, displacement=(0.3, 0.2, 0), ease=4)
        .pose(center=(0, 0.5, 0), radius=1.2, axis=(0, 0, 1), angle=0.4, ease=2))
    probes = np.random.default_rng(19).uniform(-3, 3, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "region.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


def test_pose_line_bends_along_its_segment():
    import math

    def build(angle):
        d = clay.Document()
        d.add_sdf_layer("l").add(
            clay.Capsule(a=(0, -1, 0), b=(0, 1, 0), r=0.25)
            .pose_line(a=(0, -1, 0), b=(0, 1, 0), axis=(0, 0, 1), angle=angle))
        return d

    straight, bent = build(0.0), build(1.0)
    # sample a slab and compare where the material sits
    xs = np.linspace(-3, 2, 60)
    ys = np.linspace(-2, 2, 60)
    pts = np.array([[x, y, 0.0] for y in ys for x in xs], dtype=np.float32)

    def mean_solid_x(doc):
        d = doc.eval(pts)
        solid = pts[d < 0]
        return float(solid[:, 0].mean())

    assert mean_solid_x(straight) == pytest.approx(0.0, abs=0.05)
    assert mean_solid_x(bent) < -0.05          # swung with the rotation
    assert bent.safe_step_scale() < 1.0


def test_pose_line_anchor_is_fixed():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Capsule(a=(0, -1, 0), b=(0, 1, 0), r=0.25)
        .pose_line(a=(0, -1, 0), b=(0, 1, 0), axis=(0, 0, 1), angle=0.8))
    plain = clay.Document()
    plain.add_sdf_layer("l").add(clay.Capsule(a=(0, -1, 0), b=(0, 1, 0), r=0.25))
    anchor = np.array([[0.0, -1.0, 0.0]], np.float32)
    assert float(doc.eval(anchor)[0]) == pytest.approx(float(plain.eval(anchor)[0]), abs=1e-4)


def test_pose_line_refuses_a_degenerate_segment():
    with pytest.raises(ValueError, match="!="):
        clay.Sphere(r=1.0).pose_line(a=(0, 0, 0), b=(0, 0, 0), axis=(0, 1, 0), angle=1.0)


def test_pose_line_round_trips(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Capsule(a=(0, -1, 0), b=(0, 1, 0), r=0.3)
        .pose_line(a=(0, -0.8, 0), b=(0, 0.9, 0), axis=(0.2, 0, 1), angle=0.6, ease=3))
    probes = np.random.default_rng(23).uniform(-3, 3, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "poseline.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


# -- mask field (add-mask-field) ---------------------------------------------


def test_mask_paints_and_samples():
    m = clay.MaskField(cell_size=0.1)
    assert m.empty
    m.paint((0.05, 0.05, 0.05), size=9, falloff="smooth")
    assert not m.empty
    assert m.sample((0.05, 0.05, 0.05)) == pytest.approx(1.0)
    assert m.sample((5.0, 5.0, 5.0)) == pytest.approx(0.0)

    pts = np.array([[0.05, 0.05, 0.05], [5.0, 5.0, 5.0]], dtype=np.float32)
    values = m.sample_many(pts)
    assert values.shape == (2,)
    assert values[0] == pytest.approx(1.0) and values[1] == pytest.approx(0.0)


def test_mask_falloff_is_graded():
    m = clay.MaskField(0.1)
    m.paint_cell((0, 0, 0), size=9, falloff="smooth")
    mid = m.get((3, 0, 0))
    assert 0.0 < mid < 1.0


def test_mask_freezes_a_region():
    grid = clay.VoxelGrid(0.1)
    idx = grid.palette_add("#ffffff")
    grid.fill_box((-8, -1, -1), (8, 1, 1), idx)
    before = grid.occupied_count

    m = clay.MaskField(0.1)
    for x in range(-8, 0):
        for y in (-1, 0, 1):
            for z in (-1, 0, 1):
                m.set((x, y, z), 1.0)

    grid.erase_brush((0, 0, 0), size=20, mask=m)
    assert grid.occupied_count < before
    assert all(grid.get((x, 0, 0)) != 0 for x in range(-8, 0))   # frozen
    assert all(grid.get((x, 0, 0)) == 0 for x in range(0, 9))    # erased


def test_mask_none_is_the_unmasked_brush():
    a, b = clay.VoxelGrid(0.1), clay.VoxelGrid(0.1)
    for g in (a, b):
        g.fill_box((-4, -4, -4), (4, 4, 4), g.palette_add("#ffffff"))
    a.erase_brush((0, 0, 0), size=5, falloff="smooth", seed=3)
    b.erase_brush((0, 0, 0), size=5, falloff="smooth", seed=3, mask=clay.MaskField(0.1))
    assert a.occupied_count == b.occupied_count


def test_mask_region_operations():
    m = clay.MaskField(0.1)
    m.paint_cell((0, 0, 0), size=5, falloff="linear")
    before = m.get((1, 0, 0))
    m.invert()
    assert m.get((1, 0, 0)) == pytest.approx(1.0 - before, abs=0.01)
    m.invert()
    assert m.get((1, 0, 0)) == pytest.approx(before, abs=0.01)

    (_lo, hi) = m.bounds()
    m.expand(1)
    assert m.bounds()[1][0] == hi[0] + 1
    m.contract(1)
    assert m.bounds()[1][0] == hi[0]

    m.clear()
    assert m.empty and m.bounds() is None


# The invariant this feature exists for: 3DCoat's masks die on voxelization,
# ours are addressed in world units so they cannot.
def test_mask_survives_a_resolution_change(tmp_path):
    doc = clay.Document()
    doc.add_voxel_layer("clay", voxel_size=0.1)
    m = doc.add_mask("clay", cell_size=0.1)
    m.paint((0.35, 0.35, 0.35), size=7, falloff="smooth")

    probes = np.array([[0.35, 0.35, 0.35], [0.15, 0.35, 0.35], [2.0, 2.0, 2.0]], np.float32)
    before = m.sample_many(probes)
    assert before[0] == pytest.approx(1.0)

    # Grids at three resolutions all consult the same mask through world space,
    # and none of them perturbs it.
    for voxel_size in (0.05, 0.1, 0.4):
        g = clay.VoxelGrid(voxel_size)
        g.fill_box((0, 0, 0), (20, 20, 20), g.palette_add("#ffffff"))
        g.erase_brush((3, 3, 3), size=9, mask=m)
        assert np.allclose(m.sample_many(probes), before)

    path = tmp_path / "masked.clayspace"
    doc.save(str(path))
    reloaded = clay.load(str(path))
    assert np.allclose(reloaded.mask("clay").sample_many(probes), before)


def test_mask_lookup_and_removal():
    doc = clay.Document()
    doc.add_sdf_layer("body")
    assert doc.mask("body") is None
    m = doc.add_mask("body")
    m.paint_cell((0, 0, 0), size=3)
    assert doc.mask("body").painted_count == m.painted_count
    assert doc.remove_mask("body") is True
    assert doc.mask("body") is None
    assert doc.remove_mask("body") is False


def test_mask_rejects_a_bad_cell_size():
    with pytest.raises(ValueError):
        clay.MaskField(0.0)


# -- brush stroke engine (add-brush-stroke-engine) ---------------------------


def _line(length=2.0, step=0.1, pressure=1.0):
    n = int(length / step) + 1
    return np.array([[i * step, 0.0, 0.0, pressure] for i in range(n)], np.float32)


def test_stroke_spacing_follows_the_path_not_the_samples():
    preset = clay.StrokePreset(radius=0.25, spacing=0.5)
    sparse = preset.resolve(_line(step=0.5))
    dense = preset.resolve(_line(step=0.05))
    assert sparse["positions"].shape == dense["positions"].shape
    assert np.allclose(sparse["positions"], dense["positions"], atol=1e-5)


def test_stroke_accepts_three_four_or_five_columns():
    preset = clay.StrokePreset(radius=0.25, spacing=0.5)
    xyz = np.array([[0, 0, 0], [1, 0, 0]], np.float32)
    assert preset.resolve(xyz)["positions"].shape[0] > 1
    with pytest.raises(ValueError, match=r"\(N, 3\)"):
        preset.resolve(np.zeros((2, 6), np.float32))


def test_stroke_pressure_drives_size():
    preset = clay.StrokePreset(radius=0.25, spacing=0.5, pressure_size=1.0)
    ramp = np.array([[i * 0.1, 0, 0, i / 20.0] for i in range(21)], np.float32)
    radii = preset.resolve(ramp)["radii"]
    assert np.all(np.diff(radii) >= -1e-6)
    assert radii[0] < radii[-1]


def test_stroke_jitter_is_reproducible():
    a = clay.StrokePreset(radius=0.2, spacing=0.5, jitter_position=0.5, seed=7)
    b = clay.StrokePreset(radius=0.2, spacing=0.5, jitter_position=0.5, seed=7)
    c = clay.StrokePreset(radius=0.2, spacing=0.5, jitter_position=0.5, seed=8)
    path = _line()
    assert np.array_equal(a.resolve(path)["positions"], b.resolve(path)["positions"])
    assert not np.array_equal(a.resolve(path)["positions"], c.resolve(path)["positions"])


# A preset library outlives the engine version that wrote it, which is the
# whole reason the schema version is there from the first release.
def test_stroke_preset_round_trips_and_refuses_a_newer_schema():
    preset = clay.StrokePreset(radius=0.4, spacing=0.3, taper_start=0.2,
                               accumulation=clay.Accumulation.CLAMPED, seed=99)
    data = preset.serialize()
    back = clay.StrokePreset.deserialize(data)
    assert back.radius == pytest.approx(preset.radius)
    assert back.spacing == pytest.approx(preset.spacing)
    assert back.taper_start == pytest.approx(preset.taper_start)
    assert back.accumulation == clay.Accumulation.CLAMPED
    assert back.seed == 99
    assert back.serialize() == data

    newer = bytes([clay.StrokePreset.version + 1]) + data[1:]
    with pytest.raises(ValueError, match="newer"):
        clay.StrokePreset.deserialize(newer)


def test_stroke_applied_to_a_voxel_grid():
    grid = clay.VoxelGrid(0.05)
    preset = clay.StrokePreset(radius=0.15, spacing=0.5)
    stamps = grid.apply_stroke(_line(length=1.5), preset, grid.palette_add("#ffffff"))
    assert stamps > 3
    assert grid.occupied_count > 0
    (lo, hi) = grid.bounds()
    assert hi[0] - lo[0] > 25


# The interface promise: a stroked edit is an ordinary edit, so undo applies
# to it without knowing the stroke engine exists.
def test_stroke_applied_to_a_layer_is_one_undo_step():
    doc = clay.Document()
    doc.enable_undo()
    layer = doc.add_sdf_layer("body")
    before = doc.undo_depth

    preset = clay.StrokePreset(radius=0.2, spacing=0.5)
    ids = layer.apply_stroke(_line(), preset, clay.Sphere(r=1.0))
    assert len(ids) > 3
    assert len(set(ids)) == len(ids)          # every stamp is its own node
    assert doc.undo_depth == before + 1       # ...and the stroke is one step

    probe = np.array([[1.0, 0.0, 0.0]], np.float32)
    assert doc.eval(probe)[0] < 0
    doc.undo()
    assert doc.eval(probe)[0] > 0
    doc.redo()
    assert doc.eval(probe)[0] < 0


def test_stroke_is_gated_by_a_mask():
    mask = clay.MaskField(0.05)
    for x in range(0, 60):
        for y in range(-15, 16):
            for z in range(-15, 16):
                mask.set((x, y, z), 1.0)

    preset = clay.StrokePreset(radius=0.15, spacing=0.5)
    path = np.array([[-1.5 + i * 0.05, 0, 0, 1.0] for i in range(61)], np.float32)
    total = len(preset.resolve(path)["radii"])

    grid = clay.VoxelGrid(0.05)
    gated = grid.apply_stroke(path, preset, grid.palette_add("#ffffff"), mask=mask)
    assert 0 < gated < total

    # The same freeze for a declarative SDF edit: the frozen half gets no items.
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    doc_mask = doc.add_mask("body", cell_size=0.05)
    for x in range(0, 60):
        for y in range(-15, 16):
            for z in range(-15, 16):
                doc_mask.set((x, y, z), 1.0)
    ids = layer.apply_stroke(path, preset, clay.Sphere(r=1.0), mask=doc_mask)
    assert 0 < len(ids) < total


def test_stroke_preset_rejects_a_bad_radius():
    with pytest.raises(ValueError):
        clay.StrokePreset(radius=0.0)
    with pytest.raises(ValueError):
        clay.StrokePreset(spacing=0.0)


# -- ghosted and locked layers (add-layer-ghost-lock) ------------------------


def _two_layers():
    doc = clay.Document()
    front = doc.add_sdf_layer("front")
    front.add(clay.Sphere(r=0.5, position=(0, 0, -2)))
    back = doc.add_sdf_layer("back")
    back.add(clay.Sphere(r=0.5, position=(0, 0, 2)))
    return doc, front, back


def test_protection_defaults_to_off_and_round_trips(tmp_path):
    doc, front, back = _two_layers()
    assert doc.layer_protection(front.id) == (False, False)

    doc.set_layer_protection(front.id, ghost=True)
    doc.set_layer_protection(back.id, locked=True)
    assert doc.layer_protection(front.id) == (True, False)
    assert doc.layer_protection(back.id) == (False, True)

    path = tmp_path / "protected.clayspace"
    doc.save(str(path))
    reloaded = clay.load(str(path))
    assert reloaded.layer_protection(front.id) == (True, False)
    assert reloaded.layer_protection(back.id) == (False, True)


def test_protection_does_not_change_the_field():
    doc, front, _back = _two_layers()
    probes = np.random.default_rng(3).uniform(-3, 3, size=(512, 3)).astype(np.float32)
    before = doc.eval(probes)
    doc.set_layer_protection(front.id, ghost=True, locked=True)
    assert np.array_equal(doc.eval(probes), before)


def test_a_ghost_does_not_steal_the_pick():
    doc, front, back = _two_layers()
    hit = doc.raycast((0, 0, -6), (0, 0, 1))
    assert hit["layer"] == front.id

    doc.set_layer_protection(front.id, ghost=True)
    assert doc.raycast((0, 0, -6), (0, 0, 1))["layer"] == back.id

    # Locking protects against edits, not against selection.
    doc.set_layer_protection(front.id, ghost=False, locked=True)
    assert doc.raycast((0, 0, -6), (0, 0, 1))["layer"] == front.id


def test_editing_a_protected_layer_raises():
    doc, front, _back = _two_layers()
    doc.set_layer_protection(front.id, locked=True)
    with pytest.raises(ValueError, match="locked"):
        front.add(clay.Sphere(r=0.2))
    with pytest.raises(ValueError, match="locked"):
        doc.set_layer_visible(front.id, False)

    # ...and it is reversible, or locking would be permanent.
    doc.set_layer_protection(front.id, locked=False)
    front.add(clay.Sphere(r=0.2))


def test_protection_is_undoable():
    doc, front, _back = _two_layers()
    doc.enable_undo()
    doc.set_layer_protection(front.id, ghost=True)
    assert doc.layer_protection(front.id) == (True, False)
    doc.undo()
    assert doc.layer_protection(front.id) == (False, False)


# -- control-point curves (add-curve-objects) --------------------------------


_SQUARE = np.array([[-1, 0, 0, 0.05], [0, 1, 0, 0.05], [1, 0, 0, 0.05], [0, -1, 0, 0.05]],
                   np.float32)
# The Catmull-Rom midpoint of the first span, against a chord midpoint of
# (-0.5, 0.5): a bulge of ~0.088, well outside a tube of radius 0.05.
_BULGE = np.array([[-0.5625, 0.5625, 0.0]], np.float32)


def _curve_doc(**kwargs):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Stroke(points=_SQUARE, **kwargs))
    return doc, layer, node


def test_a_hard_chain_is_the_stroke_it_always_was():
    plain, _, _ = _curve_doc()
    typed, _, _ = _curve_doc(types="hard")
    probes = np.random.default_rng(11).uniform(-2, 2, size=(512, 3)).astype(np.float32)
    assert np.array_equal(plain.eval(probes), typed.eval(probes))


def test_smooth_points_bulge_outside_the_chain():
    hard, _, _ = _curve_doc()
    smooth, _, _ = _curve_doc(types="spline")
    assert hard.eval(_BULGE)[0] > 0
    assert smooth.eval(_BULGE)[0] < 0
    # Catmull-Rom interpolates, so it still passes through every control point.
    assert all(smooth.eval(_SQUARE[i:i + 1, :3])[0] < 0 for i in range(4))


def test_bezier_handles_are_local_space():
    pts = np.array([[-1, 0, 0, 0.1], [1, 0, 0, 0.1]], np.float32)
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Stroke(
        points=pts, types="bezier",
        out_handles=np.array([[0, 2, 0], [0, 0, 0]], np.float32),
        in_handles=np.array([[0, 0, 0], [0, 2, 0]], np.float32)))
    # Both handles at +2y put the cubic's peak at y = 1.5.
    assert doc.eval(np.array([[0, 1.5, 0]], np.float32))[0] < 0
    assert doc.eval(np.array([[0, 0, 0]], np.float32))[0] > 0


def test_a_closed_curve_joins_its_ends():
    open_doc, _, _ = _curve_doc(types="spline")
    closed_doc, _, _ = _curve_doc(types="spline", closed=True)
    # The closing span's Catmull-Rom midpoint. It is not the mirror of the
    # open curve's: closing changes every span's neighbours, so the whole
    # curve differs, not only the span that was added.
    closing = np.array([[-0.625, -0.625, 0.0]], np.float32)
    assert open_doc.eval(closing)[0] > 0
    assert closed_doc.eval(closing)[0] < 0


def test_tolerance_is_a_document_property_and_deterministic():
    coarse, _, _ = _curve_doc(types="spline", tolerance=0.2)
    fine, _, _ = _curve_doc(types="spline", tolerance=0.002)
    probes = np.random.default_rng(5).uniform(-2, 2, size=(256, 3)).astype(np.float32)
    assert not np.array_equal(coarse.eval(probes), fine.eval(probes))

    again, _, _ = _curve_doc(types="spline", tolerance=0.002)
    assert np.array_equal(fine.eval(probes), again.eval(probes))

    with pytest.raises(ValueError):
        clay.Stroke(points=_SQUARE, tolerance=0.0)


def test_per_point_types_and_readback():
    s = clay.Stroke(points=_SQUARE, types=["hard", "spline", "bezier", "bspline"],
                    closed=True, tolerance=0.005)
    assert s.types == ["hard", "spline", "bezier", "bspline"]
    assert s.closed is True
    assert s.tolerance == pytest.approx(0.005)

    with pytest.raises(ValueError, match="hard"):
        clay.Stroke(points=_SQUARE, types="wobble")
    with pytest.raises(ValueError, match="one entry per point"):
        clay.Stroke(points=_SQUARE, types=["hard", "spline"])


def test_add_point_takes_a_type():
    s = clay.Stroke().add_point((0, 0, 0), 0.1, type="spline") \
                     .add_point((1, 0, 0), 0.1, type="bezier", out_handle=(0, 1, 0))
    assert s.types == ["spline", "bezier"]
    assert s.point_count == 2


def test_editing_a_curve_is_an_ordinary_edit():
    doc, layer, node = _curve_doc(types="spline")
    doc.enable_undo()
    assert doc.eval(_BULGE)[0] < 0

    layer.set_points(node, _SQUARE, types="hard")
    assert doc.eval(_BULGE)[0] > 0
    doc.undo()
    assert doc.eval(_BULGE)[0] < 0

    doc.set_layer_protection(layer.id, locked=True)
    with pytest.raises(ValueError, match="locked"):
        layer.set_points(node, _SQUARE, types="hard")


def test_curves_round_trip(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Stroke(
        points=_SQUARE, types=["bezier", "spline", "bspline", "hard"], closed=True,
        tolerance=0.004, out_handles=np.array([[0.3, 0.7, -0.2], [0, 0, 0], [0, 0, 0], [0, 0, 0]],
                                              np.float32)))
    probes = np.random.default_rng(19).uniform(-2, 2, size=(512, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "curve.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


# -- the cut tool (add-cut-tool) ---------------------------------------------


def _block(size=2.0):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(size, size, size)))
    return doc, layer


def _front(**kwargs):
    frame = dict(origin=(0, 0, -4), right=(1, 0, 0), up=(0, 1, 0), forward=(0, 0, 1))
    frame.update(kwargs)
    return frame


def test_a_cut_makes_a_hole_through():
    doc, layer = _block()
    layer.add(clay.Cut(shape=clay.CutShape.rect(0.4, 0.4), region=doc, **_front()),
              op=clay.Op.SUBTRACT)
    inside = np.array([[0, 0, -0.9], [0, 0, 0.0], [0, 0, 0.9]], np.float32)
    outside = np.array([[0.8, 0.8, 0], [-0.8, 0, 0], [0.45, 0, 0]], np.float32)
    assert (doc.eval(inside) > 0).all()     # through both faces
    assert (doc.eval(outside) < 0).all()    # and nowhere else


# The decision the design rests on: a cut is a prism. If it converged, moving
# the frame along its own sweep would change the solid.
def test_the_cut_is_a_prism_not_a_frustum():
    probes = np.random.default_rng(7).uniform(-1.2, 1.2, size=(512, 3)).astype(np.float32)
    solids = []
    for z in (-4.0, -40.0):
        doc, layer = _block()
        layer.add(clay.Cut(shape=clay.CutShape.rect(0.4, 0.4), region=doc,
                           **_front(origin=(0, 0, z))), op=clay.Op.SUBTRACT)
        solids.append(doc.eval(probes) < 0)
    assert np.array_equal(solids[0], solids[1])


def test_keep_inner_and_keep_outer_are_the_op():
    probes = np.random.default_rng(3).uniform(-0.9, 0.9, size=(256, 3)).astype(np.float32)
    results = []
    for op in (clay.Op.SUBTRACT, clay.Op.INTERSECT):
        doc, layer = _block()
        layer.add(clay.Cut(shape=clay.CutShape.circle(0.5), region=doc, **_front()), op=op)
        results.append(doc.eval(probes) < 0)
    # Inside the block every point survives exactly one of the two.
    assert not (results[0] & results[1]).any()


def test_an_explicit_extent_cuts_only_that_far():
    doc, layer = _block()
    layer.add(clay.Cut(shape=clay.CutShape.rect(0.3, 0.3), region=doc,
                       near=0.0, far=8.0, **_front(origin=(0, 0, -8))),
              op=clay.Op.SUBTRACT)
    assert doc.eval(np.array([[0, 0, -0.5]], np.float32))[0] > 0   # cut here
    assert doc.eval(np.array([[0, 0, 0.5]], np.float32))[0] < 0    # solid beyond


def test_rounding_bevels_the_cut():
    probe = np.array([[0.45, 0, 0]], np.float32)
    fields = []
    for rounding in (0.0, 0.15):
        doc, layer = _block()
        layer.add(clay.Cut(shape=clay.CutShape.rect(0.4, 0.4), region=doc, rounding=rounding,
                           **_front()), op=clay.Op.SUBTRACT)
        fields.append(float(doc.eval(probe)[0]))
    assert fields[1] > fields[0]   # a fatter cutter takes more at the wall


def test_a_spline_lasso_follows_its_curve():
    control = np.array([[-0.5, 0, 0, 0], [0, 0.5, 0, 0], [0.5, 0, 0, 0], [0, -0.5, 0, 0]],
                       np.float32)
    spline = clay.CutShape.curve(control, types="spline", tolerance=0.005)
    assert spline.vertex_count > 4

    straight = clay.CutShape.polygon(control[:, :2].copy())
    # Between the control polygon's chord through (-0.25, 0.25) and the closed
    # spline's bulge out to (-0.3125, 0.3125).
    probe = np.array([[-0.28, 0.28, 0.0]], np.float32)
    fields = []
    for shape in (straight, spline):
        doc, layer = _block()
        layer.add(clay.Cut(shape=shape, region=doc, **_front()), op=clay.Op.SUBTRACT)
        fields.append(float(doc.eval(probe)[0]))
    assert fields[0] < 0 < fields[1]


def test_a_cut_is_an_ordinary_edit():
    doc, layer = _block()
    doc.enable_undo()
    probe = np.array([[0, 0, 0]], np.float32)
    assert doc.eval(probe)[0] < 0

    layer.add(clay.Cut(shape=clay.CutShape.circle(0.4), region=doc, **_front()),
              op=clay.Op.SUBTRACT)
    assert doc.eval(probe)[0] > 0
    doc.undo()
    assert doc.eval(probe)[0] < 0

    doc.set_layer_protection(layer.id, locked=True)
    with pytest.raises(ValueError, match="locked"):
        layer.add(clay.Cut(shape=clay.CutShape.circle(0.4), region=doc, **_front()),
                  op=clay.Op.SUBTRACT)


def test_a_cut_takes_the_region_as_a_pair_too():
    doc, layer = _block()
    layer.add(clay.Cut(shape=clay.CutShape.rect(0.3, 0.3),
                       region=((-1, -1, -1), (1, 1, 1)), **_front()), op=clay.Op.SUBTRACT)
    assert doc.eval(np.array([[0, 0, 0.9]], np.float32))[0] > 0


def test_degenerate_cuts_are_refused():
    doc, _ = _block()
    with pytest.raises(ValueError, match="orthonormal"):
        clay.Cut(shape=clay.CutShape.circle(0.3), region=doc, **_front(up=(0.5, 0.5, 0)))
    with pytest.raises(ValueError, match="orthonormal"):
        clay.Cut(shape=clay.CutShape.circle(0.3), region=doc, **_front(right=(2, 0, 0)))
    with pytest.raises(ValueError, match="degenerate"):
        clay.Cut(shape=clay.CutShape.circle(0.0), region=doc, **_front())
    # A two-vertex outline is caught by the polygon converter before the cut
    # ever sees it, with a more specific message than "degenerate".
    with pytest.raises(ValueError, match="3 vertices"):
        clay.CutShape.polygon(np.zeros((2, 2), np.float32))
    # ...and an outline with no area is caught by the cut itself.
    with pytest.raises(ValueError, match="degenerate"):
        clay.Cut(shape=clay.CutShape.polygon(np.zeros((3, 2), np.float32)), region=doc,
                 **_front())


# -- the remaining voxel verbs and repair ------------------------------------


def _repair_slab(thickness=4, half=8):
    g = clay.VoxelGrid(0.1)
    g.fill_box((-half, 0, -half), (half, thickness - 1, half), g.palette_add("#9aa4b0"))
    return g


def _hollow_box(half=5):
    g = clay.VoxelGrid(0.1)
    g.fill_box((-half, -half, -half), (half, half, half), g.palette_add("#c8703a"))
    g.fill_box((-half + 1, -half + 1, -half + 1), (half - 1, half - 1, half - 1), 0)
    return g


def test_fill_cavities_fills_pockets_not_dents():
    pocket = _repair_slab()
    pocket.fill_box((0, 2, 0), (0, 3, 0), 0)      # one across, two deep
    pocket.sculpt_fill_cavities((0, 2, 0), 9, passes=2, shape="cube")
    assert pocket.get((0, 2, 0)) != 0

    # Two across and one deep is three occupied face neighbours — below the
    # threshold, and deliberately so: smoothing is the verb for that.
    dent = _repair_slab()
    dent.fill_box((-1, 3, -1), (0, 3, 0), 0)
    before = dent.occupied_count
    dent.sculpt_fill_cavities((0, 3, 0), 9, passes=2, shape="cube")
    assert dent.occupied_count == before


def test_scrape_flattens_and_smooths():
    g = _repair_slab()
    accent = g.palette_add("#d08a52")
    for x in range(-5, 6, 2):
        g.set((x, 4, 0), accent)
    g.set((0, 5, 0), accent)
    before = g.occupied_count
    g.sculpt_scrape((0, 4, 0), 13, normal=(0, 1, 0), shape="cube")
    assert g.occupied_count < before
    assert g.get((0, 5, 0)) == 0


def test_smudge_moves_the_skin_and_grab_moves_the_lump():
    def block():
        g = clay.VoxelGrid(0.1)
        g.fill_box((-6, -6, -6), (6, 6, 6), g.palette_add("#9aa4b0"))
        return g

    smudged, grabbed, plain = block(), block(), block()
    smudged.sculpt_smudge((6, 0, 0), 9, displacement=(0.3, 0, 0), shape="cube")
    grabbed.sculpt_grab((6, 0, 0), 9, displacement=(0.3, 0, 0), shape="cube")
    assert smudged.occupied_count != plain.occupied_count
    # The interior is untouched — that is the distinction from grab.
    assert all(smudged.get((x, 0, 0)) != 0 for x in range(-6, 4))


def test_carve_with_an_alpha():
    def block():
        g = clay.VoxelGrid(0.1)
        g.fill_box((-8, -8, -8), (8, 8, 8), g.palette_add("#9aa4b0"))
        return g

    alpha = np.zeros((8, 8), np.float32)
    alpha[:, 4:] = 1.0                       # opaque on one half only
    g = block()
    g.sculpt_carve_alpha((0, 0, 0), 11, alpha=alpha, direction=(0, 0, 1), shape="cube")
    assert g.occupied_count < block().occupied_count

    with pytest.raises(ValueError, match="malformed"):
        g.sculpt_carve_alpha((0, 0, 0), 11, alpha=np.zeros((0, 0), np.float32),
                             direction=(0, 0, 1))
    with pytest.raises(ValueError, match="malformed"):
        g.sculpt_carve_alpha((0, 0, 0), 11, alpha=alpha, direction=(0, 0, 0))


def test_repair_reports_before_it_repairs():
    g = _hollow_box()
    before = g.occupied_count
    report = g.repair_report()
    assert report["enclosed_voids"] == 1
    assert report["void_cells"] == 9 ** 3
    assert report["airtight"] is False
    assert g.occupied_count == before          # non-destructive

    solid = clay.VoxelGrid(0.1)
    solid.fill_box((-3, -3, -3), (3, 3, 3), solid.palette_add("#ffffff"))
    assert solid.repair_report()["airtight"] is True


def test_close_holes_then_fill_voids():
    g = _hollow_box()
    g.set((5, 0, 0), 0)                        # pierce the wall
    assert g.repair_report()["enclosed_voids"] == 0   # the outside reaches in

    occupied_before = g.occupied_count
    g.repair_close_holes(passes=1)
    assert g.repair_report()["enclosed_voids"] == 1   # sealed in now
    assert g.occupied_count >= occupied_before        # only ever adds

    shell = g.get((5, 5, 5))
    g.repair_fill_voids()
    assert g.repair_report()["airtight"] is True
    assert g.get((0, 0, 0)) == shell                  # coloured from the shell


def test_repair_leaves_an_open_cavity_alone():
    g = _hollow_box()
    g.fill_box((5, -3, -3), (5, 3, 3), 0)      # a wide mouth
    before = g.occupied_count
    g.repair_fill_voids()
    assert g.occupied_count == before


def test_repair_honours_a_mask():
    mask = clay.MaskField(0.1)
    for x in range(-6, 7):
        for y in range(-6, 7):
            for z in range(-6, 7):
                mask.set((x, y, z), 1.0)
    g = _hollow_box()
    before = g.occupied_count
    g.repair_fill_voids(mask=mask)
    assert g.occupied_count == before
    assert g.repair_report()["airtight"] is False


# -- loft (add-loft-opcode) --------------------------------------------------


def _loft_doc(profiles, half_depth=1.0, ease=0):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Loft(profiles, half_depth=half_depth, ease=ease))
    return doc


def test_a_loft_reaches_both_profiles():
    doc = _loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.25)])
    ends = np.array([[0.9, 0, -0.99], [1.1, 0, -0.99],
                     [0.2, 0, 0.99], [0.4, 0, 0.99]], np.float32)
    assert (doc.eval(ends) < 0).tolist() == [True, False, True, False]


def test_a_loft_interpolates_in_between():
    doc = _loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.25)])
    middle = np.array([[0.55, 0, 0.0], [0.7, 0, 0.0]], np.float32)
    assert (doc.eval(middle) < 0).tolist() == [True, False]


def test_more_than_two_profiles_are_bracketed():
    waisted = _loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.2),
                         clay.Profile.circle(r=1.0)])
    straight = _loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=1.0)])
    probe = np.array([[0.5, 0, 0.0]], np.float32)
    assert waisted.eval(probe)[0] > 0      # pinched at the middle profile
    assert straight.eval(probe)[0] < 0


def test_a_loft_takes_polygon_profiles():
    doc = _loft_doc([clay.Profile.circle(r=0.9),
                     clay.Profile.polygon([(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)])])
    probes = np.array([[0.85, 0, -0.99], [0.45, 0.45, 0.99], [0.7, 0.7, 0.99]], np.float32)
    assert (doc.eval(probes) < 0).tolist() == [True, True, False]


# The requirement the raymarcher depends on.
def test_a_loft_is_a_bound_with_a_real_lipschitz():
    doc = _loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.25)])
    assert doc.safe_step_scale() < 1.0

    # The same profiles over a tenth of the depth change ten times as fast
    # along Z, so the safe step must fall further.
    shallow = _loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.25)],
                        half_depth=0.1)
    assert shallow.safe_step_scale() < doc.safe_step_scale()

    # An exact primitive alone still steps at full rate, so the drop above is
    # the loft's doing and not a property of every document.
    plain = clay.Document()
    plain.add_sdf_layer("l").add(clay.Sphere(r=1.0))
    assert plain.safe_step_scale() == pytest.approx(1.0)


def test_a_loft_round_trips(tmp_path):
    doc = _loft_doc([clay.Profile.circle(r=0.9),
                     clay.Profile.polygon([(-0.5, -0.5), (0.5, -0.5), (0.0, 0.6)]),
                     clay.Profile.box(half_x=0.3, half_y=0.6)], half_depth=1.3, ease=3)
    probes = np.random.default_rng(31).uniform(-2, 2, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "loft.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


def test_a_degenerate_loft_is_refused():
    with pytest.raises(ValueError, match="two or more"):
        clay.Loft([clay.Profile.circle(r=1.0)])
    with pytest.raises(ValueError, match="two or more"):
        clay.Loft([])
    with pytest.raises(ValueError, match="half_depth"):
        clay.Loft([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.5)], half_depth=0.0)


# -- swept along a guide (add-swept-n) ---------------------------------------


_STRAIGHT = np.array([[-1, 0, 0, 0], [1, 0, 0, 0]], np.float32)


def _swept_doc(guide, profiles, **kwargs):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Swept(guide, profiles, **kwargs))
    return doc


def test_a_sweep_along_a_straight_guide_is_a_cylinder():
    doc = _swept_doc(_STRAIGHT, [clay.Profile.circle(r=0.3), clay.Profile.circle(r=0.3)])
    # Material on the axis, empty off it, and the ends are FLAT: past the end
    # the distance is the overshoot, not a rounded capsule's.
    probes = np.array([[0, 0, 0], [0, 0.5, 0], [-1.4, 0, 0]], np.float32)
    d = doc.eval(probes)
    assert d[0] < 0 and d[1] > 0
    assert d[2] == pytest.approx(0.4, abs=0.02)


def test_a_sweep_follows_a_bent_guide():
    guide = np.array([[-1, 0, 0, 0], [0, 0, 0, 0], [0, 1, 0, 0]], np.float32)
    doc = _swept_doc(guide, [clay.Profile.circle(r=0.25), clay.Profile.circle(r=0.25)])
    on = np.array([[-0.6, 0, 0], [0, 0.6, 0]], np.float32)
    off = np.array([[-0.8, 0.8, 0], [0.8, 0.8, 0]], np.float32)
    assert (doc.eval(on) < 0).all()
    assert (doc.eval(off) > 0).all()


def test_sweep_profiles_interpolate_along_the_guide():
    doc = _swept_doc(_STRAIGHT, [clay.Profile.circle(r=0.4), clay.Profile.circle(r=0.1)])
    probes = np.array([[-0.9, 0.3, 0], [0.9, 0.3, 0], [0.9, 0.07, 0]], np.float32)
    assert (doc.eval(probes) < 0).tolist() == [True, False, True]


def test_a_sweep_is_a_bound_whose_step_tracks_curvature():
    gentle = _swept_doc(np.array([[-2, 0, 0, 0], [0, 0.3, 0, 0], [2, 0, 0, 0]], np.float32),
                        [clay.Profile.circle(r=0.2), clay.Profile.circle(r=0.2)])
    sharp = _swept_doc(np.array([[-2, 0, 0, 0], [0, 1.6, 0, 0], [2, 0, 0, 0]], np.float32),
                       [clay.Profile.circle(r=0.2), clay.Profile.circle(r=0.2)])
    assert sharp.safe_step_scale() < gentle.safe_step_scale() < 1.0

    # A profile wider than the guide's tightest bend folds the sweep through
    # itself. It must still compile and evaluate — a guide is editable after
    # the fact — and report a tiny step rather than claiming to be a distance.
    folded = _swept_doc(np.array([[-1, 0, 0, 0], [0, 1.0, 0, 0], [1, 0, 0, 0]], np.float32),
                        [clay.Profile.circle(r=2.0), clay.Profile.circle(r=2.0)])
    assert folded.safe_step_scale() < 0.01
    assert np.isfinite(folded.eval(np.array([[0, 0, 0]], np.float32))[0])


def test_a_sweeps_guide_honours_point_types():
    guide = np.array([[-1, 0, 0, 0], [0, 0.8, 0, 0], [1, 0, 0, 0]], np.float32)
    hard = _swept_doc(guide, [clay.Profile.circle(r=0.15), clay.Profile.circle(r=0.15)],
                      types="hard")
    spline = _swept_doc(guide, [clay.Profile.circle(r=0.15), clay.Profile.circle(r=0.15)],
                        types="spline")
    probes = np.random.default_rng(8).uniform(-1.2, 1.2, size=(512, 3)).astype(np.float32)
    assert not np.array_equal(hard.eval(probes) < 0, spline.eval(probes) < 0)


def test_a_sweep_round_trips(tmp_path):
    guide = np.array([[-1, 0, 0, 0], [0, 0.7, 0.2, 0], [1, 0, -0.2, 0]], np.float32)
    doc = _swept_doc(guide, [clay.Profile.circle(r=0.3),
                             clay.Profile.polygon([(-0.2, -0.2), (0.2, -0.2), (0.0, 0.3)]),
                             clay.Profile.box(half_x=0.2, half_y=0.1)],
                     types="spline", tolerance=0.01, ease=3)
    probes = np.random.default_rng(12).uniform(-2, 2, size=(1024, 3)).astype(np.float32)
    before = doc.eval(probes)
    path = tmp_path / "swept.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(probes))


def test_a_degenerate_sweep_is_refused():
    with pytest.raises(ValueError, match="two or more profiles"):
        clay.Swept(_STRAIGHT, [clay.Profile.circle(r=0.3)])
    with pytest.raises(ValueError, match="two or more points"):
        clay.Swept(np.array([[0, 0, 0, 0]], np.float32),
                   [clay.Profile.circle(r=0.3), clay.Profile.circle(r=0.3)])
    with pytest.raises(ValueError, match="tolerance"):
        clay.Swept(_STRAIGHT, [clay.Profile.circle(r=0.3), clay.Profile.circle(r=0.3)],
                   tolerance=0.0)


# -- the Move brush (add-move-brush) -----------------------------------------


def _blended_form():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    for x in (-0.45, 0.45):
        layer.add(clay.Sphere(0.5).at((x, 0, 0)), blend=clay.Smooth(0.25))
    return doc, layer


def _top(doc, x):
    ys = np.arange(1.6, -1.6, -0.002, dtype=np.float32)
    pts = np.stack([np.full_like(ys, x), ys, np.zeros_like(ys)], axis=1)
    inside = np.nonzero(doc.eval(pts) <= 0.0)[0]
    return float(ys[inside[0]])


def test_move_surface_drags_a_blended_form_as_one():
    base, _ = _blended_form()
    before = {x: _top(base, x) for x in (-0.45, 0.0, 0.45)}

    doc, layer = _blended_form()
    touched = layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=0.8)
    assert len(touched) == 2                    # both items take a share

    lift = {x: _top(doc, x) - before[x] for x in (-0.45, 0.0, 0.45)}
    assert lift[-0.45] > 0.0 and lift[0.45] > 0.0
    assert lift[-0.45] == pytest.approx(lift[0.45], abs=0.005)   # symmetric
    assert lift[0.0] >= max(lift[-0.45], lift[0.45])             # peaks at the centre


def test_a_grab_on_one_item_is_not_a_move():
    """The reason move_surface exists: a grab is per item and local."""
    base, _ = _blended_form()
    before = {x: _top(base, x) for x in (-0.45, 0.45)}

    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    for x in (-0.45, 0.45):
        ball = clay.Sphere(0.5).at((x, 0, 0))
        if x < 0:
            ball.grab(center=(0, 0, 0), radius=0.8, displacement=(0, 0.4, 0))
        layer.add(ball, blend=clay.Smooth(0.25))

    assert _top(doc, -0.45) - before[-0.45] > 0.02      # its own side moves
    assert _top(doc, 0.45) - before[0.45] == pytest.approx(0.0, abs=0.005)  # the other does not


def test_move_surface_preview_names_the_nodes_and_touches_nothing():
    doc, layer = _blended_form()
    rng = np.random.default_rng(12)
    probes = rng.uniform(-1.3, 1.3, size=(3000, 3)).astype(np.float32)
    before = doc.eval(probes)

    planned = layer.move_surface_preview((0, 0, 0), (0, 0.4, 0), radius=0.8)
    assert len(planned) == 2
    assert np.array_equal(doc.eval(probes), before)      # resolving is pure

    touched = layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=0.8)
    assert sorted(touched) == sorted(planned)            # and it agreed


def test_move_surface_coalesces_over_a_drag():
    # One drag holds its centre and radius and only grows the displacement, so
    # frames replace rather than stack: five frames ending at 0.4 must be the
    # same field as a single drag of 0.4.
    stepped, stepped_layer = _blended_form()
    for d in (0.08, 0.16, 0.24, 0.32, 0.4):
        stepped_layer.move_surface((0, 0, 0), (0, d, 0), radius=1.2)
    once, once_layer = _blended_form()
    once_layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=1.2)

    rng = np.random.default_rng(21)
    probes = rng.uniform(-1.3, 1.3, size=(4000, 3)).astype(np.float32)
    assert np.allclose(stepped.eval(probes), once.eval(probes), atol=1e-6)
    # ...and the marcher does not pay for the frame count either.
    assert stepped.safe_step_scale() == pytest.approx(once.safe_step_scale())


def _reach_along(doc, direction, hi=4.0):
    u = np.array(direction, np.float32)
    u = u / np.linalg.norm(u)
    ts = np.arange(0.0, hi, 0.002, dtype=np.float32)
    pts = (ts[:, None] * u[None, :]).astype(np.float32)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(np.linalg.norm(pts[inside[-1]])) if len(inside) else float("nan")


def test_move_surface_buds_rather_than_stretching():
    """A mesh stretches; a field moves what is already there.

    grab samples the field at p - w*d, so where the weight is one the material is
    rigidly displaced and where it falls to zero nothing happens. A big pull buds
    a lump off the surface rather than drawing a lobe out of it, and pulling
    harder does not help: the reach is bounded by the falloff, not by the drag.
    """
    def pulled(radius, displacement):
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Sphere(r=1.0))
        layer.move_surface((1.0, 0, 0), (displacement, 0, 0), radius=radius)
        return doc

    gentle = _reach_along(pulled(0.5, 1.1), (1, 0, 0)) - 1.0
    hard = _reach_along(pulled(0.5, 2.5), (1, 0, 0)) - 1.0
    assert gentle > 0.0                       # it does move the surface
    assert gentle < 0.5                       # ...by far less than the 1.1 asked for
    assert hard < gentle * 1.5, (gentle, hard)  # and 2x the drag is not 2x the reach


def test_move_surface_drags_compound_the_step_scale():
    """A stroke is many drags, and each one costs the marcher multiplicatively.

    Coalescing covers frames of ONE drag, where the centre and radius are fixed.
    A stroke walks the centre outward, so those stack by design — every grab
    multiplies the declared Lipschitz. A host pulling a long lobe has to
    consolidate (bake the chain into a volume) rather than keep appending.
    """
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0))
    scales = [doc.safe_step_scale()]
    for i in range(9):
        layer.move_surface((1.0 + 0.25 * i, 0, 0), (0.25, 0, 0), radius=0.5)
        scales.append(doc.safe_step_scale())

    assert all(b < a for a, b in zip(scales, scales[1:]))   # strictly decaying
    assert scales[9] < 0.05                                  # nine drags: >20x cost
    # Geometric, not linear: the per-drag ratio is roughly constant.
    ratios = [b / a for a, b in zip(scales[1:], scales[2:])]
    assert max(ratios) - min(ratios) < 0.05, ratios


def test_snakehook_grows_what_move_cannot():
    """The verb for pulling a lobe out is snakehook, and it stays exact."""
    u = np.array([1.0, 0.0, 0.0], np.float32)

    move_doc = clay.Document()
    ml = move_doc.add_sdf_layer("l")
    ml.add(clay.Sphere(r=1.0))
    ml.move_surface((1.0, 0, 0), (1.1, 0, 0), radius=0.8)

    hook_doc = clay.Document()
    hl = hook_doc.add_sdf_layer("l")
    hl.add(clay.Sphere(r=1.0))
    path = np.array([u * t for t in np.linspace(1.05, 2.5, 7)], np.float32)
    hl.add(clay.snakehook((1.0, 0, 0), (-1.0, 0, 0), path, base_radius=0.55,
                          tip_fraction=0.12, taper_curve=0.9), blend=clay.Smooth(0.35))

    assert _reach_along(hook_doc, (1, 0, 0)) > _reach_along(move_doc, (1, 0, 0)) + 0.5
    # ...and adding material keeps the field exact, where displacing it does not:
    # one Move already costs the marcher more than 2x, before a stroke stacks any.
    assert hook_doc.safe_step_scale() == pytest.approx(1.0)
    assert move_doc.safe_step_scale() < 0.5


def test_move_surface_is_one_undo_step():
    doc, layer = _blended_form()
    doc.enable_undo()
    before = _top(doc, 0.0)

    touched = layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=0.8)
    assert len(touched) == 2
    assert _top(doc, 0.0) > before
    assert doc.undo_depth == 1          # one gesture, one step
    assert doc.undo() is True
    assert _top(doc, 0.0) == pytest.approx(before)


def test_move_surface_skips_what_it_cannot_reach():
    doc, layer = _blended_form()
    layer.add(clay.Sphere(0.3).at((50.0, 0, 0)))
    assert len(layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=0.8)) == 2


def test_move_surface_refuses_a_drag_that_is_not_one():
    _doc, layer = _blended_form()
    with pytest.raises(ValueError):
        layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=0.0)
    assert layer.move_surface((0, 0, 0), (0, 0, 0), radius=0.8) == []


def test_move_surface_pull_is_monotonic_and_short():
    base, _ = _blended_form()
    before = _top(base, 0.0)
    previous = 0.0
    for d in (0.1, 0.2, 0.4):
        doc, layer = _blended_form()
        layer.move_surface((0, 0, 0), (0, d, 0), radius=0.8)
        lift = _top(doc, 0.0) - before
        assert lift > previous      # further every time...
        assert lift < d             # ...but never the whole displacement
        previous = lift

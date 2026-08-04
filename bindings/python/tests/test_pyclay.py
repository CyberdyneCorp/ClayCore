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
                                "EMBOSS", "INSET", "SHELL", "REPLACE"])
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


def test_unsupported_deformer_reports_clearly():
    with pytest.raises(ValueError, match="no tape opcode"):
        clay.Sphere(r=1.0).wrap_around(-1.0, 1.0)

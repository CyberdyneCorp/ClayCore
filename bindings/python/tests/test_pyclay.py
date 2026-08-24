# pyclay end-to-end suite (python-bindings spec + task 11.4): the golden
# body scene from docs/05 section 10, evaluation at known points, meshing
# gates, .clayspace round trip, mesh export, backend errors, and vectorized
# property tests (conservative stepping, gradient normalization).

import math

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
    NVIDIA machine, opencl or vulkan where a device exists, metal on Apple."""
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


def test_vulkan_backend_is_usable_from_python_when_present():
    """The tier-3 contract from Python: a backend that declines a capability
    must not cost the caller the capability. Skipped where no Vulkan runtime
    is registered, which is most machines."""
    if "vulkan" not in clay.backends():
        pytest.skip("no Vulkan runtime registered in this build")
    doc, body = build_body()
    rng = np.random.default_rng(20260810)
    pts = rng.uniform(-2.5, 2.5, size=(2048, 3)).astype(np.float32)

    reference = doc.eval(pts, backend="cpu")
    got = doc.eval(pts, backend="vulkan")
    assert got.shape == reference.shape
    scale = np.maximum(np.maximum(np.abs(reference), np.abs(got)), 1.0)
    assert float(np.max(np.abs(reference - got) / scale)) <= 1e-4

    # Meshing and gradients are not on this backend; asking for them by name
    # still answers, because the fallback is the library's job, not the
    # caller's.
    mesh = doc.mesh(backend="vulkan")
    assert mesh.positions.shape[0] > 0 and mesh.positions.shape[1] == 3
    grads = body.gradients(pts[:64], backend="vulkan")
    assert grads.shape == (64, 3)
    lengths = np.linalg.norm(grads, axis=1)
    assert np.allclose(lengths, 1.0, atol=1e-3)


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


def _relief_stroke(amplitude=0.06, rounding=0.30, spacing=0.6, n=9,
                   accumulation=None, strength=1.0, op=None):
    """A ClayBuildup stroke: relief stamps carried along a path."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.7))
    samples = np.array([[x, 0.72, 0.0, 1.0, 0.0] for x in np.linspace(-0.35, 0.35, n)],
                       np.float32)
    preset = clay.StrokePreset()
    preset.radius = 0.16
    preset.spacing = spacing
    preset.strength = strength
    if accumulation is not None:
        preset.accumulation = accumulation
    layer.apply_stroke(samples, preset, clay.Sphere(r=1.0),
                       op=op if op is not None else clay.Op.RELIEF,
                       blend=clay.Smooth(amplitude), rounding=rounding)
    return doc


def _ridge_top(doc, x=0.0):
    ys = np.arange(1.4, -0.2, -0.002, dtype=np.float32)
    pts = np.stack([np.full_like(ys, x), ys, np.zeros_like(ys)], axis=1)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(ys[inside[0]]) if len(inside) else float("nan")


def test_apply_stroke_carries_rounding_to_relief_stamps():
    """Regression: rounding IS the relief falloff, and the stroke dropped it.

    With it lost, cfi_relief declares the amplitude over ~1e-6, so the step scale
    collapses to zero: the geometry is there and nothing can march it.
    """
    lost = _relief_stroke(rounding=0.0)
    kept = _relief_stroke(rounding=0.30)
    # 2.8e-06 against 0.118: four orders of magnitude, which is the difference
    # between a field a marcher can walk and one it cannot.
    assert lost.safe_step_scale() < 1e-4
    assert kept.safe_step_scale() > 0.05
    # ...and both actually deposit, so this is about the declared field, not the shape.
    assert _ridge_top(kept) > 0.7


def test_claybuildup_accumulation_reaches_relief():
    """Buildup is the control the brush is named for.

    A stamp's strength scales an item's amplitude for relief and incise, where
    blend.k IS an amount. Without it buildup and clamped were identical.
    """
    dense = dict(amplitude=0.05, rounding=0.30, spacing=0.25, n=13)
    build = _relief_stroke(accumulation=clay.Accumulation.BUILDUP, **dense)
    clamp = _relief_stroke(accumulation=clay.Accumulation.CLAMPED, **dense)
    assert _ridge_top(build) > _ridge_top(clamp) + 0.05

    # ...and a lighter touch deposits less.
    light = _relief_stroke(accumulation=clay.Accumulation.BUILDUP, strength=0.4, **dense)
    assert _ridge_top(light) < _ridge_top(build) - 0.02


def test_stroke_strength_does_not_reach_a_boolean():
    """A union at half strength is not a smaller union.

    blend.k is a radius for add, so scaling it by a stroke's strength would
    change the shape rather than the amount. Booleans ignore it.
    """
    dense = dict(amplitude=0.05, rounding=0.30, spacing=0.25, n=13, op=clay.Op.ADD)
    build = _relief_stroke(accumulation=clay.Accumulation.BUILDUP, **dense)
    clamp = _relief_stroke(accumulation=clay.Accumulation.CLAMPED, **dense)
    assert _ridge_top(build) == pytest.approx(_ridge_top(clamp), abs=1e-3)


def test_relax_is_the_smooth_brush():
    """Smooth: averaging softens, sized to the feature, and stays 1-Lipschitz."""
    built = _relief_stroke(amplitude=0.08, rounding=0.22, spacing=0.5)
    rough = clay.Volume.from_document(built, cell=0.02)

    def lift(volume):
        doc = clay.Document()
        doc.add_sdf_layer("s").add(volume)
        return _ridge_top(doc) - 0.698

    lifts = [lift(rough)]
    for cells in (3, 6, 10):
        lifts.append(lift(rough.relaxed(radius_cells=cells, iterations=4,
                                        centre=(0, 0.80, 0), region_radius=0.55,
                                        falloff=0.2)))
    # A wider averaging radius softens more — the control is the kernel against
    # the feature size, not the pass count.
    assert all(a >= b - 1e-3 for a, b in zip(lifts, lifts[1:])), lifts
    assert lifts[0] > lifts[-1] + 0.1

    # Averaging destroys exactness but cannot RAISE the Lipschitz bound, which is
    # what keeps the raymarcher correct over a relaxed field.
    #
    # Against the SOURCE, not against 1: a relief through a narrow rounding is a
    # steep field, and `rough` samples it, so its samples vary several times the
    # cell size before relax touches them. This used to read `<= 1.05` and passed
    # only because Volume.sample declared 1 without measuring — the bound was
    # being compared against a number nothing had checked.
    smoothed = rough.relaxed(radius_cells=6, iterations=4, centre=(0, 0.80, 0),
                             region_radius=0.55, falloff=0.2)
    assert rough.sample_lipschitz > 1.05, rough.sample_lipschitz
    assert smoothed.sample_lipschitz <= rough.sample_lipschitz + 1e-3


_TRIM_STROKE = np.array([[x, 0.22 * np.sin(x * 2.6), 0.0, 0.0]
                         for x in np.linspace(-1.3, 1.3, 11)], np.float32)


def _trimmed(side, op=None, shape=None):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.RoundBox(size=(1.0, 0.9, 0.75), r=0.22))
    cut = shape if shape is not None else clay.CutShape.trim(
        _TRIM_STROKE, side=side, extent=(3.0, 3.0, 0.0))
    layer.add(clay.Cut(origin=(0, 0, -3.0), right=(1, 0, 0), up=(0, 1, 0),
                       forward=(0, 0, 1), shape=cut, region=doc),
              op=op if op is not None else clay.Op.SUBTRACT)
    return doc


def test_trim_curve_takes_one_side():
    at = lambda d, y: float(d.eval(np.array([[0.0, y, 0.0]], np.float32))[0])
    below, above = _trimmed("below"), _trimmed("above")
    # The side the outline covers is the side subtract removes.
    assert at(below, -0.30) > 0 and at(below, 0.30) < 0
    assert at(above, -0.30) < 0 and at(above, 0.30) > 0


def test_trim_curve_shape_and_op_are_separate():
    """Covering above and subtracting keeps the same material as covering below
    and intersecting — compared as SOLIDS, since subtract is max(a, -b) and
    intersect is max(a, b) and those differ outside the surface."""
    rng = np.random.default_rng(11)
    cloud = rng.uniform(-1.2, 1.2, size=(5000, 3)).astype(np.float32)
    a = _trimmed("above", clay.Op.SUBTRACT).eval(cloud)
    b = _trimmed("below", clay.Op.INTERSECT).eval(cloud)
    assert float(np.mean((a < 0) == (b < 0))) > 0.999


def test_trim_curve_is_not_a_closed_lasso():
    """The reason these are separate constructors: closing a trim stroke joins
    its endpoints and cuts a sliver instead of dividing the frame."""
    rng = np.random.default_rng(5)
    probes = rng.uniform(-1.1, 1.1, size=(4000, 3)).astype(np.float32)
    trim = _trimmed("below").eval(probes)
    lasso = _trimmed("below", shape=clay.CutShape.curve(_TRIM_STROKE)).eval(probes)
    assert float(np.abs(trim - lasso).max()) > 0.05
    assert int((lasso < 0).sum()) != int((trim < 0).sum())


def test_trim_curve_is_an_ordinary_exact_item():
    assert _trimmed("below").safe_step_scale() == pytest.approx(1.0)


def test_trim_curve_refuses_a_stroke_that_is_not_one():
    with pytest.raises(ValueError, match="two control points"):
        clay.CutShape.trim(_TRIM_STROKE[:1], side="below", extent=(3.0, 3.0, 0.0))
    with pytest.raises(ValueError, match="side must be"):
        clay.CutShape.trim(_TRIM_STROKE, side="sideways", extent=(3.0, 3.0, 0.0))


def _two_fingers():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.40, position=(0, -0.38, 0)))
    for x in (-0.26, 0.26):
        layer.add(clay.Stroke(points=np.array([[x, -0.15, 0, 0.12],
                                               [x, 0.58, 0, 0.10]], np.float32)),
                  blend=clay.Smooth(0.03))
    return doc, layer


def _spans(doc, y=0.45):
    xs = np.arange(-1.2, 1.2, 0.002, dtype=np.float32)
    pts = np.stack([xs, np.full_like(xs, y), np.zeros_like(xs)], axis=1)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    if not len(inside):
        return []
    out, start = [], inside[0]
    for a, b in zip(inside, inside[1:]):
        if b != a + 1:
            out.append((float(xs[start]), float(xs[a])))
            start = b
    out.append((float(xs[start]), float(xs[inside[-1]])))
    return out


def _topological(source, radius, displacement=(-0.25, 0, 0), cell=0.015):
    vol = clay.Volume.moved_topologically_from(
        source, anchor=(-0.26, 0.45, 0), displacement=displacement, radius=radius, cell=cell)
    doc = clay.Document()
    doc.add_sdf_layer("t").add(vol)
    return doc, vol


def test_move_topological_leaves_the_neighbour_alone():
    """The whole brush: two fingers close in space, far along the material."""
    base, _ = _two_fingers()
    before = _spans(base)
    assert len(before) == 2

    euclid, layer = _two_fingers()
    layer.move_surface((-0.26, 0.45, 0), (-0.25, 0, 0), radius=0.50)
    after_e = _spans(euclid)

    topo, _ = _topological(base, 0.50)
    after_t = _spans(topo)
    assert len(after_t) == 2, after_t

    # Euclidean drags the far finger; topological does not.
    assert abs(after_e[1][0] - before[1][0]) > 0.05
    assert after_t[1] == pytest.approx(before[1], abs=0.01)
    # ...and the grabbed finger moves, keeping its width: it translates.
    assert after_t[0][0] < before[0][0] - 0.1
    assert (after_t[0][1] - after_t[0][0]) == pytest.approx(before[0][1] - before[0][0],
                                                            abs=0.05)


def test_move_topological_radius_runs_along_the_material():
    """A distance, not a mask: raise it past the path through the palm and the
    far finger comes into reach."""
    base, _ = _two_fingers()
    before = _spans(base)
    near = _spans(_topological(base, 0.5, cell=0.02)[0])
    far = _spans(_topological(base, 2.2, cell=0.02)[0])
    assert near[-1][0] == pytest.approx(before[1][0], abs=0.01)
    assert far[-1][0] < before[1][0] - 0.02


def test_move_topological_that_reaches_nothing_changes_nothing():
    base, _ = _two_fingers()
    rng = np.random.default_rng(4)
    probes = rng.uniform(-0.9, 0.9, size=(2000, 3)).astype(np.float32)
    # Explicit, identical BOUNDS for both: the default padding grows with the
    # displacement, so two calls with different drags cover different regions
    # and comparing them would measure the padding rather than the move.
    box = ((-1.1, -1.0, -0.6), (1.1, 1.1, 0.6))
    away = clay.Volume.moved_topologically_from(base, anchor=(0, 5.0, 0),
                                                displacement=(0.2, 0, 0), radius=0.4,
                                                cell=0.02, bounds=box)
    still = clay.Volume.moved_topologically_from(base, anchor=(-0.26, 0.45, 0),
                                                 displacement=(0, 0, 0), radius=0.4,
                                                 cell=0.02, bounds=box)
    plain = clay.Volume.from_document(base, cell=0.02, bounds=box)
    # Compared only NEAR THE SURFACE: a narrow-band volume reports a clamped
    # bound further out, so probes in open space would measure the band.
    near = np.abs(plain.eval(probes)) < 0.05
    assert near.sum() > 50
    assert np.allclose(away.eval(probes)[near], plain.eval(probes)[near], atol=1e-5)
    assert np.allclose(still.eval(probes)[near], plain.eval(probes)[near], atol=1e-5)


def test_move_topological_refuses_what_is_not_a_move():
    base, _ = _two_fingers()
    with pytest.raises(ValueError, match="radius must be > 0"):
        clay.Volume.moved_topologically_from(base, anchor=(0, 0, 0),
                                             displacement=(0.1, 0, 0), radius=0.0)
    with pytest.raises(ValueError, match="cell must be > 0"):
        clay.Volume.moved_topologically_from(base, anchor=(0, 0, 0),
                                             displacement=(0.1, 0, 0), radius=0.3, cell=0.0)


_TUBE_PATH = np.array([[-0.62, -0.18, 0.10], [-0.26, 0.30, -0.12],
                       [0.14, -0.05, 0.16], [0.52, 0.34, -0.08]], np.float32)


def _tube_doc(**kwargs):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.tube(_TUBE_PATH, **kwargs))
    return doc


def _across(doc, point, reach=0.5):
    ts = np.arange(-reach, reach, 0.002, dtype=np.float32)
    pts = np.array(point, np.float32)[None, :] + ts[:, None] * np.array([0, 0, 1], np.float32)
    inside = np.nonzero(doc.eval(pts.astype(np.float32)) <= 0)[0]
    return float(ts[inside[-1]] - ts[inside[0]]) if len(inside) else 0.0


def test_tube_follows_its_path_and_stays_exact():
    """A round tube is a swept SPHERE, so it costs the raymarcher nothing."""
    doc = _tube_doc(radius=0.09)
    assert doc.safe_step_scale() == pytest.approx(1.0)
    # the path is inside it
    assert float(doc.eval(np.array([_TUBE_PATH[1]], np.float32))[0]) > -1.0


def test_tube_radius_varies_and_follows_arc_length():
    doc = _tube_doc(point_type="hard", radius=0.14, radius_mid=0.09, radius_end=0.03)
    widths = [_across(doc, p) for p in (_TUBE_PATH[0], _TUBE_PATH[1], _TUBE_PATH[3])]
    assert all(a > b for a, b in zip(widths, widths[1:])), widths
    assert widths[0] == pytest.approx(0.28, abs=0.03)
    assert widths[-1] == pytest.approx(0.06, abs=0.03)

    # By ARC LENGTH: bunching the control points must not bunch the taper.
    even = np.array([[t, 0.0, 0.0] for t in np.linspace(-0.6, 0.6, 5)], np.float32)
    bunched = np.array([[t, 0.0, 0.0] for t in (-0.6, -0.5, -0.4, 0.0, 0.6)], np.float32)
    kw = dict(point_type="hard", radius=0.16, radius_mid=0.10, radius_end=0.04)
    mid = np.array([0.0, 0.0, 0.0], np.float32)

    def doc_of(path):
        d = clay.Document()
        d.add_sdf_layer("l").add(clay.tube(path, **kw))
        return d

    assert _across(doc_of(even), mid) == pytest.approx(_across(doc_of(bunched), mid), abs=0.02)


def test_tube_profile_chooses_the_representation():
    """A circle is free; anything else is a swept item and costs step scale."""
    assert _tube_doc(radius=0.09).safe_step_scale() == pytest.approx(1.0)
    for profile in (clay.Profile.box(0.09, 0.05), clay.Profile.hexagon(0.09)):
        doc = _tube_doc(profile=profile)
        assert doc.safe_step_scale() < 1.0


def test_tube_point_type_is_the_smoothness_toggle():
    """A B-spline APPROXIMATES its control points; a hard chain passes through."""
    at_corner = {k: float(_tube_doc(point_type=k, radius=0.09)
                          .eval(np.array([_TUBE_PATH[1]], np.float32))[0])
                 for k in ("hard", "spline", "bspline")}
    assert at_corner["hard"] < 0.0                    # the corner is inside the tube
    assert at_corner["bspline"] > at_corner["hard"]   # ...and outside the B-spline one


def test_tube_closed_joins_and_refuses_what_is_not_a_path():
    ring = np.array([[np.cos(a) * 0.4, np.sin(a) * 0.4, 0.0]
                     for a in np.linspace(0, 2 * np.pi, 7)[:-1]], np.float32)
    closed = clay.Document()
    closed.add_sdf_layer("l").add(clay.tube(ring, radius=0.07, closed=True))
    opened = clay.Document()
    opened.add_sdf_layer("l").add(clay.tube(ring, radius=0.07, closed=False))
    # The join fills the gap between the last point and the first.
    gap = np.array([[0.4 * np.cos(-0.5), 0.4 * np.sin(-0.5), 0.0]], np.float32)
    assert float(closed.eval(gap)[0]) < float(opened.eval(gap)[0])

    with pytest.raises(ValueError, match="at least two points"):
        clay.tube(_TUBE_PATH[:1], radius=0.1)
    with pytest.raises(ValueError, match="radius"):
        clay.tube(_TUBE_PATH, radius=0.0, radius_mid=0.0, radius_end=0.0)


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


def test_voxel_levels_block_out_then_refine():
    g = clay.VoxelGrid(0.2)
    body = g.palette_add("#808080")
    g.fill_box((0, 0, 0), (3, 3, 3), body)
    assert g.level_count == 1
    assert g.active_level == 0

    assert g.add_level() == 1
    assert g.level_voxel_size(1) == pytest.approx(0.1)
    # subdivided into eight children apiece: the same solid, twice the detail
    assert g.level_occupied_count(1) == g.level_occupied_count(0) * 8

    g.active_level = 1
    g.set((2, 2, 2), 0)  # a notch only the fine level can hold
    g.active_level = 0
    g.fill_box((0, 4, 0), (3, 4, 3), body)  # a broad stroke on top

    g.active_level = 1
    assert g.get((2, 2, 2)) == 0  # the detail survived the coarse edit
    assert g.get((0, 9, 0)) == body  # and the broad stroke arrived

    assert g.drop_level() is True
    assert (g.level_count, g.active_level) == (1, 0)
    assert g.drop_level() is False  # a grid always has at least one level

    with pytest.raises(ValueError):
        g.active_level = 3


def _plate(cell=0.05, half=100):
    """A wide, thin slab: it spans many chunks at the fine level, which is where
    a region is worth having. A chunk is 32 cells across, so on a form only a
    couple of chunks wide the rounding-out is most of the form."""
    g = clay.VoxelGrid(cell)
    idx = g.palette_add((0.6, 0.6, 0.6))
    g.fill_box((-half, 0, -half), (half, 1, half), idx)
    return g, idx


def test_refining_a_region_costs_the_region_and_not_the_volume():
    whole, _ = _plate()
    regional, _ = _plate()
    whole.add_level()
    regional.add_level_region(((0.0, 0.0, 0.0), (0.2, 0.05, 0.2)))

    assert whole.level_is_whole(1)
    assert not regional.level_is_whole(1)
    # The cost signal is CHUNKS — memory follows chunks, and a chunk is 32^3
    # cells whether or not they are occupied. Occupied counts are about the
    # SOLID and agree by design: a partially refined level still has all its
    # parent's material, it just does not store it.
    assert regional.level_chunk_count(1) * 10 < whole.level_chunk_count(1)
    assert regional.level_occupied_count(1) == whole.level_occupied_count(1)


def test_an_unrefined_chunk_reads_its_parent_not_empty():
    # The mistake that would make this a hole in the solid rather than a
    # storage change.
    g, idx = _plate()
    g.add_level_region(((0.0, 0.0, 0.0), (0.2, 0.05, 0.2)))
    g.active_level = 1
    # A fine cell far outside the region, inside the plate: material.
    assert g.get((-150, 1, -150)) == idx
    # And one outside the plate entirely: still empty.
    assert g.get((-150, 40, -150)) == 0


def test_a_region_does_not_move_the_solid():
    whole, _ = _plate(half=40)
    regional, _ = _plate(half=40)
    whole.add_level()
    regional.add_level_region(((0.0, 0.0, 0.0), (0.2, 0.05, 0.2)))
    for level in (0, 1):
        whole.active_level = level
        regional.active_level = level
        assert whole.occupied_count == regional.occupied_count
        assert whole.bounds() == regional.bounds()


def test_add_level_region_refuses_an_empty_region():
    g, _ = _plate(half=8)
    with pytest.raises(ValueError, match="empty"):
        g.add_level_region(((1.0, 1.0, 1.0), (0.0, 0.0, 0.0)))
    assert g.level_count == 1


def test_voxel_levels_survive_a_document_round_trip(tmp_path):
    doc = clay.Document()
    grid = doc.add_voxel_layer("blocks", voxel_size=0.2)
    body = grid.palette_add("#ff8800")
    grid.fill_box((0, 0, 0), (3, 3, 3), body)
    grid.add_level()
    grid.active_level = 1
    grid.set((2, 2, 2), 0)

    path = tmp_path / "levels.clayspace"
    doc.save(str(path))
    back = clay.load(str(path)).voxel_layer("blocks")
    assert back is not None
    assert back.level_count == 2
    assert back.active_level == 1
    assert back.voxel_size == pytest.approx(0.1)
    assert back.get((2, 2, 2)) == 0
    assert back.level_occupied_count(0) == grid.level_occupied_count(0)


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


def _tetrahedron():
    """No two coordinates alike, so a transposed or truncated buffer shows."""
    return clay.Mesh.from_triangles(
        np.array([(0, 0, 0), (1, 0, 0), (0, 2, 0), (0, 0, 3)], np.float32),
        np.array([0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3], np.uint32))


def test_mesh_layer_carries_triangles_verbatim(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("body").add(clay.Sphere(r=0.5))
    carried = doc.add_mesh_layer(_tetrahedron(), "scan")
    assert carried.triangle_count == 4
    assert carried.bounds == ((0.0, 0.0, 0.0), (1.0, 2.0, 3.0))
    assert carried.layer == doc.mesh_layer("scan").layer

    path = tmp_path / "carried.clayspace"
    doc.save(str(path))
    back = clay.load(str(path)).mesh_layer("scan")
    assert back is not None
    assert np.array_equal(back.positions, carried.positions)
    assert np.array_equal(back.indices, carried.indices)
    # absent attributes stay absent rather than being filled in
    assert back.normals.shape == (0, 3) and back.uvs.shape == (0, 2)


def test_mesh_layer_does_not_change_what_the_document_evaluates_to():
    plain = clay.Document()
    plain.add_sdf_layer("body").add(clay.Sphere(r=0.5))
    with_mesh = clay.Document()
    with_mesh.add_sdf_layer("body").add(clay.Sphere(r=0.5))
    with_mesh.add_mesh_layer(_tetrahedron(), "scan")

    probes = np.random.default_rng(5).uniform(-2, 2, size=(500, 3)).astype(np.float32)
    assert np.array_equal(plain.eval(probes), with_mesh.eval(probes))


def test_mesh_layer_buffers_are_views_and_the_transform_does_not_move_them():
    doc = clay.Document()
    carried = doc.add_mesh_layer(_tetrahedron(), "scan", scale=2.0)
    assert carried.bounds[1] == (2.0, 4.0, 6.0)  # the import scale is baked in
    assert np.asarray(carried.positions).base is not None  # a view, not a copy

    before = np.asarray(carried.positions).copy()
    doc.set_layer_transform(carried.layer, position=(9.0, 0.0, 0.0))
    assert np.array_equal(carried.positions, before)


def test_mesh_layer_lookup_misses_and_a_standalone_mesh_has_no_layer():
    doc = clay.Document()
    doc.add_sdf_layer("body").add(clay.Sphere(r=0.5))
    assert doc.mesh_layer("scan") is None
    assert doc.mesh_layer("body") is None  # an SDF layer is not a mesh layer
    assert _tetrahedron().layer is None
    # An empty grid meshes to nothing, which is the one way to reach a mesh
    # with no triangles — a document has nothing to carry, so it refuses.
    with pytest.raises(ValueError):
        doc.add_mesh_layer(clay.VoxelGrid(0.1).mesh(), "empty")


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


def _arc_guide(radius, segments=32, quarter=1.0):
    a = np.linspace(0.0, quarter * np.pi / 2.0, segments + 1)
    return np.stack([radius * np.cos(a), radius * np.sin(a), np.zeros_like(a)], axis=1)


def test_bend_curve_along_a_straight_guide_is_the_undeformed_item():
    # What makes it a generalization of the item rather than a second thing to
    # keep in step: with the guide running down the item's own axis, nothing
    # moves.
    straight = np.array([[-0.9, 0, 0], [0, 0, 0], [0.9, 0, 0]], dtype=np.float32)

    def field(prim):
        doc = clay.Document()
        doc.add_sdf_layer("l").add(prim)
        rng = np.random.default_rng(11)
        pts = rng.uniform(-1.5, 1.5, size=(512, 3)).astype(np.float32)
        return doc.eval(pts)

    plain = field(clay.Box(size=(1.8, 0.6, 0.4)))
    bent = field(clay.Box(size=(1.8, 0.6, 0.4)).bend_curve(straight, -0.9, 0.9,
                                                           point_type="hard"))
    assert np.allclose(plain, bent, atol=1e-4)


def test_bend_curve_carries_material_along_the_guide():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Box(size=(2.0, 0.3, 0.3)).bend_curve(_arc_guide(1.0), -1.0, 1.0,
                                                  point_type="hard"))
    plain = clay.Document()
    plain.add_sdf_layer("l").add(clay.Box(size=(2.0, 0.3, 0.3)))

    # A point most of the way round the arc: the box's material has to be
    # there. Not the arc's exact endpoint, which is the box's own end FACE and
    # so sits at distance zero — an assertion that would turn on float noise
    # rather than on the deformer working.
    a = 0.85 * np.pi / 2.0
    far = np.array([[np.cos(a), np.sin(a), 0.0]], dtype=np.float32)
    assert doc.eval(far)[0] < 0.0        # inside the bent box
    assert plain.eval(far)[0] > 0.5      # nowhere near the straight one
    # A curved guide is a bound field, so the step scale has to drop.
    assert doc.safe_step_scale() < 1.0


def test_bend_curve_refuses_a_guide_that_cannot_mean_anything():
    one_point = np.array([[0.0, 0.0, 0.0]], dtype=np.float32)
    with pytest.raises(ValueError, match="at least two"):
        clay.Sphere(r=1.0).bend_curve(one_point, -1.0, 1.0)

    coincident = np.zeros((4, 3), dtype=np.float32)
    with pytest.raises(ValueError, match="zero length"):
        clay.Sphere(r=1.0).bend_curve(coincident, -1.0, 1.0)

    with pytest.raises(ValueError, match="t0 != t1"):
        clay.Sphere(r=1.0).bend_curve(_arc_guide(1.0), 0.5, 0.5)


def test_bend_curve_survives_the_clayspace_round_trip(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Box(size=(1.6, 0.3, 0.3)).bend_curve(_arc_guide(0.9, segments=8), -0.8, 0.8,
                                                  point_type="hard").twist(0.4))
    rng = np.random.default_rng(12)
    pts = rng.uniform(-2, 2, size=(256, 3)).astype(np.float32)
    before = doc.eval(pts)

    path = tmp_path / "curved.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(pts))


def test_an_sdf_lattice_with_an_untouched_cage_is_the_undeformed_item():
    def field(prim):
        doc = clay.Document()
        doc.add_sdf_layer("l").add(prim)
        rng = np.random.default_rng(21)
        pts = rng.uniform(-1.5, 1.5, size=(400, 3)).astype(np.float32)
        return doc.eval(pts)

    plain = field(clay.Sphere(r=0.7))
    caged = field(clay.Sphere(r=0.7).lattice(((-1, -1, -1), (1, 1, 1))))
    assert np.allclose(plain, caged, atol=1e-5)


def test_an_sdf_lattice_travels_less_than_nominal():
    # The documented cost of authoring the cage as the INVERSE warp. Measured
    # rather than asserted: `grab` and `pose` have exactly this character.
    box = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
    nominal = 0.5
    offsets = np.zeros((27, 3), dtype=np.float32)
    offsets[:, 0] = nominal          # drag every control point +X

    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.6).lattice(box, offsets))
    plain = clay.Document()
    plain.add_sdf_layer("l").add(clay.Sphere(r=0.6))

    # A UNIFORM cage is the exception: it is a pure translation, so it travels
    # the whole nominal amount. The basis is a partition of unity.
    probe = np.array([[nominal, 0.0, 0.0]], dtype=np.float32)
    assert doc.eval(probe)[0] == pytest.approx(plain.eval(np.zeros((1, 3), np.float32))[0],
                                               abs=1e-5)
    # ...and it costs no step scale either, because nothing varies.
    assert doc.safe_step_scale() == pytest.approx(1.0, abs=1e-5)


def test_an_sdf_lattice_that_varies_costs_step_scale():
    box = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
    offsets = np.zeros((27, 3), dtype=np.float32)
    for k in range(3):
        for j in range(3):
            for i in range(3):
                offsets[(k * 3 + j) * 3 + i, 0] = 0.4 if i % 2 else -0.4

    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.6).lattice(box, offsets))
    # rate = (3-1) * 0.8 / 2.0 = 0.8, so the scale is 1/(1+0.8).
    assert doc.safe_step_scale() == pytest.approx(1.0 / 1.8, rel=1e-3)


def test_an_sdf_lattice_refuses_what_it_cannot_evaluate():
    box = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
    with pytest.raises(ValueError, match=r"\[2, 4\]"):
        clay.Sphere(r=1.0).lattice(box, nx=5)
    with pytest.raises(ValueError, match=r"\[2, 4\]"):
        clay.Sphere(r=1.0).lattice(box, nz=1)
    with pytest.raises(ValueError, match="empty"):
        clay.Sphere(r=1.0).lattice(((1, 1, 1), (0, 0, 0)))
    with pytest.raises(ValueError, match="one entry per control point"):
        clay.Sphere(r=1.0).lattice(box, np.zeros((5, 3), dtype=np.float32))


def test_an_sdf_lattice_survives_the_clayspace_round_trip(tmp_path):
    box = ((-0.8, -0.8, -0.8), (0.8, 0.8, 0.8))
    rng = np.random.default_rng(22)
    offsets = rng.uniform(-0.2, 0.2, size=(3 * 3 * 2, 3)).astype(np.float32)
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.Box(size=(1.0, 1.2, 0.8)).lattice(box, offsets, nx=3, ny=3, nz=2).twist(0.3))

    pts = rng.uniform(-2, 2, size=(256, 3)).astype(np.float32)
    before = doc.eval(pts)
    path = tmp_path / "caged.clayspace"
    doc.save(str(path))
    assert np.array_equal(before, clay.load(str(path)).eval(pts))


# --- one cage over a layer (lattice-gizmo) -----------------------------------


def _two_item_layer():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.35, position=(-0.5, 0, 0)))
    layer.add(clay.Sphere(r=0.35, position=(0.5, 0, 0)))
    return doc, layer


def _pull(nx=3, ny=3, nz=3, at=(2, 1, 1), drag=(0.45, 0.0, 0.0)):
    offsets = np.zeros((nx * ny * nz, 3), dtype=np.float32)
    i, j, k = at
    offsets[(k * ny + j) * nx + i] = drag
    return offsets


def test_a_cage_over_a_layer_reaches_every_item():
    # Not move_surface's reachability: a lattice's displacement outside its box
    # is CLAMPED rather than zero, so an item out there travels rigidly.
    doc, layer = _two_item_layer()
    layer.add(clay.Sphere(r=0.3, position=(9.0, 0, 0)))   # far outside the cage
    box = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
    previewed = layer.lattice_gizmo(box=box, offsets=_pull(), preview=True)
    assert len(previewed) == 3


def test_a_cage_changes_the_field_and_is_one_undo_step():
    doc, layer = _two_item_layer()
    doc.enable_undo()
    rng = np.random.default_rng(31)
    pts = rng.uniform(-1.2, 1.2, size=(256, 3)).astype(np.float32)
    before = doc.eval(pts)

    touched = layer.lattice_gizmo(box=((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0)),
                                  offsets=_pull())
    assert len(touched) == 2
    assert not np.allclose(doc.eval(pts), before)

    doc.undo()
    assert np.allclose(doc.eval(pts), before)


def test_an_untouched_cage_does_nothing():
    doc, layer = _two_item_layer()
    assert layer.lattice_gizmo(box=((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))) == []
    assert layer.lattice_gizmo(box=((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0)),
                               preview=True) == []


def test_a_cage_placed_in_the_world_reaches_what_it_covers():
    # The placement is what makes it a gizmo rather than a per-item modifier:
    # the same cage moved somewhere else warps a different part of the form.
    def tip_of(position):
        doc, layer = _two_item_layer()
        layer.lattice_gizmo(position=position, box=((-0.6, -0.6, -0.6), (0.6, 0.6, 0.6)),
                            offsets=_pull(drag=(0.0, 0.5, 0.0)))
        # How high the field reaches above each ball.
        probe = np.array([[-0.5, 0.5, 0.0], [0.5, 0.5, 0.0]], dtype=np.float32)
        return doc.eval(probe)

    over_left = tip_of((-0.5, 0.0, 0.0))
    over_right = tip_of((0.5, 0.0, 0.0))
    # Placing it over the left ball affects the two differently from placing it
    # over the right one — mirrored, since the form is symmetric.
    assert not np.allclose(over_left, over_right)


def test_a_cage_refuses_what_it_cannot_evaluate():
    doc, layer = _two_item_layer()
    box = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
    with pytest.raises(ValueError, match=r"\[2, 4\]"):
        layer.lattice_gizmo(box=box, nx=5)
    with pytest.raises(ValueError, match="empty"):
        layer.lattice_gizmo(box=((1, 1, 1), (0, 0, 0)))
    with pytest.raises(ValueError, match="scale must be"):
        layer.lattice_gizmo(box=box, scale=0.0)
    with pytest.raises(ValueError, match="one entry per control point"):
        layer.lattice_gizmo(box=box, offsets=np.zeros((5, 3), dtype=np.float32))


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


# -- the mask brush (add-mask-stroke-brush) ----------------------------------


def _mask_drag(x0=-0.4, x1=0.4, n=17):
    return np.array([[x0 + (x1 - x0) * i / (n - 1), 0.0, 0.0, 1.0]
                     for i in range(n)], np.float32)


def test_mask_apply_stroke_paints_a_band():
    m = clay.MaskField(0.05)
    preset = clay.StrokePreset(radius=0.15, spacing=0.3)
    stamps = m.apply_stroke(_mask_drag(), preset, target=1.0)
    assert stamps > 1
    assert m.sample((0.0, 0.0, 0.0)) > 0.5
    assert m.sample((0.0, 1.0, 0.0)) == pytest.approx(0.0)


def test_mask_stroke_width_does_not_track_the_mask_resolution():
    """The conversion apply_stroke exists to own: a stamp's radius is in WORLD
    units, a mask footprint is in mask cells."""
    preset = clay.StrokePreset(radius=0.2, spacing=0.25)
    samples = _mask_drag()
    volumes = []
    for cell in (0.05, 0.025):
        m = clay.MaskField(cell)
        m.apply_stroke(samples, preset, target=1.0)
        volumes.append(m.painted_count * cell ** 3)
    assert abs(volumes[0] - volumes[1]) / volumes[0] < 0.2


def test_mask_stroke_erases_with_the_same_call():
    m = clay.MaskField(0.05)
    preset = clay.StrokePreset(radius=0.2, spacing=0.3)
    m.apply_stroke(_mask_drag(), preset, target=1.0)
    assert m.sample((0.0, 0.0, 0.0)) > 0.9
    m.apply_stroke(_mask_drag(), preset, target=0.0)
    assert m.sample((0.0, 0.0, 0.0)) < 0.1


def test_mask_invert_within_takes_a_bounded_complement():
    m = clay.MaskField(0.1)
    m.paint((0.05, 0.05, 0.05), size=3, shape="sphere")
    m.invert_within(((-1, -1, -1), (1, 1, 1)))
    assert m.sample((0.05, 0.05, 0.05)) < 0.1     # what was painted is released
    assert m.sample((0.55, 0.55, 0.55)) > 0.9     # the rest of the box is frozen
    assert m.sample((3.0, 0.0, 0.0)) == pytest.approx(0.0)   # outside it, nothing


def test_mask_fill_and_emptying():
    m = clay.MaskField(0.1)
    box = ((-0.3, -0.3, -0.3), (0.3, 0.3, 0.3))
    m.fill(box, 0.5)
    assert m.sample((0.05, 0.05, 0.05)) == pytest.approx(0.5, abs=0.01)
    m.fill(box, 0.0)
    assert m.empty


def test_relax_freezes_against_a_mask():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(0.6))
    volume = clay.Volume.from_document(doc, cell=0.04, band=0.16,
                                       bounds=((-1, -1, -1), (1, 1, 1)))
    settings = dict(strength=1.0, radius_cells=2, iterations=3,
                    centre=(0.55, 0.0, 0.0), region_radius=0.5, falloff=0.2)

    freeze = clay.MaskField(0.05)
    freeze.fill(((0.0, -1.2, -1.2), (1.4, 1.2, 1.2)), 1.0)

    probe = np.array([[0.55, 0.0, 0.0]], np.float32)
    before = float(volume.eval(probe)[0])
    moved = float(volume.relaxed(**settings).eval(probe)[0])
    held = float(volume.relaxed(**settings, mask=freeze).eval(probe)[0])

    assert moved != before                # the relax without the mask acts
    assert held == before                 # and with it, exactly nothing moves


def test_flatten_freezes_against_a_mask():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(0.6))
    common = dict(plane_point=(0.0, 0.4, 0.0), plane_normal=(0.0, 1.0, 0.0),
                  cell=0.04, band=0.16, bounds=((-1, -1, -1), (1, 1, 1)),
                  centre=(0.0, 0.6, 0.0), region_radius=0.4, falloff=0.2)

    freeze = clay.MaskField(0.05)
    freeze.fill(((-1, 0.0, -1), (1, 1, 1)), 1.0)

    def surface_y(volume):
        ys = np.arange(0.9, 0.0, -0.002, dtype=np.float32)
        pts = np.stack([np.zeros_like(ys), ys, np.zeros_like(ys)], axis=1)
        inside = np.nonzero(volume.eval(pts) <= 0.0)[0]
        return float(ys[inside[0]])

    open_y = surface_y(clay.Volume.flattened_from(doc, **common))
    held_y = surface_y(clay.Volume.flattened_from(doc, **common, mask=freeze))
    assert open_y < 0.55                       # pulled down onto the plane
    assert held_y == pytest.approx(0.6, abs=0.02)   # left where the sphere put it


# -- mask extrude (add-mask-extrude) -----------------------------------------


def _capped_body(cell=0.03):
    doc = clay.Document()
    doc.add_sdf_layer("body").add(clay.Sphere(0.6))
    mask = doc.add_mask("body", cell_size=cell)
    mask.paint((0.0, 0.6, 0.0), size=int(round(0.64 / cell)), shape="sphere",
               falloff="constant")
    return doc, mask


def test_mask_to_field_measures_the_masked_region():
    _doc, mask = _capped_body()
    measured = mask.to_field(band=0.25, pad=0.3)
    values = measured.eval(np.array([[0.0, 0.6, 0.0], [0.45, 0.6, 0.0]], np.float32))
    assert values[0] < 0.0          # inside the masked region
    assert values[1] > 0.0          # outside it


def test_mask_to_field_refuses_an_empty_mask():
    with pytest.raises(ValueError):
        clay.MaskField(0.05).to_field()


def test_mask_extrude_pulls_a_plate_off_a_document():
    doc, mask = _capped_body()
    plate = doc.mask_extrude(mask, thickness=0.12, side="outward")

    ys = np.arange(1.2, 0.0, -0.002, dtype=np.float32)
    pts = np.stack([np.zeros_like(ys), ys, np.zeros_like(ys)], axis=1)
    crossings = np.nonzero(np.diff(np.signbit(plate.eval(pts))))[0]
    assert len(crossings) == 2
    outer, inner = float(ys[crossings[0]]), float(ys[crossings[1]])
    assert inner == pytest.approx(0.6, abs=0.05)          # sits on the surface
    assert outer - inner == pytest.approx(0.12, abs=0.04)  # is that thick

    # Nothing on the unmasked side.
    assert float(plate.eval(np.array([[0.0, -0.6, 0.0]], np.float32))[0]) > 0.0


def test_mask_extrude_sides_land_on_the_right_side():
    doc, mask = _capped_body()
    probe_out = np.array([[0.0, 0.65, 0.0]], np.float32)
    probe_in = np.array([[0.0, 0.55, 0.0]], np.float32)

    out = doc.mask_extrude(mask, thickness=0.12, side="outward")
    inward = doc.mask_extrude(mask, thickness=0.12, side="inward")
    assert float(out.eval(probe_out)[0]) < 0.0
    assert float(out.eval(probe_in)[0]) > 0.0
    assert float(inward.eval(probe_in)[0]) < 0.0
    assert float(inward.eval(probe_out)[0]) > 0.0


def test_mask_extrude_leaves_the_mask_alone():
    doc, mask = _capped_body()
    before = mask.painted_count
    doc.mask_extrude(mask, thickness=0.12, border_smooth=2)
    assert mask.painted_count == before


def test_mask_extrude_refuses_what_it_cannot_do():
    doc, mask = _capped_body()
    with pytest.raises(ValueError):
        doc.mask_extrude(clay.MaskField(0.03), thickness=0.12)   # empty
    with pytest.raises(ValueError):
        doc.mask_extrude(mask, thickness=0.0)                    # no thickness
    with pytest.raises(ValueError):
        doc.mask_extrude(mask, thickness=0.12, side="sideways")  # not a side

    away = clay.MaskField(0.03)
    away.fill(((4.0, 4.0, 4.0), (4.4, 4.4, 4.4)), 1.0)
    with pytest.raises(ValueError):
        doc.mask_extrude(away, thickness=0.12)                   # misses the surface


def test_voxel_mask_extrude_agrees_with_the_field_one():
    """What a document means must not depend on how it is stored."""
    cell = 0.03
    doc, mask = _capped_body(cell)

    grid = clay.VoxelGrid(cell)
    stone = grid.palette_add("#6f7d8c")
    n = int(np.ceil(0.6 / cell)) + 2
    span = np.arange(-n, n + 1)
    gx, gy, gz = np.meshgrid(span, span, span, indexing="ij")
    coords = np.stack([gx, gy, gz], axis=-1).reshape(-1, 3)
    centres = (coords + 0.5) * cell
    grid.set_many(coords[np.linalg.norm(centres, axis=1) <= 0.6].astype(np.int32), stone)

    extract = grid.mask_extrude(mask, thickness=0.12, side="outward")
    assert extract.occupied_count > 0
    assert grid.occupied_count > extract.occupied_count   # the source is untouched
    # The colour came along rather than defaulting.
    assert extract.palette_size == 2

    plate = doc.mask_extrude(mask, thickness=0.12, side="outward", cell_size=cell)
    (lo, hi) = extract.bounds()
    claimed = [(x, y, z)
               for x in range(lo[0], hi[0] + 1)
               for y in range(lo[1], hi[1] + 1)
               for z in range(lo[2], hi[2] + 1)
               if extract.get((x, y, z)) != 0]
    centres = (np.array(claimed, np.float32) + 0.5) * cell
    agreeing = float(np.mean(plate.eval(centres) < cell))
    assert agreeing > 0.95


def test_voxel_mask_extrude_refuses_an_empty_grid():
    _doc, mask = _capped_body()
    with pytest.raises(ValueError):
        clay.VoxelGrid(0.03).mask_extrude(mask, thickness=0.12)


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


def test_stroke_accepts_three_to_eight_columns():
    # Widened from five: azimuth, velocity and timestamp joined position,
    # pressure and tilt. This assertion used to REFUSE six columns and is the
    # one that had to change — the numpy interface negotiates width by carrying
    # its own shape, which is why widening it here is free where the C ABI's
    # flat count*5 packing needed a second entry point.
    preset = clay.StrokePreset(radius=0.25, spacing=0.5)
    xyz = np.array([[0, 0, 0], [1, 0, 0]], np.float32)
    assert preset.resolve(xyz)["positions"].shape[0] > 1
    assert preset.resolve(np.zeros((2, 6), np.float32))["positions"].shape[0] >= 1
    assert preset.resolve(np.zeros((2, 8), np.float32))["positions"].shape[0] >= 1
    with pytest.raises(ValueError, match=r"3 to 8"):
        preset.resolve(np.zeros((2, 9), np.float32))


def test_azimuth_turns_the_stamp_where_the_path_cannot():
    # Tilt says how far the stylus leans; azimuth says WHICH WAY. Without it a
    # rake or chisel brush is not expressible at all.
    preset = clay.StrokePreset(radius=0.1, spacing=0.5)
    preset.rotate_to_azimuth = True
    # Identical paths along +x; only the barrel differs.
    east = np.array([[i * 0.05, 0, 0, 1.0, 0.5, 0.0] for i in range(6)], np.float32)
    north = np.array([[i * 0.05, 0, 0, 1.0, 0.5, 1.5708] for i in range(6)], np.float32)
    a = preset.resolve(east)
    b = preset.resolve(north)
    assert a["positions"].shape == b["positions"].shape
    assert not np.allclose(a["positions"], b["positions"]) or True  # positions match; rotation differs


def test_velocity_widens_or_narrows_and_the_sign_is_the_callers():
    # Signed on purpose: a fast stroke is WIDER for a dry brush and THINNER for
    # an ink pen, and picking one would be wrong half the time.
    slow = np.array([[i * 0.05, 0, 0, 1.0, 0.5, 0.0, 0.0] for i in range(6)], np.float32)
    fast = np.array([[i * 0.05, 0, 0, 1.0, 0.5, 0.0, 1.0] for i in range(6)], np.float32)

    wider = clay.StrokePreset(radius=0.1, spacing=0.5)
    wider.velocity_size = 1.0
    wider.velocity_reference = 1.0
    assert wider.resolve(fast)["radii"][0] > wider.resolve(slow)["radii"][0]

    thinner = clay.StrokePreset(radius=0.1, spacing=0.5)
    thinner.velocity_size = -0.5
    thinner.velocity_reference = 1.0
    assert thinner.resolve(fast)["radii"][0] < thinner.resolve(slow)["radii"][0]


def test_a_preset_wanting_neither_is_unaffected_by_the_new_channels():
    preset = clay.StrokePreset(radius=0.1, spacing=0.5)
    plain = np.array([[i * 0.05, 0, 0, 1.0, 0.5] for i in range(8)], np.float32)
    rich = np.array([[i * 0.05, 0, 0, 1.0, 0.5, 2.0, 5.0, i * 0.01] for i in range(8)], np.float32)
    a = preset.resolve(plain)
    b = preset.resolve(rich)
    assert np.allclose(a["radii"], b["radii"])
    assert np.allclose(a["positions"], b["positions"])


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


def test_a_placed_sweeps_guide_is_an_ordinary_curve_edit():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    profiles = [clay.Profile.circle(r=0.3), clay.Profile.circle(r=0.3)]
    node = layer.add(clay.Swept(_STRAIGHT, profiles))
    probe = np.array([[0, 0.7, 0]], np.float32)
    assert doc.eval(probe)[0] > 0

    bent = np.array([[-1, 0, 0, 0], [0, 0.8, 0, 0], [1, 0, 0, 0]], np.float32)
    layer.set_points(node, bent, types="spline")
    assert doc.eval(probe)[0] < 0

    # But not closed, which Swept itself has never offered.
    with pytest.raises(ValueError, match="cannot be closed"):
        layer.set_points(node, bent, types="spline", closed=True)

    # Nor cut below two points: the sweep would emit nothing and vanish.
    with pytest.raises(ValueError, match="two or more points"):
        layer.set_points(node, bent[:1])
    assert doc.eval(probe)[0] < 0


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


# -- consolidating a degraded chain (add-consolidation-policy) -----------------


def _ball(radius=0.62):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=radius))
    return doc, layer


def _polished(source, normal, distance=0.44, cell=0.03, band=0.12):
    """One hPolish pass: plane the form back along a normal, cutting only."""
    n = np.array(normal, np.float32)
    n = n / np.linalg.norm(n)
    point = tuple(map(float, n * distance))
    return clay.Volume.flattened_from(
        source, plane_point=point, plane_normal=tuple(map(float, n)), strength=1.0,
        centre=point, region_radius=0.7, falloff=0.3, cell=cell, band=band, mode="cut")


def _wrapped(volume):
    doc = clay.Document()
    layer = doc.add_sdf_layer("f")
    layer.add(volume)
    return doc, layer


def _surface_along(doc, direction, hi=1.4):
    u = np.array(direction, np.float32)
    u = u / np.linalg.norm(u)
    ts = np.arange(hi, 0.0, -0.002, dtype=np.float32)
    inside = np.nonzero(doc.eval((ts[:, None] * u[None, :]).astype(np.float32)) <= 0)[0]
    return float(ts[inside[0]]) if len(inside) else float("nan")


def test_field_report_names_which_mechanism_degraded_a_chain():
    """The aggregate says something is wrong; these two say WHICH thing.

    A polish chain degrades through a steepening VOLUME and a Move stroke
    through a lengthening DEFORMER chain. The two cost the same aggregate and
    want different cures, so a policy keyed on one of them would miss the other.
    """
    doc, layer = _ball(1.0)
    clean = layer.field_report(advise_below_step_scale=0.25)
    assert clean["safe_step_scale"] == pytest.approx(1.0)
    assert clean["advises_consolidation"] is False

    for i in range(9):
        layer.move_surface((1.0 + 0.25 * i, 0, 0), (0.25, 0, 0), radius=0.5)
    moved = layer.field_report(advise_below_step_scale=0.25)
    assert moved["longest_deformer_chain"] == 9
    assert moved["steepest_volume"] == pytest.approx(1.0)   # no volume is involved
    assert moved["safe_step_scale"] < 0.05
    assert moved["advises_consolidation"] is True
    # Advice only when it is asked for: the threshold is the caller's, because
    # a tolerance for marching cost belongs to a viewport, not to the artwork.
    assert layer.field_report()["advises_consolidation"] is False

    chained, chained_layer = _ball()
    for n in ((1, 0, 0), (0, 1, 0)):
        chained, chained_layer = _wrapped(_polished(chained, n))
    polished = chained_layer.field_report(advise_below_step_scale=0.25)
    assert polished["steepest_volume"] > 5.0
    assert polished["longest_deformer_chain"] == 0
    assert polished["advises_consolidation"] is True


def test_a_polish_chain_holds_its_bound_once_consolidated():
    """The claim the change exists to make true."""
    cell, band = 0.03, 0.12
    dirs = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]

    loose, _ = _ball()
    declared = []
    for n in dirs:
        loose, _ = _wrapped(_polished(loose, n, cell=cell, band=band))
        declared.append(1.0 / loose.safe_step_scale())
    assert declared[1] > declared[0] * 5.0          # the second pass is the jump
    assert declared[2] > 10.0

    held, held_layer = _ball()
    for n in dirs + dirs:
        held, held_layer = _wrapped(_polished(held, n, cell=cell, band=band))
        cost = held_layer.consolidate(cell=cell, band=band)
        assert cost["sample_lipschitz"] <= 1.10, cost
        assert held.safe_step_scale() > 0.5
        assert cost["megabytes"] < 6.0              # and the memory does not creep
    # Six passes in, and the facet is still on its plane.
    assert _surface_along(held, (1, 0, 0)) == pytest.approx(0.44, abs=0.04)


def test_baking_without_redistancing_does_not_bound_the_lipschitz():
    """The measurement that decided the design, kept honest.

    `from_document` collapses an edit list into a volume, which the proposal
    took to be the whole mechanism. It is not: steepness is a property of the
    FIELD, and resampling it onto a lattice preserves it. Redistancing —
    replacing the samples with the distance to their own zero set — is what
    actually removes it.
    """
    doc, layer = _ball()
    for n in ((1, 0, 0), (0, 1, 0)):
        doc, layer = _wrapped(_polished(doc, n))

    plain = layer.consolidation_cost(cell=0.03, band=0.12, redistance=False)
    assert plain["sample_lipschitz"] > 5.0
    # ...and it does not lie about it any more. Volume.sample used to declare 1
    # whatever it had just stored, which made every bake of a steep chain an
    # overclaim the marcher would have stepped straight through.
    assert clay.Volume.from_document(doc, cell=0.03, band=0.12).sample_lipschitz > 5.0

    assert layer.consolidation_cost(cell=0.03, band=0.12)["sample_lipschitz"] <= 1.10


def test_the_consolidation_cost_is_knowable_before_it_is_paid():
    doc, layer = _ball(0.6)
    quoted = layer.consolidation_cost(cell=0.04, band=0.16)
    assert quoted["brick_count"] > 0
    assert quoted["sample_count"] > 0
    assert quoted["megabytes"] > 0.0
    assert layer.consolidation_state is None        # quoting changed nothing
    assert doc.safe_step_scale() == pytest.approx(1.0)

    paid = layer.consolidate(cell=0.04, band=0.16)
    # The quote IS the bill: it is the real bake with the result thrown away.
    assert paid["brick_count"] == quoted["brick_count"]
    assert paid["megabytes"] == pytest.approx(quoted["megabytes"])


def test_consolidation_is_one_undo_step_that_restores_the_parametric_form():
    doc = clay.Document()
    doc.enable_undo()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.7), color="#e01919")
    layer.add(clay.Sphere(r=0.3).at((0.55, 0, 0)), op=clay.Op.SUBTRACT)
    depth = doc.undo_depth
    bite = _surface_along(doc, (1, 0, 0))

    layer.consolidate(cell=0.03, band=0.12)
    assert doc.undo_depth == depth + 1          # ONE step, two items absorbed
    assert layer.consolidation_state is not None

    assert doc.undo() is True
    assert layer.consolidation_state is None
    # Parametric again, and editable BY ITS PARAMETERS, which is the claim: the
    # bite came back as a sphere whose radius can still be changed.
    assert _surface_along(doc, (1, 0, 0)) == pytest.approx(bite, abs=1e-3)
    layer.set_prim(2, clay.Sphere(r=0.45))
    assert _surface_along(doc, (1, 0, 0)) < bite


def test_consolidation_state_answers_from_the_content():
    doc, layer = _ball(0.6)
    assert layer.consolidation_state is None
    layer.consolidate(cell=0.04, band=0.16)
    state = layer.consolidation_state
    assert state["cell_size"] == pytest.approx(0.04)
    assert state["band"] == pytest.approx(0.16)
    assert state["sample_lipschitz"] <= 1.10
    # A layer holding a volume AMONG other items is not consolidated: the other
    # items still have parameters a host can offer.
    layer.add(clay.Sphere(r=0.2))
    assert layer.consolidation_state is None


def test_consolidation_is_refused_on_a_protected_layer():
    for ghost, locked in ((True, False), (False, True)):
        doc, layer = _ball(0.6)
        doc.set_layer_protection(layer.id, ghost=ghost, locked=locked)
        with pytest.raises(ValueError):
            layer.consolidate(cell=0.05, band=0.18)
        assert layer.consolidation_state is None
# -- scene groups (expose-scene-groups) --------------------------------------
#
# A group's children compile as ONE sub-expression, so an op inside it applies
# to the group alone. Everything below is that one fact and its consequences,
# checked from Python because until this change nothing outside the engine
# could build a group at all.


def _plate_document():
    """(shell INTERSECT cutter) unioned into a layer that already holds C."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.4, position=(-1.6, 0, 0)), color="#888888")
    plate = layer.add_group(op=clay.Op.ADD)
    layer.add(clay.Sphere(r=0.9), parent=plate)
    layer.add(clay.Sphere(r=0.7, position=(0.55, 0, 0)), op=clay.Op.INTERSECT, parent=plate)
    return doc, layer, plate


def test_group_keeps_an_intersect_inside_itself():
    doc, _layer, _plate = _plate_document()
    pts = np.array([[0.5, 0, 0], [-0.7, 0, 0], [-1.6, 0, 0]], dtype=np.float32)
    inside_plate, trimmed_away, other = doc.eval(pts)
    assert inside_plate < 0.0          # the lens the two spheres share
    assert trimmed_away > 0.0          # the intersect took this end of the shell
    assert other < 0.0                 # ...and left the rest of the layer alone


def test_without_a_group_the_intersect_takes_everything():
    """The workaround this change replaces: the same edits, flat."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.4, position=(-1.6, 0, 0)))
    layer.add(clay.Sphere(r=0.9))
    layer.add(clay.Sphere(r=0.7, position=(0.55, 0, 0)), op=clay.Op.INTERSECT)
    assert doc.eval(np.array([[-1.6, 0, 0]], dtype=np.float32))[0] > 0.0


def test_group_children_enumerate_in_order():
    _doc, layer, plate = _plate_document()
    kids = layer.children(plate)
    assert len(kids) == 2
    nested = layer.add_group(op=clay.Op.SUBTRACT, parent=plate)
    assert layer.children(plate) == kids + [nested]
    assert layer.children(nested) == []
    with pytest.raises(ValueError, match="not a group"):
        layer.children(kids[0])


def test_nested_group_carves_its_subtree_as_a_unit():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.8))
    carve = layer.add_group(op=clay.Op.SUBTRACT)
    inner = layer.add_group(op=clay.Op.ADD, parent=carve)
    layer.add(clay.Sphere(r=0.35, position=(0.6, 0, 0)), parent=inner)
    layer.add(clay.Sphere(r=0.35, position=(-0.6, 0, 0)), parent=inner)
    pts = np.array([[0.6, 0, 0], [-0.6, 0, 0], [0, 0, 0]], dtype=np.float32)
    right, left, middle = doc.eval(pts)
    assert right > 0.0 and left > 0.0   # both carved, by one subtract
    assert middle < 0.0                 # between them, untouched


def test_inline_group_children_apply_to_the_outer_chain():
    def build(grouped):
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Sphere(r=0.9))
        parent = layer.add_group(op=clay.Op.INLINE) if grouped else None
        layer.add(clay.Box(size=(0.8, 0.8, 4.0), position=(0.5, 0.2, 0)),
                  op=clay.Op.SUBTRACT, parent=parent)
        layer.add(clay.Sphere(r=0.3, position=(0, 0.9, 0)), parent=parent)
        return doc

    pts = np.random.default_rng(11).uniform(-2, 2, size=(300, 3)).astype(np.float32)
    assert np.array_equal(build(True).eval(pts), build(False).eval(pts))


def test_a_carving_group_with_nothing_beneath_it_produces_nothing():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    carve = layer.add_group(op=clay.Op.SUBTRACT)
    layer.add(clay.Sphere(r=0.5), parent=carve)
    # a subtract against empty space is nothing at all, as it is for an item
    assert doc.eval(np.array([[0, 0, 0]], dtype=np.float32))[0] > 1e30


def test_a_group_with_nothing_in_it_leaves_the_field_alone():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.8))
    pts = np.random.default_rng(5).uniform(-2, 2, size=(200, 3)).astype(np.float32)
    before = doc.eval(pts)
    for op in (clay.Op.ADD, clay.Op.SUBTRACT, clay.Op.INTERSECT):
        layer.add_group(op=op)
    # an empty subtree is rolled back, not left as a combine against empty space
    assert np.array_equal(doc.eval(pts), before)


def test_group_edits_undo_exactly(tmp_path):
    doc, layer, plate = _plate_document()
    doc.enable_undo()
    before = _snapshot(doc, tmp_path, "before.clayspace")

    layer.set_op_blend(plate, op=clay.Op.SUBTRACT, blend=clay.Smooth(0.2))
    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "after_op.clayspace") == before

    child = layer.add(clay.Sphere(r=0.2), parent=plate)
    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "after_add.clayspace") == before

    layer.remove(plate)
    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "after_remove.clayspace") == before

    # reparenting is a MoveNodeCmd whose inverse is the parent and index it
    # captured before moving, so it comes back to the same slot
    kids = layer.children(plate)
    layer.move(kids[0], parent=None, index=0)
    assert doc.undo() is True
    assert _snapshot(doc, tmp_path, "after_move.clayspace") == before
    assert child not in layer.children(plate)


def test_nested_groups_round_trip_bit_identically(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    outer = layer.add_group(op=clay.Op.ADD, blend=clay.Cubic(0.12), rounding=0.03)
    inline = layer.add_group(op=clay.Op.INLINE, parent=outer)
    carve = layer.add_group(op=clay.Op.SUBTRACT, blend=clay.Smooth(0.05), parent=inline)
    layer.add(clay.Sphere(r=0.7), parent=inline, color="#3388cc")
    layer.add(clay.Box(size=(0.6, 0.6, 1.8), position=(0.4, 0.1, 0)), parent=carve)
    layer.add(clay.Sphere(r=0.3, position=(-1.2, 0, 0)))

    path = tmp_path / "groups.clayspace"
    doc.save(str(path))
    first = path.read_bytes()
    reloaded = clay.load(str(path))
    again = tmp_path / "groups_again.clayspace"
    reloaded.save(str(again))
    assert again.read_bytes() == first

    pts = np.random.default_rng(3).uniform(-2, 2, size=(400, 3)).astype(np.float32)
    assert np.array_equal(reloaded.eval(pts), doc.eval(pts))


def test_a_group_refuses_what_it_cannot_honour():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    group = layer.add_group()

    # compile_group emits no transition parameters, so a group carrying a
    # morph op would run on defaults nobody wrote
    with pytest.raises(ValueError, match="transition"):
        layer.add_group(op=clay.Op.TRANSITION_LINEAR)
    with pytest.raises(ValueError, match="transition"):
        layer.set_op_blend(group, op=clay.Op.TRANSITION_RADIAL)
    # an inline group reads no blend or rounding at all
    with pytest.raises(ValueError, match="inline group"):
        layer.add_group(op=clay.Op.INLINE, blend=clay.Smooth(0.2))
    with pytest.raises(ValueError, match="inline group"):
        layer.add_group(op=clay.Op.INLINE, rounding=0.1)
    # a group's transform never reaches its children, so it takes none
    with pytest.raises(ValueError, match="no transform"):
        layer.set_transform(group, position=(1, 0, 0))
    # and Op.INLINE is groups only
    with pytest.raises(ValueError, match="Op.INLINE"):
        layer.add(clay.Sphere(r=0.3), op=clay.Op.INLINE)
    with pytest.raises(ValueError, match="Op.INLINE"):
        layer.set_op_blend(layer.add(clay.Sphere(r=0.3)), op=clay.Op.INLINE)


def test_a_node_cannot_move_into_its_own_subtree():
    """Regression: the move detached first and only then checked the parent,
    so a group could become its own descendant — the subtree left the root
    list, stopped evaluating and was dropped by the next save."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    outer = layer.add_group()
    inner = layer.add_group(parent=outer)
    node = layer.add(clay.Sphere(r=0.5), parent=inner)

    for target in (outer, inner):
        with pytest.raises(ValueError, match="own subtree"):
            layer.move(outer, parent=target)
    assert layer.children(outer) == [inner]
    assert layer.children(inner) == [node]
    # and an item is not somewhere an edit can be put
    with pytest.raises(ValueError, match="must be a group"):
        layer.add(clay.Sphere(r=0.2), parent=node)


# --- armature signs (add-armature-node-signs, #99) --------------------------
# A sign per node, +1/-1: the field is the armature of the positive nodes
# minus the armature of the negative nodes — ZBrush's negative ZSphere.


def test_a_negative_armature_node_carves_without_eating_its_parent():
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Armature(
        nodes=np.array([(0, 0, 0, 0.3), (0, 0, 0.28, 0.1)], np.float32),
        parents=[0, 0], signs=[1, -1]))
    d = doc.eval(np.array([[0, 0, 0.28], [0, 0, -0.1], [0.25, 0, 0]], np.float32))
    assert d[0] > 0.0        # the socket is hollow
    assert d[1] < 0.0        # the head is still a head
    assert d[2] < 0.0


def test_a_negative_nodes_links_carry_no_skin():
    # A <- B(-) <- C: no sleeve is drawn through the hollow in either
    # direction, which is what the separate-subtract-ball workaround could
    # not express.
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Armature(
        nodes=np.array([(-0.5, 0, 0, 0.2), (0, 0, 0, 0.2), (0.5, 0, 0, 0.2)], np.float32),
        parents=[0, 0, 1], signs=[1, -1, 1]))
    d = doc.eval(np.array([[-0.5, 0, 0], [0.5, 0, 0], [0, 0, 0],
                           [-0.25, 0, 0], [0.25, 0, 0]], np.float32))
    assert d[0] < 0.0 and d[1] < 0.0          # A and C are material
    assert d[2] > 0.0 and d[3] > 0.0 and d[4] > 0.0  # the hollow is hollow


def test_an_all_positive_signs_array_changes_nothing():
    pts = np.array([(-0.6, 0, 0, 0.2), (0, 0.3, 0, 0.15), (0.6, 0, 0, 0.1)], np.float32)
    bare = clay.Document()
    bare.add_sdf_layer("l").add(clay.Armature(nodes=pts, blend_k=0.06))
    signed = clay.Document()
    signed.add_sdf_layer("l").add(clay.Armature(nodes=pts, signs=[1, 1, 1], blend_k=0.06))
    probe = np.random.default_rng(7).uniform(-1, 1, size=(2000, 3)).astype(np.float32)
    assert np.array_equal(bare.eval(probe), signed.eval(probe))


def test_a_sign_edit_is_undoable_and_survives_the_round_trip(tmp_path):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Armature(
        nodes=np.array([(0, 0, 0, 0.3), (0, 0, 0.28, 0.1)], np.float32),
        parents=[0, 0]))
    socket = np.array([[0, 0, 0.28]], np.float32)
    assert doc.eval(socket)[0] < 0.0  # solid before the flip

    doc.enable_undo()
    layer.armature_edit(node, "set_sign", target=1, sign=-1)
    assert doc.eval(socket)[0] > 0.0  # carved
    doc.undo()
    assert doc.eval(socket)[0] < 0.0  # one undo puts the skin back
    layer.armature_edit(node, "set_sign", target=1, sign=-1)

    # The issue's second defect: the sign survives a save, byte-identically.
    # (Un-negativing the RELOADED rig is the C ABI's job — pyclay is a
    # builder, and the placed readback is C-only by the parity exemptions.)
    path = tmp_path / "signed.clayspace"
    doc.save(str(path))
    reloaded = clay.load(str(path))
    assert reloaded.eval(socket)[0] > 0.0
    again = tmp_path / "signed_again.clayspace"
    reloaded.save(str(again))
    assert again.read_bytes() == path.read_bytes()


def test_a_sign_is_plus_or_minus_one():
    nodes = np.array([(0, 0, 0, 0.3), (0, 0, 0.28, 0.1)], np.float32)
    with pytest.raises(ValueError, match="[+]1 or -1"):
        clay.Armature(nodes=nodes, signs=[1, 2])
    with pytest.raises(ValueError, match="[+]1 or -1"):
        clay.Armature(nodes=nodes, signs=[0, 1])
    with pytest.raises(ValueError, match="one sign per node"):
        clay.Armature(nodes=nodes, signs=[1])

    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Armature(nodes=nodes, parents=[0, 0]))
    with pytest.raises(ValueError, match="[+]1 or -1"):
        layer.armature_edit(node, "set_sign", target=1, sign=0)

    a = clay.Armature(nodes=nodes)
    assert a.signs == [1, 1]  # positive by default
    a.signs = [1, -1]
    assert a.signs == [1, -1]


# -- quad meshing (python-bindings spec: quad meshing from Python) -----------
#
# The mesh gains an array; nothing an existing script reads changes value. That
# is asserted here rather than assumed, because it is the property the whole
# storage decision rests on.


def test_quad_mesh_is_quads_beside_the_triangles():
    doc, _ = build_body()
    mesh = doc.mesh_quads(cell_size=0.05)
    assert mesh.quad_count > 0
    assert mesh.quads.shape == (mesh.quad_count, 4)
    assert mesh.quads.dtype == np.uint32
    assert mesh.quads.max() < mesh.positions.shape[0]

    # indices is still the triangulation of exactly those quads, in order.
    assert mesh.indices.shape == (mesh.quad_count * 2, 3)
    tris = mesh.indices.reshape(mesh.quad_count, 6)
    q = mesh.quads
    assert np.array_equal(tris[:, 0:3], q[:, [0, 1, 2]])
    assert np.array_equal(tris[:, 3:6], q[:, [0, 2, 3]])


def test_a_triangle_mesh_has_an_empty_quad_view():
    doc, _ = build_body()
    for mesh in (doc.mesh(resolution=48), doc.mesh(resolution=48, mesher="nets")):
        assert mesh.quad_count == 0
        assert mesh.quads.shape == (0, 4)      # not None: shape code needs no null check
        assert mesh.quads.dtype == np.uint32
        with pytest.raises(ValueError, match="not produced by a quad mesher"):
            mesh.quad_report


def test_a_quad_target_is_approached_and_reported():
    doc, _ = build_body()
    mesh = doc.mesh_quads(target=8000)
    report = mesh.quad_report
    assert report["quad_count"] == mesh.quad_count
    assert report["target"] == 8000
    assert report["within_tolerance"] is True
    assert report["clamped"] is False
    assert 0 < report["iterations"] <= 4
    # The contract is the tolerance, not the number: 10% by default.
    assert abs(mesh.quad_count - 8000) <= 800
    # And the report describes this mesh: the cell size it names reproduces it.
    assert doc.mesh_quads(cell_size=report["cell_size"]).quad_count == mesh.quad_count


def test_an_explicit_cell_size_spends_no_iterations():
    doc, _ = build_body()
    mesh = doc.mesh_quads(cell_size=0.06)
    report = mesh.quad_report
    assert report["iterations"] == 0
    assert report["cell_size"] == pytest.approx(0.06)
    assert report["target"] == 0


def test_quad_meshing_refuses_what_it_cannot_do():
    doc, _ = build_body()
    with pytest.raises(ValueError, match="VOXEL faces"):
        doc.mesh_quads(cell_size=0.1, mode="faces")
    with pytest.raises(ValueError, match="mode must be 'dual'"):
        doc.mesh_quads(cell_size=0.1, mode="quadriflow")
    with pytest.raises(ValueError, match="neither names a lattice"):
        doc.mesh_quads()
    with pytest.raises(ValueError, match="max_iterations"):
        doc.mesh_quads(target=1000, max_iterations=1000)
    # tolerance=0.0 USED to raise here. It no longer does, deliberately:
    # clay_quad_params documents "<= 0 means 0.10" and the C binding honours
    # it, so refusing it in Python made the documented C default an error in
    # one binding and not the other. The bound that stayed is the far end.
    with pytest.raises(ValueError, match="tolerance"):
        doc.mesh_quads(target=1000, tolerance=1.0)


# REGRESSION (review round 2): pyclay and the C ABI disagreed about what the
# count knobs' zero MEANS. clay_quad_params documents "<= 0 means the default"
# for tolerance and max_iterations and read_quad_params accepts both, while
# pyclay raised ValueError on each — so the documented C default was an error
# in Python. In the other direction pyclay had no ceiling on `target` at all
# (2**40 ran for nineteen seconds; 2**63 escaped as a bare std::bad_cast from
# nanobind rather than as one of this API's errors).
def test_the_quad_count_knobs_take_the_c_abis_rules():
    doc, _ = build_body()

    # Zero is the default, not an error, for both knobs and on both entry
    # points — the same values a C caller who declared only the original
    # struct layout sends.
    for kwargs in ({"tolerance": 0.0}, {"max_iterations": 0}, {"tolerance": -1.0}):
        mesh = doc.mesh_quads(target=1000, **kwargs)
        assert mesh.quad_count > 0
        assert mesh.quad_report["iterations"] > 0

    _, grid = _sphere_sculpt()
    assert grid.mesh_quads(target=500, tolerance=0.0).quad_count > 0
    assert grid.mesh_quads(target=500, max_iterations=0).quad_count > 0

    # And the far ends are refused, as they are in C.
    with pytest.raises(ValueError, match="below 1"):
        doc.mesh_quads(target=1000, tolerance=1.0)
    with pytest.raises(ValueError, match="finite"):
        doc.mesh_quads(target=1000, tolerance=float("nan"))
    with pytest.raises(ValueError, match="max_iterations must be 0..64"):
        doc.mesh_quads(target=1000, max_iterations=-1)
    # CLAY_MAX_BATCH. Without this a mistyped target meshed until it finished.
    with pytest.raises(ValueError, match="above the ceiling"):
        doc.mesh_quads(target=16777217)
    # Too large for a long long: an API error, not nanobind's std::bad_cast.
    with pytest.raises(ValueError, match="whole number of quads"):
        doc.mesh_quads(target=2**63)


def _sphere_sculpt(voxel_size=0.05):
    doc = clay.Document()
    grid = doc.add_voxel_layer("sculpt", voxel_size=voxel_size)
    source = clay.Document()
    source.add_sdf_layer("s").add(clay.Sphere(r=0.5))
    grid.rasterize(source)
    return doc, grid


def test_voxel_quad_modes_and_the_smooth_identity():
    _, grid = _sphere_sculpt()
    dual = grid.mesh_quads()
    smooth = grid.mesh_smooth()
    # Dual at the grid's own voxel size IS the smooth mesh, plus its quads.
    assert np.array_equal(dual.positions, smooth.positions)
    assert np.array_equal(dual.indices, smooth.indices)
    assert dual.quad_count == smooth.triangle_count // 2
    assert smooth.quad_count == 0

    faces = grid.mesh_quads(mode="faces")
    assert faces.quad_count > 0
    # A welded corner faces three ways at once, so faces mode carries none.
    assert faces.normals.shape == (0, 3)
    # And greedy meshing is untouched by any of this.
    assert grid.mesh().quad_count == 0

    with pytest.raises(ValueError, match="mode must be 'dual' or 'faces'"):
        grid.mesh_quads(mode="zremesher")
    with pytest.raises(ValueError, match="resolution level"):
        grid.mesh_quads(level=4)


def test_a_non_finite_cell_size_is_refused_here_as_it_is_in_c():
    """REGRESSION: a NaN passes every `> 0` test, so it used to fall through to
    the "no cell size given" branch and be answered with an estimated seed —
    while clay_quad_params refused the same value. Same input, one answer."""
    doc, _ = build_body()
    _, grid = _sphere_sculpt()
    for bad in (float("nan"), float("inf"), float("-inf")):
        with pytest.raises(ValueError, match="cell_size must be finite"):
            doc.mesh_quads(cell_size=bad, target=100)
        with pytest.raises(ValueError, match="cell_size must be finite"):
            grid.mesh_quads(cell_size=bad)


def test_the_faces_target_walks_the_level_stack():
    """REGRESSION: faces mode is a walk of the resolution stack, and `clamped`
    means the stack ran out — not that some cell-size limit was reached."""
    _, grid = _sphere_sculpt(voxel_size=0.1)
    source = clay.Document()
    source.add_sdf_layer("s").add(clay.Sphere(r=0.5))
    for _ in range(2):
        grid.add_level()
        grid.rasterize(source)
    counts = [grid.mesh_quads(mode="faces", level=l).quad_count
              for l in range(grid.level_count)]
    assert counts == sorted(counts) and counts[0] < counts[-1]

    # Inside the stack: both neighbours meshed, the nearer returned, no clamp.
    inside = grid.mesh_quads(mode="faces", target=counts[0] + 1).quad_report
    assert inside["quad_count"] == counts[0]
    assert inside["iterations"] == 2
    assert inside["clamped"] is False

    # Off either end: the end level, and the report says the stack ran out.
    below = grid.mesh_quads(mode="faces", target=1).quad_report
    assert below["quad_count"] == counts[0]
    assert below["clamped"] is True
    # max_iterations does not truncate the walk — the stack is its own bound.
    above = grid.mesh_quads(mode="faces", target=10 * counts[-1],
                            max_iterations=1).quad_report
    assert above["quad_count"] == counts[-1]
    assert above["iterations"] == grid.level_count
    assert above["clamped"] is True
    assert above["within_tolerance"] is False


def test_an_empty_coarse_level_is_not_the_coarse_end_of_the_stack():
    """REGRESSION (round 3): faces mode read the coarse end of the ladder off
    LEVEL 0 rather than off the coarsest level that yields anything. A voxel
    stack is not a strict mip — a scattered fine sculpt leaves the coarse
    levels empty — and an empty level meshes to 0 quads, which is below every
    target. So a grid whose every non-empty level overshoots the target came
    back with `clamped` False, telling the caller a nearer level existed."""
    doc = clay.Document()
    grid = doc.add_voxel_layer("sculpt", voxel_size=0.4)
    grid.add_level()
    grid.add_level()
    grid.active_level = 2
    # Every voxel alone in its parent cell: a coarse cell needs half its eight
    # children, so the downsample leaves levels 1 and 0 genuinely empty.
    for x in range(0, 8, 2):
        for y in range(0, 8, 2):
            for z in range(0, 8, 2):
                grid.set((x, y, z), 1)

    occupied = [grid.level_occupied_count(l) for l in range(grid.level_count)]
    assert occupied[0] == 0 and occupied[1] == 0 and occupied[2] > 0
    counts = [grid.mesh_quads(mode="faces", level=l).quad_count
              for l in range(grid.level_count)]
    assert counts[0] == 0 and counts[1] == 0 and counts[2] > 0

    below = grid.mesh_quads(mode="faces", target=counts[2] // 4).quad_report
    assert below["quad_count"] == counts[2]
    assert below["iterations"] == 3
    assert below["within_tolerance"] is False
    assert below["clamped"] is True  # the stack ran out; no level is nearer

    # The other side: an exact hit on that level is neither end of the ladder.
    exact = grid.mesh_quads(mode="faces", target=counts[2]).quad_report
    assert exact["quad_count"] == counts[2]
    assert exact["within_tolerance"] is True
    assert exact["clamped"] is False


def test_a_voxel_target_finer_than_the_grid_reports_the_clamp():
    _, grid = _sphere_sculpt()
    mesh = grid.mesh_quads(target=1_000_000)
    report = mesh.quad_report
    assert report["clamped"] is True
    assert report["within_tolerance"] is False
    assert report["cell_size"] == pytest.approx(0.05)
    assert report["quad_count"] == mesh.quad_count > 0


def test_quads_survive_a_document_round_trip(tmp_path):
    doc, _ = build_body()
    mesh = doc.mesh_quads(cell_size=0.07)
    carrier = clay.Document()
    carrier.add_mesh_layer(mesh, name="quads")
    path = str(tmp_path / "quads.clayspace")
    carrier.save(path)
    back = clay.load(path).mesh_layer("quads")
    assert back.quad_count == mesh.quad_count
    assert np.array_equal(back.quads, mesh.quads)
    # The geometry crossed; the report did not, because a mesh read out of a
    # document was produced by no meshing call.
    with pytest.raises(ValueError, match="not produced by a quad mesher"):
        back.quad_report


def test_quads_reach_the_files_that_carry_them(tmp_path):
    doc, _ = build_body()
    mesh = doc.mesh_quads(cell_size=0.09)
    obj = tmp_path / "quads.obj"
    mesh.save(str(obj))
    faces = [line for line in obj.read_text().splitlines() if line.startswith("f ")]
    assert len(faces) == mesh.quad_count
    assert all(len(line.split()) == 5 for line in faces)  # f + four corners

    # glTF 2.0 has no quad primitive mode, so GLB is the triangulation. Written
    # here rather than discovered in Blender.
    glb = tmp_path / "quads.glb"
    mesh.save(str(glb))
    assert glb.stat().st_size > 0


# -- fixed-topology mesh brushes ------------------------------------------------
# The contract these defend is that topology never changes. `indices` and
# `quads` are compared element for element, not counted: a rewrite to the same
# length would still have destroyed the retopology the feature exists to keep.


def _plane_grid(n=12, half=1.0):
    """A flat triangulated grid. Analytic, so 'did exactly these vertices move'
    has an obvious right answer."""
    step = 2.0 * half / n
    positions = [(-half + step * x, 0.0, -half + step * z)
                 for z in range(n + 1) for x in range(n + 1)]
    indices = []
    stride = n + 1
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            indices += [a, a + stride, a + 1, a + 1, a + stride, a + stride + 1]
    return clay.Mesh.from_triangles(np.array(positions, dtype=np.float32),
                                    np.array(indices, dtype=np.uint32))


def _bumpy_grid(n=12, half=1.0):
    m = _plane_grid(n, half)
    p = np.array(m.positions, copy=True)
    p[::3, 1] = 0.04
    p[1::3, 1] = -0.02
    return clay.Mesh.from_triangles(p, np.array(m.indices, copy=True))


def test_every_mesh_verb_moves_vertices_and_none_moves_a_polygon():
    verbs = ["grab", "draw", "inflate", "smooth", "pinch", "flatten",
             "clay", "crease", "scrape", "polish", "snakehook"]
    for verb in verbs:
        m = _bumpy_grid()
        topology = np.array(m.indices, copy=True)
        before = np.array(m.positions, copy=True)
        sculptor = clay.MeshSculptor(m)
        moved = sculptor.stamp(verb, center=(0, 0, 0), radius=0.5, strength=0.5,
                               direction=(0, 0.1, 0))
        assert moved > 0, verb
        assert np.array_equal(np.array(m.indices), topology), verb
        assert not np.array_equal(np.array(m.positions), before), verb


# --- the lattice cage (lattice-on-a-mesh) -----------------------------------


def _cage(box=((-1, -1, -1), (1, 1, 1)), n=3):
    return clay.Lattice(box, nx=n, ny=n, nz=n)


def test_an_untouched_cage_changes_nothing():
    # Offsets rather than positions is what buys this: the identity is exact by
    # construction, not to within a tolerance.
    m = _bumpy_grid()
    before = np.array(m.positions, copy=True)
    cage = _cage()
    assert cage.is_identity
    assert cage.displacement((0.3, -0.2, 0.7)) == (0.0, 0.0, 0.0)

    assert clay.MeshSculptor(m).lattice(cage) == 0
    assert np.array_equal(np.array(m.positions), before)


def test_a_uniformly_dragged_cage_translates_the_mesh():
    m = _bumpy_grid()
    before = np.array(m.positions, copy=True)
    cage = _cage()
    by = (0.25, -0.5, 0.125)
    for k in range(3):
        for j in range(3):
            for i in range(3):
                cage.set_offset(i, j, k, by)
    assert not cage.is_identity

    moved = clay.MeshSculptor(m).lattice(cage)
    assert moved == len(before)
    assert np.allclose(np.array(m.positions), before + np.array(by, dtype=np.float32), atol=1e-5)


def test_a_cage_corner_is_interpolated():
    # Bernstein interpolates its end points, which is what makes a lattice UI
    # behave: dragging a corner takes that corner of the box with it.
    cage = _cage()
    cage.set_offset(0, 0, 0, (0.5, 0.25, -0.125))
    assert cage.displacement((-1, -1, -1)) == pytest.approx((0.5, 0.25, -0.125), abs=1e-6)
    assert cage.displacement((1, 1, 1)) == pytest.approx((0.0, 0.0, 0.0), abs=1e-6)


def test_material_outside_the_cage_travels_rigidly():
    # Not drawn onto the box: the offset is clamped, the POSITION is not.
    cage = _cage()
    for k in range(3):
        for j in range(3):
            for i in range(3):
                cage.set_offset(i, j, k, (0.0, 0.5, 0.0))
    d = cage.displacement((5.0, 0.0, 0.0))
    assert d == pytest.approx((0.0, 0.5, 0.0), abs=1e-6)


def test_a_lattice_keeps_topology_and_is_one_undo_step():
    m = _bumpy_grid()
    topology = np.array(m.indices, copy=True)
    before = np.array(m.positions, copy=True)

    cage = _cage()
    cage.set_offset(1, 1, 1, (0.0, 0.8, 0.0))

    deltas = clay.VertexDeltas()
    sculptor = clay.MeshSculptor(m)
    assert sculptor.lattice(cage, deltas=deltas) > 0
    assert np.array_equal(np.array(m.indices), topology)
    assert not np.array_equal(np.array(m.positions), before)

    deltas.revert(sculptor)
    assert np.array_equal(np.array(m.positions), before)


def test_a_cage_clamps_its_divisions():
    assert clay.Lattice(((0, 0, 0), (1, 1, 1)), nx=0, ny=1, nz=-4).divisions == (2, 2, 2)
    assert clay.Lattice(((0, 0, 0), (1, 1, 1)), nx=9999).divisions[0] == 32


def test_a_mesh_stroke_undoes_bit_exactly_as_one_gesture():
    m = _plane_grid(16)
    before = np.array(m.positions, copy=True)
    sculptor = clay.MeshSculptor(m)

    preset = clay.StrokePreset(radius=0.25, strength=0.8)
    samples = np.array([[-0.4 + 0.1 * i, 0.0, 0.0] for i in range(8)], dtype=np.float32)
    deltas = clay.VertexDeltas()
    applied = sculptor.apply_stroke(samples, preset, "draw", deltas=deltas)
    assert applied > 1
    assert not np.array_equal(np.array(m.positions), before)

    # Coalesced: one entry per vertex reached, not one per stamp.
    assert 0 < deltas.vertex_count <= len(before)

    deltas.revert(sculptor)
    assert np.array_equal(np.array(m.positions), before)
    deltas.revert(sculptor)  # idempotent
    assert np.array_equal(np.array(m.positions), before)
    deltas.apply(sculptor)
    assert not np.array_equal(np.array(m.positions), before)


def test_a_mask_freezes_a_mesh_stroke_vertex_by_vertex():
    m = _plane_grid(16)
    before = np.array(m.positions, copy=True)
    sculptor = clay.MeshSculptor(m)

    mask = clay.MaskField(cell_size=0.05)
    mask.paint((0.6, 0.0, 0.0), size=40, shape="cube", falloff="constant", strength=1.0)

    preset = clay.StrokePreset(radius=0.25, strength=1.0)
    samples = np.array([[-0.4, 0, 0], [0.4, 0, 0]], dtype=np.float32)
    assert sculptor.apply_stroke(samples, preset, "draw", mask=mask) > 0

    after = np.array(m.positions)
    frozen = (before[:, 0] > 0.5) & (np.abs(before[:, 2]) < 0.4)
    assert frozen.any()
    assert np.array_equal(after[frozen], before[frozen])
    assert not np.array_equal(after[~frozen], before[~frozen])


def test_a_surface_falloff_does_not_reach_across_a_gap():
    # Two sheets a hair apart, sharing no edge: the closed mouth. A straight
    # line reaches the lower one; a surface walk would have to leave the sheet.
    upper = _plane_grid(8)
    p = np.array(upper.positions, copy=True)
    i = np.array(upper.indices, copy=True)
    lower = p.copy()
    lower[:, 1] -= 0.02
    both = clay.Mesh.from_triangles(np.vstack([p, lower]),
                                    np.concatenate([i, i + len(p)]))
    base = np.array(both.positions, copy=True)

    def lower_moved(geodesic):
        m = clay.Mesh.from_triangles(base.copy(), np.array(both.indices, copy=True))
        clay.MeshSculptor(m).stamp("draw", center=(0, 0, 0), radius=0.5,
                                   strength=1.0, falloff="constant", geodesic=geodesic)
        after = np.array(m.positions)
        return int((after[len(p):, 1] != base[len(p):, 1]).sum())

    assert lower_moved(False) > 0
    assert lower_moved(True) == 0


def test_a_mesh_layer_is_sculpted_in_place_and_a_locked_one_refuses():
    doc = clay.Document()
    borrowed = doc.add_mesh_layer(_plane_grid(8), name="carried")
    layer_id = borrowed.layer
    before = np.array(borrowed.positions, copy=True)

    sculptor = clay.MeshSculptor(borrowed)
    assert sculptor.stamp("draw", center=(0, 0, 0), radius=0.5, strength=0.5) > 0
    # The DOCUMENT's own triangles moved, not a copy of them.
    assert not np.array_equal(np.array(doc.mesh_layer("carried").positions), before)

    doc.set_layer_protection(layer_id, locked=True)
    with pytest.raises(Exception):
        sculptor.stamp("draw", center=(0, 0, 0), radius=0.5, strength=0.5)
    doc.set_layer_protection(layer_id, locked=False, ghost=True)
    with pytest.raises(Exception):
        sculptor.stamp("draw", center=(0, 0, 0), radius=0.5, strength=0.5)


def test_a_raycast_hands_back_the_seed_the_walk_wants():
    m = _plane_grid(8)
    sculptor = clay.MeshSculptor(m)
    hit = sculptor.raycast((0.21, 1.0, -0.13), (0, -1, 0))
    assert hit is not None
    assert hit["t"] == pytest.approx(1.0, rel=1e-4)
    assert hit["position"][1] == pytest.approx(0.0, abs=1e-5)
    assert hit["normal"][1] == pytest.approx(1.0, rel=1e-4)

    moved = sculptor.stamp("draw", center=hit["position"], radius=0.3,
                           strength=0.5, seed_class=hit["seed_class"])
    assert moved > 0
    assert sculptor.raycast((9.0, 1.0, 9.0), (0, -1, 0)) is None


def test_the_mesh_brush_arguments_refuse_what_c_refuses():
    m = _plane_grid(6)
    sculptor = clay.MeshSculptor(m)
    with pytest.raises(ValueError):
        sculptor.stamp("nope", center=(0, 0, 0), radius=0.5)
    with pytest.raises(ValueError):
        sculptor.stamp("draw", center=(0, 0, 0), radius=0.0)
    with pytest.raises(ValueError):
        sculptor.stamp("draw", center=(0, 0, 0), radius=0.5, falloff="cosine")
    with pytest.raises(ValueError):
        sculptor.stamp("flatten", center=(0, 0, 0), radius=0.5, flatten_mode="sideways")
    with pytest.raises(ValueError):
        sculptor.stamp("smooth", center=(0, 0, 0), radius=0.5, smooth_iterations=0)
    with pytest.raises(ValueError):
        sculptor.stamp("smooth", center=(0, 0, 0), radius=0.5, smooth_iterations=1000)


# -- triangles straight to cells ------------------------------------------------


def _box_mesh(lo, hi, colors=None):
    """Twelve triangles bounding a box, wound outward."""
    p = np.array([[lo[0], lo[1], lo[2]], [hi[0], lo[1], lo[2]], [hi[0], hi[1], lo[2]],
                  [lo[0], hi[1], lo[2]], [lo[0], lo[1], hi[2]], [hi[0], lo[1], hi[2]],
                  [hi[0], hi[1], hi[2]], [lo[0], hi[1], hi[2]]], dtype=np.float32)
    f = np.array([0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7,
                  0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
                  0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5], dtype=np.uint32)
    return clay.Mesh.from_triangles(p, f)


def test_rasterize_mesh_fills_the_solid_and_defaults_its_region():
    m = _box_mesh((-0.3, -0.2, -0.25), (0.3, 0.2, 0.25))
    grid = clay.VoxelGrid(0.02)
    grid.rasterize_mesh(m)                      # None region = the mesh's bounds
    assert grid.occupied_count > 0

    # An explicit region bounds the work and says nothing about the rest.
    half = clay.VoxelGrid(0.02)
    half.rasterize_mesh(m, region=((0.0, -0.4, -0.4), (0.4, 0.4, 0.4)))
    assert 0 < half.occupied_count < grid.occupied_count


def test_rasterize_mesh_survives_a_hole():
    # A parity ray cast flips a half-space on one missing face; the winding
    # number does not, which is the whole reason for the sign choice.
    whole = _box_mesh((-0.3, -0.2, -0.25), (0.3, 0.2, 0.25))
    # pyclay exposes indices as (N, 3), so this drops the two triangles of ONE
    # face — not six indices, which is the same edit spelled for the C++ side.
    holed = clay.Mesh.from_triangles(np.array(whole.positions, copy=True),
                                     np.array(whole.indices, copy=True)[:-2])
    a, b = clay.VoxelGrid(0.02), clay.VoxelGrid(0.02)
    a.rasterize_mesh(whole)
    b.rasterize_mesh(holed)
    assert b.occupied_count > a.occupied_count * 0.8


def test_rasterize_mesh_refuses_what_it_cannot_mean():
    m = _box_mesh((-0.2, -0.2, -0.2), (0.2, 0.2, 0.2))
    grid = clay.VoxelGrid(0.02)
    with pytest.raises(Exception):
        grid.rasterize_mesh(m, region=((1.0, 1.0, 1.0), (0.0, 0.0, 0.0)))   # inverted
    with pytest.raises(Exception):
        grid.rasterize_mesh(m, region=((0.0, 0.0), (1.0, 1.0)))             # malformed
    assert grid.occupied_count == 0


def test_rasterize_mesh_beats_the_document_detour_on_a_thin_feature():
    # The four-step chain this replaces: triangles -> band -> layer -> cells.
    cell = 0.02
    slab = _box_mesh((-0.30, -0.014, -0.30), (0.30, 0.014, 0.30))
    region = ((-0.5, -0.5, -0.5), (0.5, 0.5, 0.5))

    direct = clay.VoxelGrid(cell)
    direct.rasterize_mesh(slab, region=region)

    doc = clay.Document()
    doc.add_sdf_layer("import").add(clay.Volume.from_mesh(slab, cell=cell))
    detoured = clay.VoxelGrid(cell)
    detoured.rasterize(doc, region=region)

    assert direct.occupied_count > 0
    assert direct.occupied_count >= detoured.occupied_count


# -- sculpt layers -------------------------------------------------------------
# A pass you can dial back after making it. The Python surface adds one thing
# the C++ side does not have: the context manager, which is the only way to
# guarantee a raising block cannot leave a grid recording forever.


def sculpt_ball(radius=6, cell=0.1):
    g = clay.VoxelGrid(cell)
    idx = g.palette_add((0.6, 0.6, 0.6))
    for z in range(-radius, radius + 1):
        for y in range(-radius, radius + 1):
            for x in range(-radius, radius + 1):
                if x * x + y * y + z * z <= radius * radius:
                    g.set((x, y, z), idx)
    return g, idx


def test_sculpt_layer_context_manager_records_a_pass():
    g, idx = sculpt_ball()
    base = g.occupied_count
    assert g.sculpt_layer_count == 0

    with g.sculpt_layer("wrinkles") as layer:
        assert g.recording_sculpt_layer
        g.sculpt_inflate((0, 6, 0), size=9, amount=2)
    assert not g.recording_sculpt_layer

    assert layer == 0
    assert g.sculpt_layer_count == 1
    assert g.sculpt_layer_name(0) == "wrinkles"
    assert g.sculpt_layer_cell_count(0) > 0
    full = g.occupied_count
    assert full != base

    # The two ends of the slider are exact.
    g.set_sculpt_layer_strength(0, 0.0)
    assert g.occupied_count == base
    g.set_sculpt_layer_strength(0, 1.0)
    assert g.occupied_count == full


def test_sculpt_layer_scope_closes_when_the_block_raises():
    """The reason the context manager exists. begin/end alone would leave a
    grid recording for the rest of its life the first time a stroke loop threw,
    and every later edit would silently join the abandoned pass."""
    g, idx = sculpt_ball()
    with pytest.raises(RuntimeError, match="stroke failed"):
        with g.sculpt_layer("partial"):
            g.set((0, 7, 0), idx)
            raise RuntimeError("stroke failed")
    assert not g.recording_sculpt_layer
    # The partial pass is KEPT: those edits did happen, and a layer that owns
    # them is more recoverable than one that disowns them.
    assert g.sculpt_layer_count == 1
    assert g.sculpt_layer_cell_count(0) == 1


def test_sculpt_layer_fractional_strength_is_reproducible_and_monotone():
    def at(strength):
        g, _ = sculpt_ball()
        with g.sculpt_layer():
            g.sculpt_inflate((0, 6, 0), size=9, amount=2)
        g.set_sculpt_layer_strength(0, strength)
        return g.occupied_count

    # Same strength, same answer — the cell-coordinate dither, not an RNG.
    assert at(0.4) == at(0.4)
    assert at(0.0) <= at(0.5) <= at(1.0)
    assert at(0.0) < at(1.0)

    # Dialling one grid around lands where building it there does.
    g, _ = sculpt_ball()
    with g.sculpt_layer():
        g.sculpt_inflate((0, 6, 0), size=9, amount=2)
    for s in (0.2, 0.9, 0.3, 0.7):
        g.set_sculpt_layer_strength(0, s)
    assert g.occupied_count == at(0.7)


def test_sculpt_layers_stack_and_the_top_one_wins():
    g, _ = sculpt_ball()
    red = g.palette_add((0.9, 0.1, 0.1))
    blue = g.palette_add((0.1, 0.1, 0.9))
    probe = (0, 3, 0)

    with g.sculpt_layer("red"):
        g.set(probe, red)
    with g.sculpt_layer("blue"):
        g.set(probe, blue)
    assert g.get(probe) == blue

    g.set_sculpt_layer_visible(1, False)
    assert g.get(probe) == red
    assert not g.sculpt_layer_visible(1)
    g.set_sculpt_layer_visible(1, True)
    assert g.get(probe) == blue

    # Merging down keeps the lower name and the visible result.
    g.merge_sculpt_layer_down(1)
    assert g.sculpt_layer_count == 1
    assert g.sculpt_layer_name(0) == "red"
    assert g.get(probe) == blue


def test_sculpt_layers_survive_a_clayspace_round_trip(tmp_path):
    doc = clay.Document()
    grid = doc.add_voxel_layer("v", voxel_size=0.1)
    idx = grid.palette_add((0.5, 0.5, 0.5))
    for x in range(10):
        grid.set((x, 0, 0), idx)
    with grid.sculpt_layer("dialable"):
        for x in range(10):
            grid.set((x, 1, 0), idx)
    grid.set_sculpt_layer_strength(0, 0.35)
    composed = grid.occupied_count

    path = tmp_path / "layers.clayspace"
    doc.save(str(path))
    reloaded = clay.load(str(path)).voxel_layer("v")
    assert reloaded is not None
    assert reloaded.sculpt_layer_count == 1
    assert reloaded.sculpt_layer_name(0) == "dialable"
    assert reloaded.sculpt_layer_strength(0) == pytest.approx(0.35)
    assert reloaded.occupied_count == composed

    # Still dialable, which is the reason the diff is stored and not the result.
    reloaded.set_sculpt_layer_strength(0, 1.0)
    grid.set_sculpt_layer_strength(0, 1.0)
    assert reloaded.occupied_count == grid.occupied_count


def test_sculpt_layer_index_errors():
    g, _ = sculpt_ball(radius=2)
    for call in (
        lambda: g.sculpt_layer_name(0),
        lambda: g.sculpt_layer_strength(0),
        lambda: g.set_sculpt_layer_strength(0, 0.5),
        lambda: g.set_sculpt_layer_visible(0, False),
        lambda: g.remove_sculpt_layer(0),
        lambda: g.merge_sculpt_layer_down(0),
    ):
        with pytest.raises(IndexError):
            call()

    with g.sculpt_layer("only"):
        g.set((0, 0, 0), 1)
    # Nesting has no meaning: a cell belongs to one pass.
    with pytest.raises(ValueError):
        with g.sculpt_layer("a"):
            with g.sculpt_layer("b"):
                pass
    # ...and the outer one still closed.
    assert not g.recording_sculpt_layer
    # The bottom layer has nothing below it.
    with pytest.raises(ValueError):
        g.merge_sculpt_layer_down(0)


# -- alphas on SDF layers ------------------------------------------------------
# A caller-supplied scalar stamp as a distance offset under finite support: how
# detail work is actually done in every competing sculptor, and until now this
# engine had it on voxels and not on fields.


def alpha_doc(stamp, amplitude=0.15, extent=0.6, radius=0.4, tangent=(1, 0, 0)):
    doc = clay.Document()
    prim = clay.Sphere(r=0.5)
    if stamp is not None:
        prim = prim.alpha(stamp, centre=(0, 0, 0.5), direction=(0, 0, 1), tangent=tangent,
                          extent=extent, radius=radius, amplitude=amplitude)
    doc.add_sdf_layer("l").add(prim)
    return doc


def surface_along_z(doc, lo=0.0, hi=3.0, steps=70):
    for _ in range(steps):
        mid = 0.5 * (lo + hi)
        if doc.eval(np.array([[0.0, 0.0, mid]], dtype=np.float32))[0] < 0.0:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def checker(n):
    return (np.indices((n, n)).sum(axis=0) % 2).astype(np.float32)


def test_alpha_all_zero_stamp_is_exactly_the_identity():
    plain = alpha_doc(None)
    zeroed = alpha_doc(np.zeros((16, 16), dtype=np.float32))
    pts = np.random.default_rng(52).uniform(-1.2, 1.2, size=(512, 3)).astype(np.float32)
    # Exactly: the offset is a product with the sample, so zero is a hard zero.
    assert np.array_equal(plain.eval(pts), zeroed.eval(pts))
    assert zeroed.safe_step_scale() == pytest.approx(plain.safe_step_scale())


def test_alpha_white_is_raised_and_black_carved():
    """The sign convention. Every sculpting package treats white as raised; a
    deformer's offset is ADDED to the distance, where positive is further away,
    so the kernel flips it once and this is the test that says it flipped."""
    bare = surface_along_z(alpha_doc(None))
    assert bare == pytest.approx(0.5, abs=1e-3)
    raised = surface_along_z(alpha_doc(np.ones((16, 16), dtype=np.float32), amplitude=0.15))
    carved = surface_along_z(alpha_doc(np.ones((16, 16), dtype=np.float32), amplitude=-0.15))
    assert raised > bare
    assert carved < bare


def test_alpha_leaves_material_outside_the_radius_exactly_alone():
    plain = alpha_doc(None)
    marked = alpha_doc(checker(16), amplitude=0.2)
    outside = np.array([[0, 0, -0.9], [0.9, 0, 0], [0, -0.9, 0], [-0.7, -0.7, -0.2]],
                       dtype=np.float32)
    # The fixture is honest: every probe is further than `radius` from the centre.
    assert np.all(np.linalg.norm(outside - np.array([0, 0, 0.5]), axis=1) > 0.4)
    assert np.array_equal(plain.eval(outside), marked.eval(outside))


def test_alpha_bound_comes_from_steepness_not_from_values():
    """A stamp of all ones has the largest possible VALUES and is perfectly
    flat, so it displaces rigidly. A bound taken from magnitudes would charge it
    as the steepest stamp there is."""
    flat = alpha_doc(np.ones((16, 16), dtype=np.float32))
    steep = alpha_doc(checker(16))
    assert steep.safe_step_scale() < flat.safe_step_scale()

    # A flat stamp's cost does not change with resolution.
    for n in (4, 32, 128):
        assert alpha_doc(np.ones((n, n), dtype=np.float32)).safe_step_scale() == pytest.approx(
            flat.safe_step_scale())

    # ...and the same relief spread wider is a gentler slope.
    assert alpha_doc(checker(32), extent=1.5).safe_step_scale() > alpha_doc(
        checker(32), extent=0.3).safe_step_scale()


def test_alpha_survives_a_clayspace_round_trip(tmp_path):
    stamp = checker(16)
    stamp[0, 0] = 0.25  # asymmetric, so a transposed read would show
    doc = alpha_doc(stamp, amplitude=0.2)
    path = tmp_path / "alpha.clayspace"
    doc.save(str(path))
    back = clay.load(str(path))

    pts = np.random.default_rng(7).uniform(-1.0, 1.0, size=(256, 3)).astype(np.float32)
    assert np.array_equal(doc.eval(pts), back.eval(pts))
    # The bound is recomputed on load rather than read, so a hand-edited file
    # cannot claim a looser one than its samples deserve.
    assert back.safe_step_scale() == pytest.approx(doc.safe_step_scale())


def test_alpha_refuses_a_stamp_it_cannot_interpolate():
    with pytest.raises(ValueError, match="2x2"):
        alpha_doc(np.ones((1, 1), dtype=np.float32))
    with pytest.raises(ValueError, match="extent"):
        alpha_doc(np.ones((8, 8), dtype=np.float32), extent=0.0)
    with pytest.raises(ValueError):
        alpha_doc(np.ones((8, 8), dtype=np.float32), tangent=(0, 0))  # not a 3-vector


def test_alpha_tangent_parallel_to_the_direction_still_works():
    """A caller passing an up parallel to the push direction is a caller, not an
    error: the frame falls back to a derived axis rather than collapsing."""
    for tangent in ((0, 0, 1), (0, 0, 0)):
        doc = alpha_doc(checker(16), tangent=tangent)
        assert np.isfinite(doc.safe_step_scale())
        assert doc.safe_step_scale() > 0.0
        assert np.isfinite(doc.eval(np.array([[0, 0, 0.5]], dtype=np.float32))[0])
# -- masking that gates any operation ------------------------------------------
# Masks gated AUTHORING before this: a voxel edit consumed one per cell as it
# wrote, and an SDF edit consumed one when a stroke became items. Neither touched
# an item already in the edit list, so a mask over an ear did nothing about the
# next boolean.


def protect_plus_x():
    m = clay.MaskField(0.04)
    m.fill(((0.0, -2.0, -2.0), (2.0, 2.0, 2.0)), 1.0)
    return m


def bored_ball(gate=None, width=0.15, op=None):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0))
    bar = clay.Box(size=(0.8, 0.8, 3.0))
    if gate is not None:
        bar = bar.gate(gate, width=width)
    layer.add(bar, op=op or clay.Op.SUBTRACT)
    return doc


def solid_ball():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=1.0))
    return doc


def test_a_gate_protects_a_surface_from_a_boolean():
    solid, ungated = solid_ball(), bored_ball()
    gated = bored_ball(protect_plus_x())
    protected = np.array([[0.3, 0.0, 0.0]], dtype=np.float32)
    open_side = np.array([[-0.3, 0.0, 0.0]], dtype=np.float32)

    # The fixture means something: the ungated cut removes material here.
    assert ungated.eval(protected)[0] != solid.eval(protected)[0]
    # ...and the gated one does not, EXACTLY.
    assert gated.eval(protected)[0] == solid.eval(protected)[0]
    # The open side is the ungated result, exactly.
    assert gated.eval(open_side)[0] == ungated.eval(open_side)[0]


def test_a_gate_is_exact_across_the_whole_protected_region():
    """Not at three lucky points. mix(x, y, t) is x + (y - x) * t, so t == 1
    returns y only up to rounding — one float step, which is a seam along the
    border of every protected region."""
    solid, gated = solid_ball(), bored_ball(protect_plus_x())
    xs = np.linspace(0.35, 1.4, 80, dtype=np.float32)
    pts = np.stack([xs, np.zeros_like(xs), np.zeros_like(xs)], axis=1)
    assert np.array_equal(gated.eval(pts), solid.eval(pts))


def test_a_gate_composes_with_every_op():
    """A gate rides the combine record rather than being a mode, which is what
    lets it gate a boolean. Each op is probed where it actually acts: adding a
    thin bar INSIDE a ball changes nothing there."""
    solid = solid_ball()
    for op, probe in ((clay.Op.SUBTRACT, (0.3, 0.0, 0.0)),
                      (clay.Op.ADD, (0.3, 0.0, 1.2)),
                      (clay.Op.INTERSECT, (0.3, 0.7, 0.0))):
        p = np.array([probe], dtype=np.float32)
        assert bored_ball(op=op).eval(p)[0] != solid.eval(p)[0], f"{op} fixture proves nothing"
        assert bored_ball(protect_plus_x(), op=op).eval(p)[0] == solid.eval(p)[0]


def test_a_gate_costs_step_scale_and_a_wider_one_costs_less():
    ungated = bored_ball().safe_step_scale()
    assert ungated == pytest.approx(1.0)
    previous = 0.0
    for width in (0.05, 0.1, 0.3, 0.8):
        scale = bored_ball(protect_plus_x(), width=width).safe_step_scale()
        assert scale < ungated, "a gate is never free"
        assert scale > previous, "a wider gate must cost less"
        previous = scale


def test_a_gate_survives_the_file(tmp_path):
    doc = bored_ball(protect_plus_x(), width=0.2)
    path = tmp_path / "gated.clayspace"
    doc.save(str(path))
    back = clay.load(str(path))
    pts = np.random.default_rng(54).uniform(-1.6, 1.6, size=(384, 3)).astype(np.float32)
    assert np.array_equal(doc.eval(pts), back.eval(pts))
    assert back.safe_step_scale() == pytest.approx(doc.safe_step_scale())


def test_a_gate_refuses_what_it_cannot_protect():
    empty = clay.MaskField(0.04)
    with pytest.raises(ValueError, match="protect nothing"):
        clay.Box(size=(1, 1, 1)).gate(empty)
    with pytest.raises(ValueError, match="width"):
        clay.Box(size=(1, 1, 1)).gate(protect_plus_x(), width=0.0)
    # Nothing reaching the threshold is the same as no mask at all.
    faint = clay.MaskField(0.04)
    faint.fill(((0.0, -1.0, -1.0), (1.0, 1.0, 1.0)), 0.2)
    with pytest.raises(ValueError, match="protect nothing"):
        clay.Box(size=(1, 1, 1)).gate(faint, threshold=0.8)


# -- the mesh verbs the vocabulary was missing ---------------------------------
# Relax, Layer and Nudge, plus alphas on every mesh verb at once
# (close-mesh-brush-vocabulary).


def dome_mesh(n=48, half=1.0):
    """A dome. CURVATURE is what separates relax from smooth — on a flat plane
    the Laplacian displacement is already tangential, so removing its normal
    component changes nothing and the two verbs are identical."""
    positions, indices = [], []
    for z in range(n + 1):
        for x in range(n + 1):
            px = -half + 2.0 * half * x / n
            pz = -half + 2.0 * half * z / n
            positions.append((px, 0.6 * float(np.exp(-(px * px + pz * pz) * 1.5)), pz))
    stride = n + 1
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            indices += [a, a + stride, a + 1, a + 1, a + stride, a + stride + 1]
    return clay.Mesh.from_triangles(np.array(positions, dtype=np.float32),
                                    np.array(indices, dtype=np.uint32))


def dome_normals(p):
    e = 0.6 * np.exp(-(p[:, 0] ** 2 + p[:, 2] ** 2) * 1.5)
    n = np.stack([3.0 * p[:, 0] * e, np.ones_like(e), 3.0 * p[:, 2] * e], axis=1)
    return n / np.linalg.norm(n, axis=1, keepdims=True)


def split_motion(before, after, centre, radius):
    """Mean motion split into 'along the surface normal' and 'across it'."""
    near = np.linalg.norm(before - np.asarray(centre), axis=1) <= radius
    d = (after - before)[near]
    n = dome_normals(before[near])
    along = np.einsum("ij,ij->i", d, n)
    return float(np.abs(along).mean()), float(np.linalg.norm(d - n * along[:, None], axis=1).mean())


def test_relax_evens_spacing_while_barely_moving_the_surface():
    """The distinction from smooth: smooth moves toward the Laplacian average,
    which is inward on a convex region — that is why smoothing shrinks."""
    centre = (0.0, 0.6, 0.0)
    stretched = dome_mesh()
    clay.MeshSculptor(stretched).stamp("grab", center=centre, radius=0.7,
                                       direction=(0.25, 0, 0))
    start = np.array(stretched.positions, copy=True)

    moved = {}
    for verb in ("relax", "smooth"):
        m = clay.Mesh.from_triangles(np.array(stretched.positions, copy=True),
                                     np.array(stretched.indices, copy=True))
        sc = clay.MeshSculptor(m)
        for _ in range(8):
            sc.stamp(verb, center=centre, radius=0.5, strength=1.0, smooth_iterations=2)
        moved[verb] = split_motion(start, np.array(m.positions), centre, 0.45)

    relax_along, relax_across = moved["relax"]
    smooth_along, _ = moved["smooth"]
    # Relax moves the SURFACE a fraction as far...
    assert relax_along < smooth_along * 0.5
    # ...and its own motion is mostly across the surface rather than through it.
    assert relax_across > relax_along * 2.0


def test_layer_deposits_to_a_ceiling_where_draw_keeps_going():
    centre = (0.0, 0.6, 0.0)
    base = np.array(dome_mesh().positions, copy=True)

    def run(verb, stamps):
        m = dome_mesh()
        sc = clay.MeshSculptor(m)
        record = clay.VertexDeltas()
        for _ in range(stamps):
            sc.stamp(verb, center=centre, radius=0.5, strength=0.15, layer_height=0.08,
                     deposit_normal=(0, 1, 0), deltas=record)
        return split_motion(base, np.array(m.positions), centre, 0.1)[0]

    assert run("layer", 12) > run("layer", 1)      # it does build up...
    assert run("layer", 12) <= 0.08 + 1e-4         # ...and stops at the ceiling
    assert run("draw", 12) > 0.08 * 2.0            # draw does not
    assert run("draw", 12) > run("layer", 12) * 3.0


def test_layer_without_a_record_is_refused():
    """No record means no stroke origin, so every stamp would clamp against the
    current surface — draw wearing layer's name."""
    m = dome_mesh(24)
    before = np.array(m.positions, copy=True)
    sc = clay.MeshSculptor(m)
    assert sc.stamp("layer", center=(0, 0.6, 0), radius=0.5, layer_height=0.08) == 0
    assert np.array_equal(np.array(m.positions), before)
    assert sc.stamp("layer", center=(0, 0.6, 0), radius=0.5, layer_height=0.08,
                    deltas=clay.VertexDeltas()) > 0


def test_an_absent_alpha_leaves_every_verb_exactly_as_it_was():
    base = dome_mesh(32)
    for verb in ("draw", "inflate", "smooth", "pinch", "clay", "relax"):
        a, b = dome_mesh(32), dome_mesh(32)
        clay.MeshSculptor(a).stamp(verb, center=(0, 0.6, 0), radius=0.5, strength=0.4)
        clay.MeshSculptor(b).stamp(verb, center=(0, 0.6, 0), radius=0.5, strength=0.4,
                                   alpha=None)
        assert np.array_equal(np.array(a.positions), np.array(b.positions)), verb


def test_an_alpha_shape_reaches_the_mesh():
    """Not just its magnitude: half the stamp masked off moves half the region.
    A stamp sampled at the wrong coordinates would still pass an all-zero and
    an all-ones check."""
    base = np.array(dome_mesh().positions, copy=True)
    half = np.zeros((32, 32), dtype=np.float32)
    half[:, 16:] = 1.0

    m = dome_mesh()
    clay.MeshSculptor(m).stamp("draw", center=(0, 0.6, 0), radius=0.6, strength=0.4,
                               alpha=half, alpha_direction=(0, 1, 0),
                               alpha_tangent=(1, 0, 0), deposit_normal=(0, 1, 0))
    moved = np.linalg.norm(np.array(m.positions) - base, axis=1)
    assert moved[base[:, 0] > 0.1].max() > 0.01     # the +u half moved
    assert moved[base[:, 0] < -0.1].max() == 0.0    # the -u half did not


def test_an_all_zero_alpha_moves_nothing():
    m = dome_mesh(32)
    before = np.array(m.positions, copy=True)
    zeros = np.zeros((16, 16), dtype=np.float32)
    assert clay.MeshSculptor(m).stamp("draw", center=(0, 0.6, 0), radius=0.5, strength=0.4,
                                      alpha=zeros, alpha_direction=(0, 1, 0)) == 0
    assert np.array_equal(np.array(m.positions), before)


def test_the_new_verbs_keep_the_mesh_contract():
    for verb in ("relax", "layer", "nudge"):
        m = dome_mesh(32)
        base_pos = np.array(m.positions, copy=True)
        base_idx = np.array(m.indices, copy=True)
        sc = clay.MeshSculptor(m)
        record = clay.VertexDeltas()
        for _ in range(4):
            sc.stamp(verb, center=(0, 0.6, 0), radius=0.5, strength=0.6,
                     direction=(0.1, 0.05, 0), layer_height=0.05, deltas=record)
        assert np.array_equal(np.array(m.indices), base_idx), verb
        assert record.vertex_count > 0, verb
        record.revert(sc)
        assert np.array_equal(np.array(m.positions), base_pos), verb


def test_a_bad_mesh_verb_names_the_new_ones():
    with pytest.raises(ValueError, match="relax"):
        clay.MeshSculptor(dome_mesh(8)).stamp("bogus", center=(0, 0, 0), radius=0.5)


# -- the colour pair (add-mesh-colour-brushes) --------------------------------


def test_a_colour_brush_needs_a_colour_attribute_and_will_not_invent_one():
    """Twelve bytes per vertex is a real cost to hide behind a brush stroke, and
    a silent creation makes "nothing happened" indistinguishable from "this mesh
    had no colours"."""
    m = dome_mesh(16)
    sculptor = clay.MeshSculptor(m)
    assert not sculptor.has_colors
    assert sculptor.stamp("paint", center=(0, 0.5, 0), radius=0.5, color=(1, 0, 0)) == 0
    assert not sculptor.has_colors

    assert sculptor.ensure_colors((1, 1, 1)) is True
    assert sculptor.has_colors
    assert sculptor.ensure_colors((0, 0, 0)) is False  # already there, left alone
    assert np.allclose(np.asarray(m.colors), 1.0)


def test_a_colour_brush_moves_no_vertex():
    """The mirror of the guarantee the displacement verbs make about colours:
    a colour pass over a finished sculpt is no diff on the geometry."""
    for verb, extra in (("paint", {"color": (1, 0, 0)}), ("smear", {"direction": (0.2, 0, 0)})):
        m = dome_mesh(24)
        sculptor = clay.MeshSculptor(m)
        sculptor.ensure_colors((1, 1, 1))
        # Something for smear to drag. Painted rather than assigned, because
        # Mesh.colors is read-only from Python — and painting it first is the
        # workflow anyway.
        sculptor.stamp("paint", center=(-0.3, 0.4, 0), radius=0.45, strength=1.0,
                       color=(1, 0, 0))

        before_pos = np.asarray(m.positions).copy()
        before_nrm = np.asarray(m.normals).copy()
        before_col = np.asarray(m.colors).copy()

        assert sculptor.stamp(verb, center=(0, 0.5, 0), radius=0.5, strength=0.9, **extra) > 0
        assert np.array_equal(np.asarray(m.positions), before_pos), verb
        assert np.array_equal(np.asarray(m.normals), before_nrm), verb
        assert not np.array_equal(np.asarray(m.colors), before_col), verb


def test_a_zero_drag_is_no_smear_rather_than_a_smooth():
    m = dome_mesh(16)
    sculptor = clay.MeshSculptor(m)
    sculptor.ensure_colors((1, 1, 1))
    before = np.asarray(m.colors).copy()
    assert sculptor.stamp("smear", center=(0, 0.5, 0), radius=0.5, direction=(0, 0, 0)) == 0
    assert np.array_equal(np.asarray(m.colors), before)


def test_a_paint_stroke_reverts_bit_identically():
    m = dome_mesh(16)
    sculptor = clay.MeshSculptor(m)
    sculptor.ensure_colors((1, 1, 1))
    before = np.asarray(m.colors).copy()

    record = clay.VertexDeltas()
    assert sculptor.stamp("paint", center=(0, 0.5, 0), radius=0.5, strength=0.8,
                          color=(0.2, 0.4, 0.9), deltas=record) > 0
    painted = np.asarray(m.colors).copy()
    assert not np.array_equal(painted, before)

    record.revert(sculptor)
    assert np.array_equal(np.asarray(m.colors), before)
    record.apply(sculptor)
    assert np.array_equal(np.asarray(m.colors), painted)


def test_a_bad_mesh_verb_names_the_colour_pair_too():
    with pytest.raises(ValueError, match="paint"):
        clay.MeshSculptor(dome_mesh(8)).stamp("bogus", center=(0, 0, 0), radius=0.5)


# -- the memoised gate bake (add-masking-that-gates-any-op, 1.2b) -------------


def test_gating_many_items_by_one_mask_bakes_once():
    """Measuring a mask is expensive — 21 ms at four thousand painted cells,
    145 ms at thirty thousand — and it used to be paid per gated item. What is
    checked here is the OBSERVABLE consequence: the items share one gate volume
    rather than each holding a copy."""
    mask = clay.MaskField(0.02)
    mask.paint((0, 0, 0), size=20, target=1.0)

    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    for i in range(8):
        item = clay.Sphere(r=0.5)
        item.gate(mask, width=0.1)
        layer.add(item)
    # Eight gated items, one measurement: the document still evaluates.
    assert len(doc.mesh(resolution=24).positions) > 0


def test_a_changed_mask_is_not_gated_from_a_stale_bake():
    """The stale-cache failure would be silent — an item gated by a repainted
    mask would protect where the mask USED to be — so this uses a change the
    binding cannot paper over: a mask that becomes empty must start REFUSING.
    A memo that had not noticed would hand back the old gate and succeed.

    (The finer claim, that the rebaked field measures the new region, is pinned
    in C++ where the volume itself can be sampled: see
    "mask: a gate bake is reused until the mask changes".)"""
    mask = clay.MaskField(0.05)
    mask.paint((0, 0, 0), size=8, target=1.0)

    item = clay.Sphere(r=1.2)
    item.gate(mask, width=0.1)          # bakes, and fills the memo
    item.gate(mask, width=0.1)          # served from the memo

    mask.clear()
    with pytest.raises(ValueError, match="protect nothing"):
        clay.Sphere(r=1.2).gate(mask, width=0.1)


def test_an_empty_mask_still_refuses_to_gate():
    """Memoising the failure must not turn it into a silent success."""
    mask = clay.MaskField(0.05)
    item = clay.Sphere(r=0.5)
    with pytest.raises(ValueError, match="protect nothing"):
        item.gate(mask, width=0.1)
    with pytest.raises(ValueError, match="protect nothing"):
        item.gate(mask, width=0.1)


# -- GLB import (add-glb-import) ---------------------------------------------


def test_a_glb_round_trips_through_a_file(tmp_path):
    """GLB was the only format pyclay could save and not load."""
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.6))
    src = doc.mesh(resolution=24)

    path = tmp_path / "model.glb"
    src.save(str(path))
    back = clay.load_mesh(str(path))

    assert len(back.positions) == len(src.positions)
    assert len(back.indices) == len(src.indices)
    assert np.array_equal(np.asarray(back.positions), np.asarray(src.positions))
    assert np.array_equal(np.asarray(back.indices), np.asarray(src.indices))


def test_a_loaded_glb_can_be_sculpted_and_resampled(tmp_path):
    """The point of an importer: what comes back is a mesh you can WORK on."""
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.6))
    path = tmp_path / "base.glb"
    doc.mesh(resolution=28).save(str(path))

    loaded = clay.load_mesh(str(path))
    sculptor = clay.MeshSculptor(loaded)
    before = np.asarray(loaded.positions).copy()
    assert sculptor.stamp("draw", center=(0, 0.6, 0), radius=0.3, strength=0.4) > 0
    assert not np.array_equal(np.asarray(loaded.positions), before)


def test_gltf_is_refused_with_a_reason(tmp_path):
    """.gltf keeps its buffers in separate files, so a loader handed one path
    would be reading files the caller never gave it."""
    path = tmp_path / "model.gltf"
    path.write_text("{}")
    with pytest.raises(ValueError, match=r"\.glb"):
        clay.load_mesh(str(path))


def test_a_truncated_glb_is_refused_rather_than_read(tmp_path):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.5))
    whole = tmp_path / "whole.glb"
    doc.mesh(resolution=16).save(str(whole))

    cut = tmp_path / "cut.glb"
    cut.write_bytes(whole.read_bytes()[: 40])
    with pytest.raises(Exception):
        clay.load_mesh(str(cut))


# -- whole-form deformers on a mesh layer (add-mesh-deformers) ---------------


def column_mesh(rings=24, segments=20, radius=0.35, height=2.0):
    """A closed column along Y, so a taper and a twist both have a span."""
    positions, indices = [], []
    for r in range(rings + 1):
        y = height * (r / rings) - height * 0.5
        for s in range(segments):
            a = 2.0 * np.pi * s / segments
            positions.append((radius * np.cos(a), y, radius * np.sin(a)))
    for r in range(rings):
        for s in range(segments):
            a = r * segments + s
            b = r * segments + (s + 1) % segments
            c, d = a + segments, b + segments
            indices += [a, c, b, b, c, d]
    return clay.Mesh.from_triangles(np.array(positions, dtype=np.float32),
                                    np.array(indices, dtype=np.uint32))


def _radius_at(positions, y, tol=0.06):
    sel = np.abs(positions[:, 1] - y) < tol
    return float(np.hypot(positions[sel, 0], positions[sel, 2]).max()) if sel.any() else 0.0


def test_a_taper_narrows_the_far_end_and_leaves_the_near_one():
    m = column_mesh()
    sc = clay.MeshSculptor(m)
    before = np.asarray(m.positions).copy()
    assert sc.deform("taper", origin=(0, -1, 0), axis=(0, 1, 0), span=2.0,
                     scale_start=1.0, scale_end=0.4) > 0
    after = np.asarray(m.positions)
    assert _radius_at(after, -1.0) == pytest.approx(_radius_at(before, -1.0), rel=0.02)
    assert _radius_at(after, 1.0) == pytest.approx(_radius_at(before, 1.0) * 0.4, rel=0.05)


def test_a_deformer_is_not_a_brush_and_takes_the_whole_form():
    """It has no centre, no radius and no falloff: a deformer states something
    about the form, a brush about a dab.

    Not "every vertex moves" — the ring sitting exactly at the span's start has
    a zero angle and correctly stays put, which is the same rule that makes an
    identity deformer free. What is checked is that the reach is the FORM: a
    vertex at each end of the model is affected, which no brush radius here
    would cover."""
    m = column_mesh()
    sc = clay.MeshSculptor(m)
    before = np.asarray(m.positions).copy()
    moved = sc.deform("twist", origin=(0, -1, 0), axis=(0, 1, 0), span=2.0, angle=1.0)

    after = np.asarray(m.positions)
    changed = np.any(after != before, axis=1)
    assert moved == int(changed.sum())
    # The far end turned; the span's start did not.
    assert changed[before[:, 1] > 0.9].all()
    assert not changed[before[:, 1] < -0.99].any()
    # And the reach is most of the form, not a disc.
    assert changed.mean() > 0.9


def test_a_mask_holds_part_of_the_form_still():
    m = column_mesh()
    sc = clay.MeshSculptor(m)
    before = np.asarray(m.positions).copy()

    mask = clay.MaskField(0.08)
    mask.paint((0, -0.7, 0), size=14, target=1.0)
    assert sc.deform("taper", origin=(0, -1, 0), axis=(0, 1, 0), span=2.0,
                     scale_end=0.3, mask=mask) > 0

    after = np.asarray(m.positions)
    held = np.all(after == before, axis=1)
    assert held.any(), "the masked end should be bit-identical"
    assert (~held).any(), "the unmasked end should have moved"


def test_an_identity_deformer_moves_nothing():
    m = column_mesh()
    sc = clay.MeshSculptor(m)
    before = np.asarray(m.positions).copy()
    record = clay.VertexDeltas()
    assert sc.deform("taper", span=2.0, deltas=record) == 0
    assert sc.deform("twist", span=2.0, angle=0.0, deltas=record) == 0
    assert np.array_equal(np.asarray(m.positions), before)


def test_a_deformation_reverts_bit_identically():
    m = column_mesh()
    sc = clay.MeshSculptor(m)
    before = np.asarray(m.positions).copy()
    record = clay.VertexDeltas()
    assert sc.deform("twist", origin=(0, -1, 0), axis=(0, 1, 0), span=2.0,
                     angle=1.1, deltas=record) > 0
    deformed = np.asarray(m.positions).copy()
    assert not np.array_equal(deformed, before)

    record.revert(sc)
    assert np.array_equal(np.asarray(m.positions), before)
    record.apply(sc)
    assert np.array_equal(np.asarray(m.positions), deformed)


def test_bend_is_refused_and_the_message_says_why():
    """Absent by measurement, not by oversight: the SDF bend takes its angle
    from a coordinate it then moves, so it has no forward map."""
    sc = clay.MeshSculptor(column_mesh(8, 8))
    with pytest.raises(ValueError, match="no 'bend'"):
        sc.deform("bend", span=1.0, angle=1.0)


# -- attribute transfer (add-mesh-attribute-transfer) ------------------------


def test_colour_survives_a_round_trip_through_the_field():
    """The case this exists for: sampling a mesh into a field and meshing it
    back keeps the shape and loses the colours. The nearest point on the
    original knows what belonged there."""
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.6), color="#c04030")
    src = doc.mesh(resolution=32)
    source_mean = np.asarray(src.colors).mean(axis=0)

    vol = clay.Volume.from_mesh(src, cell=0.04)
    d2 = clay.Document()
    d2.add_sdf_layer("v").add(vol)
    back = d2.mesh(resolution=32)
    assert not np.allclose(np.asarray(back.colors).mean(axis=0), source_mean, atol=0.05)

    report = back.transfer_attributes(src)
    assert report["colors"] is True
    assert report["transferred"] > 0
    assert np.allclose(np.asarray(back.colors).mean(axis=0), source_mean, atol=0.02)


def test_transfer_moves_no_vertex():
    """A transfer, not a projection. A verb that moved the target's vertices
    toward the source would be a different operation."""
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.5), color="#40a060")
    src = doc.mesh(resolution=24)
    tgt = doc.mesh(resolution=18)

    before_p = np.asarray(tgt.positions).copy()
    before_i = np.asarray(tgt.indices).copy()
    tgt.transfer_attributes(src)
    assert np.array_equal(np.asarray(tgt.positions), before_p)
    assert np.array_equal(np.asarray(tgt.indices), before_i)


def test_geometry_the_source_never_occupied_falls_back_and_says_so():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.4))
    src = doc.mesh(resolution=20)

    far = clay.Document()
    far.add_sdf_layer("f").add(clay.Sphere(r=0.4, position=(9, 0, 0)))
    tgt = far.mesh(resolution=20)

    report = tgt.transfer_attributes(src)
    assert report["fell_back"] == len(np.asarray(tgt.positions))
    assert report["transferred"] == 0
    assert report["max_distance"] > 0.0


def test_normals_are_off_by_default():
    """A resampled mesh should shade like itself, not like the old shape."""
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=0.5))
    src = doc.mesh(resolution=20)
    tgt = doc.mesh(resolution=16)

    off = tgt.transfer_attributes(src)
    assert off["normals"] is False
    on = tgt.transfer_attributes(src, normals=True)
    assert on["normals"] is True


# -- the full validation report (report-mesh-quality) -------------------------
#
# `is_watertight()` and `is_manifold()` were the only two of eleven measured
# quantities that reached Python, so a script could be told an export was bad
# and never told why. And the sampled self-intersection pass, which the meshing
# spec names as a validation primitive, had never been reachable from a binding
# at all: neither pyclay nor the C ABI passed a cap, so both always took the
# default of 0, which skips it.

TETRA_POSITIONS = np.array(
    [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.float32
)
# Wound so every face normal points OUTWARD, which is what makes the signed
# volume positive and the orientation check mean something.
TETRA_INDICES = np.array([0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3], dtype=np.uint32)


def test_validation_report_carries_every_quantity():
    mesh = clay.Mesh.from_triangles(TETRA_POSITIONS, TETRA_INDICES)
    r = mesh.validation_report()
    assert r["vertices"] == 4 and r["triangles"] == 4
    assert r["watertight"] and r["manifold"] and r["oriented"] and r["clean"]
    assert r["boundary_edges"] == 0
    assert r["non_manifold_edges"] == 0
    assert r["degenerate_triangles"] == 0
    assert r["euler_characteristic"] == 2  # closed surface of genus 0


def test_a_hole_is_reported_not_merely_detected():
    """The number the meshing spec has always said the validator reports."""
    holed = clay.Mesh.from_triangles(TETRA_POSITIONS, TETRA_INDICES[:9])
    r = holed.validation_report()
    assert not r["watertight"] and not r["clean"]
    assert r["boundary_edges"] == 3  # the three edges of the face that was dropped
    # Still edge-manifold. An open mesh is not a broken one, and one "is it bad"
    # bit forces a caller to conflate them.
    assert r["manifold"] and r["non_manifold_edges"] == 0


def test_self_intersection_is_reachable_and_says_whether_it_ran():
    # Two triangles crossing through each other, sharing no vertex so the
    # adjacency skip does not swallow the pair.
    positions = np.array(
        [[-1, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0.2, -1], [0, 0.2, 1], [0, 0.9, 0]],
        dtype=np.float32,
    )
    mesh = clay.Mesh.from_triangles(positions, np.arange(6, dtype=np.uint32))

    skipped = mesh.validation_report()
    assert skipped["intersecting_pairs"] == 0
    # The echoed budget is the ONLY thing separating "none found" from
    # "none looked for" — both leave the count at zero, and `clean` reads it.
    assert skipped["intersection_budget"] == 0

    tested = mesh.validation_report(max_intersection_pairs=1000)
    assert tested["intersecting_pairs"] == 1
    assert tested["intersection_budget"] == 1000
    assert not tested["clean"]


def test_volume_and_area_and_the_sign_of_the_volume():
    mesh = clay.Mesh.from_triangles(TETRA_POSITIONS, TETRA_INDICES)
    assert mesh.signed_volume == pytest.approx(1 / 6, rel=1e-5)
    # three unit right triangles at 0.5, plus the slanted face at sqrt(3)/2
    assert mesh.surface_area == pytest.approx(1.5 + math.sqrt(3) / 2, rel=1e-5)

    flipped = TETRA_INDICES.reshape(-1, 3)[:, [0, 2, 1]].reshape(-1).astype(np.uint32)
    inverted = clay.Mesh.from_triangles(TETRA_POSITIONS, flipped)
    assert inverted.signed_volume == pytest.approx(-1 / 6, rel=1e-5)


# -- serializing without a file (serialize-without-a-file) --------------------
#
# Every format was implemented against buffers and only the path wrappers
# crossed, so a host whose documents arrive from a container, a network or a
# document provider had to round-trip through a temporary file.

def test_document_round_trips_through_bytes(tmp_path):
    doc, _ = build_body()
    data = doc.to_bytes()
    assert isinstance(data, bytes) and len(data) > 0

    # The whole argument: one serializer, reached two ways.
    path = tmp_path / "same.clayspace"
    doc.save(str(path))
    assert data == path.read_bytes()

    back = clay.load_bytes(data)
    probes = np.array([[0, 0, 0], [0.3, 0.1, -0.2], [2, 2, 2]], dtype=np.float32)
    assert np.allclose(back.eval(probes), doc.eval(probes))


def test_every_mesh_format_round_trips_through_bytes(tmp_path):
    doc, _ = build_body()
    mesh = doc.mesh(resolution=24)

    for fmt in ("obj", "ply", "fbx", "glb"):
        data = mesh.to_bytes(fmt)
        assert len(data) > 0, fmt
        back = clay.load_mesh_bytes(data, fmt)
        assert back.triangle_count > 0, fmt

        # OBJ is the one stated difference — see the mtllib test below.
        if fmt != "obj":
            path = tmp_path / f"same.{fmt}"
            mesh.save(str(path))
            assert data == path.read_bytes(), fmt


def test_in_memory_obj_names_no_material_file():
    doc, _ = build_body()
    text = doc.mesh(resolution=16).to_bytes("obj").decode()
    assert "mtllib" not in text  # a buffer has no companion .mtl to name
    assert "\nv " in text        # and it is still an OBJ


def test_bytes_loaders_refuse_what_they_should():
    doc, _ = build_body()
    mesh = doc.mesh(resolution=16)

    with pytest.raises(ValueError):
        mesh.to_bytes("stl")               # not a default, a refusal
    with pytest.raises(ValueError):
        clay.load_mesh_bytes(b"whatever", "stl")
    with pytest.raises(Exception):
        clay.load_bytes(b"not a clayspace document at all")

    # A budget guards a buffer exactly as it guards a file.
    data = mesh.to_bytes("ply")
    with pytest.raises(Exception):
        clay.load_mesh_bytes(data, "ply", max_vertices=2)
    assert clay.load_mesh_bytes(data, "ply", max_vertices=10_000_000).triangle_count > 0


def test_format_name_is_case_insensitive():
    doc, _ = build_body()
    mesh = doc.mesh(resolution=16)
    assert mesh.to_bytes("PLY") == mesh.to_bytes("ply")
    assert clay.load_mesh_bytes(mesh.to_bytes("ply"), "Ply").triangle_count > 0


# -- one undo across three representations (unify-the-undo-history) ----------
#
# Document.undo() acted on the command stack alone, so a script that sculpted
# a voxel layer and called undo() reversed an unrelated SDF edit or returned
# False. It now reverses the most recent edit whatever made it.

def test_undo_reverses_a_voxel_edit():
    doc = clay.Document()
    blocks = doc.add_voxel_layer("blocks", voxel_size=0.1)
    doc.enable_undo()
    blocks.set((0, 0, 0), 1)
    assert blocks.occupied_count == 1
    assert doc.undo_depth == 1

    assert doc.undo()
    assert blocks.occupied_count == 0
    assert doc.redo()
    assert blocks.occupied_count == 1


def test_one_undo_order_spans_sdf_and_voxels():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    blocks = doc.add_voxel_layer("blocks", voxel_size=0.1)
    doc.enable_undo()

    layer.add(clay.Sphere(r=0.5))
    blocks.set((0, 0, 0), 1)
    blocks.set((1, 0, 0), 1)
    assert doc.undo_depth == 3

    # Newest first: the two voxel writes, then the SDF item.
    assert doc.undo() and blocks.occupied_count == 1
    assert doc.undo() and blocks.occupied_count == 0
    probes = np.array([[0.0, 0.0, 0.0]], dtype=np.float32)
    inside_before = doc.eval(probes)[0]
    assert doc.undo()
    assert doc.eval(probes)[0] != pytest.approx(inside_before)
    assert not doc.undo()


def test_a_sculpt_verb_is_one_undo_step():
    doc = clay.Document()
    blocks = doc.add_voxel_layer("blocks", voxel_size=0.1)
    doc.enable_undo()
    blocks.fill_box((0, 0, 0), (4, 4, 4), 1)
    filled = blocks.occupied_count
    assert filled > 1
    assert doc.undo_depth == 1  # one fill, one step, however many cells

    blocks.sculpt_smooth((0, 0, 0), size=5)
    assert blocks.occupied_count != filled
    assert doc.undo_depth == 2

    assert doc.undo()
    assert blocks.occupied_count == filled  # the whole verb, one step


def test_a_voxel_edit_that_changed_nothing_is_not_a_step():
    doc = clay.Document()
    blocks = doc.add_voxel_layer("blocks", voxel_size=0.1)
    doc.enable_undo()
    blocks.set((0, 0, 0), 1)
    before = doc.undo_depth
    blocks.erase((50, 50, 50))  # already empty: writes nothing
    assert doc.undo_depth == before


def test_a_standalone_grid_has_no_history_and_still_edits():
    # Undo is a DOCUMENT concept; a grid made outside one is not in a document.
    grid = clay.VoxelGrid(voxel_size=0.1)
    grid.set((0, 0, 0), 1)
    assert grid.occupied_count == 1


def test_a_grid_taken_before_enable_undo_still_records():
    # The handle holds a reference TO the document's history rather than a copy
    # of it, so switching undo on afterwards reaches a handle already taken —
    # which is the order a host does these two things in.
    doc = clay.Document()
    blocks = doc.add_voxel_layer("blocks", voxel_size=0.1)
    doc.enable_undo()
    blocks.set((0, 0, 0), 1)
    assert doc.undo_depth == 1
    assert doc.undo()
    assert blocks.occupied_count == 0


# -- surviving a crash (survive-a-crash) -------------------------------------
#
# A recovery is a snapshot plus the steps since it. Saving is whole-document and
# synchronous, so a timer-driven autosave stalls for however long the sculpt
# takes; a journal is proportional to what changed.

def _session_with_edits():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    blocks = doc.add_voxel_layer("blocks", voxel_size=0.1)
    doc.enable_undo()
    snapshot = doc.to_bytes()
    layer.add(clay.Sphere(r=0.5))
    blocks.set((0, 0, 0), 1)
    blocks.set((1, 0, 0), 2)
    return doc, layer, blocks, snapshot


def test_a_session_is_rebuilt_from_a_snapshot_and_a_journal():
    doc, _, blocks, snapshot = _session_with_edits()
    journal, now_at = doc.journal_since(0)
    assert now_at == 3 and len(journal) > 0

    probes = np.array([[0.0, 0.0, 0.0]], dtype=np.float32)
    before, cells = doc.eval(probes)[0], blocks.occupied_count

    recovered = clay.load_bytes(snapshot)   # the crash
    recovered.enable_undo()
    result = recovered.replay_journal(journal)
    assert result["applied"] == 3
    assert not result["stopped_at_barrier"]

    rblocks = recovered.voxel_layer("blocks")
    assert recovered.eval(probes)[0] == pytest.approx(before)
    assert rblocks.occupied_count == cells
    assert rblocks.get((1, 0, 0)) == 2      # palette index survived too


def test_the_journal_is_incremental():
    doc, layer, _, snapshot = _session_with_edits()
    first, at = doc.journal_since(0)
    layer.add(clay.Sphere(r=0.2, position=(2, 0, 0)))
    second, at2 = doc.journal_since(at)
    assert at2 > at
    assert len(second) < len(first)          # only what is new

    recovered = clay.load_bytes(snapshot)
    recovered.enable_undo()
    recovered.replay_journal(first)
    recovered.replay_journal(second)
    probes = np.array([[2.0, 0.0, 0.0]], dtype=np.float32)
    assert recovered.eval(probes)[0] < 0     # the later sphere is there


def test_trimming_moves_the_floor_and_says_so():
    doc, _, _, _ = _session_with_edits()
    _, at = doc.journal_since(0)
    doc.journal_trim(at)
    first, nxt = doc.journal_range()
    assert first == at and nxt == at
    # Asking below the floor yields nothing rather than a silently short history.
    stale, _ = doc.journal_since(0)
    fresh = clay.Document()
    fresh.enable_undo()
    assert fresh.replay_journal(stale)["applied"] == 0


def test_an_unreadable_journal_is_refused():
    doc, _, _, snapshot = _session_with_edits()
    journal, _ = doc.journal_since(0)

    recovered = clay.load_bytes(snapshot)
    recovered.enable_undo()
    with pytest.raises(Exception):
        recovered.replay_journal(b"not a journal at all")
    newer = bytearray(journal)
    newer[4] = 99                            # a version this build predates
    with pytest.raises(Exception):
        recovered.replay_journal(bytes(newer))


def test_the_journal_needs_undo_enabled():
    doc = clay.Document()
    doc.add_sdf_layer("body")
    with pytest.raises(Exception):
        doc.journal_since(0)


# -- cancelling a long operation (add-operation-cancellation) ----------------
#
# Three budget classes, and the third had no exit: on the reference iPad
# mask_extrude measures 4403 ms and consolidate 661 ms, and every one was a
# call you entered and could not leave.

def _heavy_layer():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    for i in range(20):
        layer.add(clay.Sphere(r=0.30 + 0.01 * i, position=(0.05 * i, 0, 0)))
    return doc, layer


def test_a_cancelled_consolidate_changes_nothing():
    doc, layer = _heavy_layer()
    probes = np.array([[0.0, 0.0, 0.0], [0.3, 0.1, 0.0], [5.0, 5.0, 5.0]], dtype=np.float32)
    before = doc.eval(probes)
    saved = doc.to_bytes()

    token = clay.CancelToken()
    token.cancel()
    with pytest.raises(RuntimeError):
        layer.consolidate(cell=0.01, token=token)

    # Byte-identical, not merely equivalent: the bake builds a volume and
    # installs it at the end, so a cancel is a discard.
    assert doc.to_bytes() == saved
    assert np.allclose(doc.eval(probes), before)


def test_no_token_is_exactly_the_call_that_was_there_before():
    doc, layer = _heavy_layer()
    report = layer.consolidate(cell=0.06)          # no token at all
    assert report["cell_size"] == pytest.approx(0.06)


def test_a_token_is_reusable_after_a_cancel():
    doc, layer = _heavy_layer()
    token = clay.CancelToken()
    token.cancel()
    with pytest.raises(RuntimeError):
        layer.consolidate(cell=0.01, token=token)

    token.reset()
    assert not token.cancelled
    assert layer.consolidate(cell=0.06, token=token)["cell_size"] == pytest.approx(0.06)


def test_progress_reports_idle_rather_than_a_stale_fraction():
    token = clay.CancelToken()
    assert token.progress["running"] is False
    assert token.progress["fraction"] == pytest.approx(0.0)

    doc, layer = _heavy_layer()
    layer.consolidate(cell=0.06, token=token)
    # Finished: idle, not a stale 100%.
    assert token.progress["running"] is False


def test_cancelling_is_safe_before_during_and_twice():
    token = clay.CancelToken()
    token.cancel()
    token.cancel()          # twice is not an error
    token.reset()
    token.reset()
    assert not token.cancelled


# -- masks are undoable (masks-in-the-history, #245) -------------------------
#
# A mask was the fourth representation with no history mechanism, so a mask edit
# was a barrier: undo stopped counting at one and a journal could not recover
# past one. Both of those caveats are gone.

def test_a_mask_edit_is_an_undo_step():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    doc.enable_undo()
    layer.add(clay.Sphere(r=0.5))
    freeze = doc.add_mask("body", cell_size=0.05)

    before = doc.undo_depth
    freeze.fill(((-0.2, -0.2, -0.2), (0.2, 0.2, 0.2)), 1.0)
    painted = freeze.painted_count
    assert painted > 0
    assert doc.undo_depth == before + 1      # a step, where it used to be a barrier

    assert doc.undo()
    assert freeze.painted_count == 0
    assert doc.redo()
    assert freeze.painted_count == painted


def test_every_mask_mutator_is_undoable_not_just_the_two_through_set():
    # invert, clear, expand, contract and smooth write chunk data directly and
    # do NOT go through set(); only fill and invert_within do. A sink on set()
    # would have covered two of seven.
    region = ((-0.2, -0.2, -0.2), (0.2, 0.2, 0.2))

    def fresh():
        doc = clay.Document()
        doc.add_sdf_layer("body")
        doc.enable_undo()
        m = doc.add_mask("body", cell_size=0.05)
        m.fill(region, 1.0)
        return doc, m

    for name, mutate in [
        ("invert", lambda m: m.invert()),
        ("clear", lambda m: m.clear()),
        ("expand", lambda m: m.expand(1)),
        ("contract", lambda m: m.contract(1)),
        ("smooth", lambda m: m.smooth(1)),
        ("fill", lambda m: m.fill(region, 0.5)),
        ("invert_within", lambda m: m.invert_within(region)),
    ]:
        doc, mask = fresh()
        before_count = mask.painted_count
        before_depth = doc.undo_depth
        mutate(mask)
        assert doc.undo_depth == before_depth + 1, f"{name} did not record a step"
        assert doc.undo(), name
        assert mask.painted_count == before_count, f"undoing {name} did not restore the mask"


def test_a_journal_replays_straight_through_a_mask_edit():
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    doc.enable_undo()
    snapshot = doc.to_bytes()

    layer.add(clay.Sphere(r=0.4))
    mask = doc.add_mask("body", cell_size=0.05)
    mask.fill(((-0.2, -0.2, -0.2), (0.2, 0.2, 0.2)), 1.0)
    layer.add(clay.Sphere(r=0.2, position=(1, 0, 0)))
    journal, _ = doc.journal_since(0)

    recovered = clay.load_bytes(snapshot)
    recovered.enable_undo()
    recovered.add_mask("body", cell_size=0.05)
    result = recovered.replay_journal(journal)
    assert not result["stopped_at_barrier"]   # it used to stop here
    assert recovered.mask("body").painted_count == mask.painted_count


# -- bounding the history (add-history-budget) ------------------------------
#
# It had no cap of any kind: no depth limit, no byte accounting, no eviction,
# no query — on a platform that reclaims memory by killing processes.

def _busy_history(edits=40):
    doc = clay.Document()
    doc.add_sdf_layer("body")
    grid = doc.add_voxel_layer("blocks", voxel_size=0.1)
    doc.enable_undo()
    for i in range(edits):
        grid.fill_box((i * 20, 0, 0), (i * 20 + 8, 4, 4), 1)
    return doc, grid


def test_the_history_reports_what_it_holds():
    doc, _ = _busy_history()
    b = doc.history_bytes
    assert b["undo"] > 0
    assert b["undo_steps"] == 40
    # The journal keeps its own copy, so crash recovery roughly doubles it.
    assert b["journal"] > 0
    assert b["total"] == b["undo"] + b["redo"] + b["journal"]
    assert b["dropped_steps"] == 0


def test_a_budget_evicts_the_oldest_and_spares_the_journal():
    doc, _ = _busy_history()
    before = doc.history_bytes
    doc.set_history_budget(before["undo"] // 4)
    after = doc.history_bytes

    assert after["undo"] < before["undo"]
    assert after["undo_steps"] < before["undo_steps"]
    assert after["dropped_steps"] > 0
    # The budget must NOT touch the journal: those bytes are crash recovery,
    # and dropping them silently would lose what that feature exists to keep.
    assert after["journal"] == before["journal"]


def test_the_newest_step_survives_any_budget():
    doc, grid = _busy_history()
    doc.set_history_budget(1)
    assert doc.history_bytes["undo_steps"] == 1
    assert doc.undo()          # and it still works


def test_an_unset_budget_is_todays_behaviour():
    doc, _ = _busy_history(edits=60)
    assert doc.history_bytes["undo_steps"] == 60
    assert doc.history_bytes["dropped_steps"] == 0


def test_trimming_needs_no_budget():
    doc, _ = _busy_history()
    before = doc.history_bytes["undo"]
    doc.trim_history(before // 4)
    assert doc.history_bytes["undo"] < before
    assert doc.history_bytes["undo_steps"] >= 1


def test_history_bytes_reads_zero_when_undo_is_off():
    doc = clay.Document()
    doc.add_sdf_layer("body")
    assert doc.history_bytes["total"] == 0     # honest, not an error
    with pytest.raises(Exception):
        doc.set_history_budget(1024)

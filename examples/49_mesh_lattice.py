"""A lattice cage over a mesh layer — and why this one needs no inverse.

ZBrush's Gizmo 3D ships four deformers worth parity: Twist, Taper, Bend Curve
and **Lattice**. The first three are here as `twist_range`, `taper` and
`bend_curve`, all of them SDF deformers. The lattice is not, and the reason is
the interesting part.

**A claycore SDF deformer is an inverse point map.** Evaluating an implicit
field means asking "what material is at this point", so every deformer answers
"where did the material at p come from" — `grab` returns `p - displacement * w`,
`bend_curve` projects p back onto its guide. That works because those maps have
closed forms *backwards*.

**Forward FFD does not.** Given a cage, moving a point through it is one
tensor-product evaluation; recovering which point *landed* somewhere is a root
find. So a lattice on an SDF item needs a compromise — iterate per sample (which
breaks the single-source kernel dialect), bake through a volume (which loses
re-editability), or author the cage *inverse* (which warps a bit less than
nominal, exactly as `grab` does). That is still open.

**A mesh has no such problem**, and neither do ZBrush or Blender. Both deform
explicit vertices, so FFD runs *forward*: find each vertex's parameters in the
cage, evaluate, move it. Nothing inverts, nothing iterates, nothing is
approximated. Blender's Lattice modifier is exactly this; ZBrush's Gizmo Lattice
acts on the PolyMesh3D's points.

ClayCore has mesh layers with fixed topology, so the real thing is available
here — with the same contract every mesh verb holds: **topology never changes**.

Two design choices this script demonstrates rather than asserts:

**The cage stores OFFSETS, not positions.** So an untouched cage is *exactly*
the identity, and material outside the box travels rigidly with the nearest part
of the cage instead of being dragged onto its surface. Storing positions and
clamping would collapse everything outside onto the box — the one real trap in
this formulation.

**Evaluation is trivariate Bernstein**, one formula at every cage size: degree
is one less than the control-point count, so 2×2×2 is exactly trilinear. It also
*interpolates the corners*, which is what makes a lattice UI feel right — drag a
corner handle and that corner of the box goes with it, all the way.
"""

import numpy as np

import pyclay as clay

import _render as R


def source_model(resolution=88):
    """A blobby form, meshed once. Every cage below gets its own copy.

    The FIN matters. The rest of this shape is a body of revolution about Y, and
    a twist about Y is invisible on one — the render would show a pinch and the
    caption would say "twist", which is a picture that lies. The fin is the
    asymmetry that makes a rotation something you can see."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("source")
    layer.add(clay.Sphere(r=0.55))
    layer.add(clay.Capsule(a=(0, 0.3, 0), b=(0, 0.9, 0), r=0.20),
              blend=clay.Smooth(0.14))
    layer.add(clay.Torus(R=0.50, r=0.09, position=(0, -0.32, 0)),
              blend=clay.Smooth(0.06))
    layer.add(clay.Box(size=(0.62, 0.22, 0.10), position=(0.30, 0.42, 0)),
              blend=clay.Smooth(0.05))
    return doc.mesh(resolution=resolution)


def copy_of(mesh):
    return clay.Mesh.from_triangles(np.array(mesh.positions, copy=True),
                                    np.array(mesh.indices, copy=True))


def preview(mesh, cell=0.013, colour="#7a8fb0"):
    """A DISPLAY-ONLY document. A mesh layer is never evaluated, so the
    renderer — which raycasts a field — has nothing to trace against.
    Resampling here is exactly the approximation a mesh layer exists to avoid:
    fine for a picture, wrong for the export."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell),
                                     color=colour)
    return doc


def cage_over(mesh, n=3, pad=0.02):
    """A cage spanning the mesh, with a little padding so no axis is flat."""
    p = np.array(mesh.positions)
    lo, hi = p.min(axis=0) - pad, p.max(axis=0) + pad
    return clay.Lattice((tuple(lo), tuple(hi)), nx=n, ny=n, nz=n)


def deformed(mesh, shape_cage):
    """Copy the mesh, build a cage, let the caller shape it, apply. Returns the
    mesh and how many vertices moved."""
    m = copy_of(mesh)
    cage = cage_over(m)
    shape_cage(cage)
    moved = clay.MeshSculptor(m).lattice(cage)
    return m, moved


def main():
    R.banner("49 mesh lattice — forward FFD, no inverse anywhere")

    model = source_model()
    base_positions = np.array(model.positions, copy=True)
    base_indices = np.array(model.indices, copy=True)
    print(f"  model: {model.triangle_count} triangles, {len(base_positions)} vertices")

    # --- the identity is exact, not approximate ------------------------------
    untouched = copy_of(model)
    cage = cage_over(untouched)
    moved = clay.MeshSculptor(untouched).lattice(cage)
    same = np.array_equal(np.array(untouched.positions), base_positions)
    print(f"\n  an untouched cage: {moved} vertices moved, positions identical={same}")
    if moved != 0 or not same:
        raise SystemExit("an untouched cage must be the identity exactly")

    # --- a uniform drag is a translation, exactly ----------------------------
    # Bernstein is a partition of unity, so every point picks up the same
    # offset. Not "approximately a translation" — a translation.
    by = np.array([0.2, -0.1, 0.05], dtype=np.float32)

    def drag_all(c):
        for k in range(c.divisions[2]):
            for j in range(c.divisions[1]):
                for i in range(c.divisions[0]):
                    c.set_offset(i, j, k, tuple(by))

    translated, moved = deformed(model, drag_all)
    err = float(np.abs(np.array(translated.positions) - (base_positions + by)).max())
    print(f"  every control point dragged by {tuple(by)}: {moved} vertices moved, "
          f"worst error {err:.2e}")
    if err > 1e-5:
        raise SystemExit("a uniform cage drag must translate the mesh exactly")

    # --- corners are interpolated -------------------------------------------
    probe = cage_over(model)
    probe.set_offset(0, 0, 0, (0.3, 0.0, 0.0))
    at_corner = probe.displacement(probe.rest(0, 0, 0))
    at_far = probe.displacement(probe.rest(2, 2, 2))
    print(f"\n  dragging one corner by 0.30: that corner moves {at_corner[0]:.3f}, "
          f"the opposite one {at_far[0]:.3f}")
    if abs(at_corner[0] - 0.3) > 1e-5 or abs(at_far[0]) > 1e-5:
        raise SystemExit("Bernstein must interpolate the corner control points")

    # --- outside the box travels, it is not collapsed ------------------------
    # The trap this formulation avoids: a POSITION cage clamped to its box
    # would pull every outside point onto the box's surface.
    far_away = (9.0, 0.0, 0.0)
    d = probe.displacement(far_away)
    landed = np.array(far_away) + np.array(d)
    print(f"  a point at x=9 is displaced to x={landed[0]:.3f} — carried along, "
          f"not dragged onto the cage")
    if landed[0] < 8.0:
        raise SystemExit("material outside the box was collapsed onto it")

    # --- the shapes a cage makes ---------------------------------------------
    def taper_top(c):
        n = c.divisions[0]
        for k in range(n):
            for i in range(n):
                c.set_offset(i, n - 1, k,
                             (-0.30 * (i - 1), 0.0, -0.30 * (k - 1)))

    def twist_top(c):
        """A quarter turn of the top layer about Y.

        The OFFSET of a rotation is (R - I)p, not Rp — the cage stores how far a
        control point moved, not where it ended up."""
        n = c.divisions[0]
        angle = np.pi / 2.0
        cos_a, sin_a = float(np.cos(angle)), float(np.sin(angle))
        for k in range(n):
            for i in range(n):
                x, z = float(i - 1), float(k - 1)
                c.set_offset(i, n - 1, k,
                             (x * (cos_a - 1.0) - z * sin_a, 0.0,
                              x * sin_a + z * (cos_a - 1.0)))

    def bulge_middle(c):
        n = c.divisions[0]
        for k in range(n):
            for i in range(n):
                x, z = i - 1, k - 1
                if x or z:
                    c.set_offset(i, 1, k, (0.28 * x, 0.0, 0.28 * z))

    def shear(c):
        n = c.divisions[0]
        for k in range(n):
            for i in range(n):
                c.set_offset(i, n - 1, k, (0.45, 0.0, 0.0))

    cases = [("untouched", lambda c: None), ("taper", taper_top),
             ("twist", twist_top), ("bulge", bulge_middle), ("shear", shear)]

    tiles = []
    print("\n  what a 3x3x3 cage can say:")
    for name, shape in cases:
        m, moved = deformed(model, shape)
        # The contract, checked per case rather than once.
        if not np.array_equal(np.array(m.indices), base_indices):
            raise SystemExit(f"{name} changed the topology")
        drift = float(np.abs(np.array(m.positions) - base_positions).max())
        print(f"    {name:<10} {moved:>6} vertices moved, furthest {drift:.3f}")
        tiles.append(R.render_array(preview(m), width=250, height=250))

    R.contact_sheet(tiles, "49_mesh_lattice.png", columns=5,
                    caption=", ".join(n for n, _ in cases))

    # --- one undo step -------------------------------------------------------
    m = copy_of(model)
    cage = cage_over(m)
    twist_top(cage)
    deltas = clay.VertexDeltas()
    sculptor = clay.MeshSculptor(m)
    sculptor.lattice(cage, deltas=deltas)
    print(f"\n  the whole cage is ONE undo step: {deltas.vertex_count} vertices recorded")
    deltas.revert(sculptor)
    if not np.array_equal(np.array(m.positions), base_positions):
        raise SystemExit("reverting a lattice must restore the mesh bit-exactly")
    print("  reverted bit-exactly")

    # --- 2x2x2 is exactly trilinear -----------------------------------------
    # Degree is one less than the control-point count, so there is no separate
    # linear path to keep in step — the same formula covers both.
    box = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
    lin = clay.Lattice(box, nx=2, ny=2, nz=2)
    corners = {}
    rng = np.random.default_rng(49)
    for c in range(8):
        o = tuple(rng.uniform(-0.4, 0.4, size=3))
        corners[c] = np.array(o)
        lin.set_offset(c & 1, (c >> 1) & 1, (c >> 2) & 1, o)

    worst = 0.0
    for p in rng.uniform(-1.0, 1.0, size=(64, 3)):
        s, t, u = (p + 1.0) * 0.5
        want = np.zeros(3)
        for c in range(8):
            wx = s if (c & 1) else 1 - s
            wy = t if ((c >> 1) & 1) else 1 - t
            wz = u if ((c >> 2) & 1) else 1 - u
            want += corners[c] * (wx * wy * wz)
        worst = max(worst, float(np.abs(np.array(lin.displacement(tuple(p))) - want).max()))
    print(f"\n  a 2x2x2 cage against a hand-written trilinear blend: worst error {worst:.2e}")
    if worst > 1e-5:
        raise SystemExit("degree-1 Bernstein must be trilinear")

    # Exported from a COARSER mesh: the committed models have a size budget, and
    # a lattice is resolution-independent, so nothing about the demonstration
    # depends on the export being the same mesh the pictures used.
    R.save_model(deformed(source_model(resolution=44), twist_top)[0],
                 "49_mesh_lattice.ply")


if __name__ == "__main__":
    main()

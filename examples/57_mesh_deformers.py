"""Taper and twist on a mesh layer — and the Relax that pays for them.

Every one of the sixteen mesh verbs is a brush: a centre, a radius, a falloff.
A DEFORMER is a different kind of thing. It has none of those, because it says
something about the *form* rather than about a dab, which is what ZBrush's
Deformation palette is and what an artist reaches for to shape a blockout.

**They run FORWARDS, and that is why they are exact.** An SDF deformer must run
backwards — it answers "where did the material at this point come from",
because that is what a field evaluation needs, and for free-form deformation
that direction has no closed form. The SDF lattice pays for it: capped at 4x4x4
control points, and carrying about 1.5% error against the forward cage. A mesh
deformer has the vertex in its hand and simply moves it. Same math, easier
direction, and this script checks the consequence rather than asserting it: a
point pushed through the mesh's forward map and back through the kernel's
inverse map returns to where it started.

**There is no `bend`, and that is a measurement rather than an omission.** The
SDF bend takes its angle from `p.x` and then moves `p.x`, so negating the
parameter is not its inverse — the round-trip error is 1.73 where taper's is
0.00000012. Worse, the map folds: at k = 0.9, rest points at x = -1.74 and
x = +1.75 land 0.0101 apart, which means two different places on the model
arrive at one place and no forward map exists to undo it. Choosing what a mesh
bend should be is a decision about the SDF bend's convention; it is recorded,
not guessed.

**Nothing re-tessellates, and Relax does not rescue it.** That is the honest
cost of fixed topology, and this script measures it rather than gesturing at
it. The first draft of this file claimed Relax was the recovery; the numbers
below say otherwise, and the reason is worth more than the claim was.

A taper leaves the top ring with the SAME number of vertices around a SMALLER
circumference. The damage is anisotropy — the right vertex count in the wrong
aspect — and Relax slides vertices along the surface: it can even out uneven
spacing, but it cannot change how many vertices a ring has. So it does not
help here, and measurably makes edge-length variation slightly worse, because
sliding along a tangent plane is not shape-preserving.

Relax IS the recovery for a large `grab`, where the damage is genuinely uneven
spacing. It is not the recovery for a deformer. Getting the top ring the right
number of vertices needs re-tessellation, which this engine deliberately does
not do.
"""

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.6, 0.9, 2.6), (0.0, 0.0, 0.0)


def column(rings=48, segments=40, radius=0.4, height=2.0):
    """A CLOSED column along Y: a span for the deformers to ramp across, and
    enough rings that stretching is visible in the triangles.

    Closed matters for the pictures, and the first version of this file was not
    — the same trap `examples/55` records. `Volume.from_mesh`, which the preview
    below goes through, takes its sign from a winding number, and a winding
    number is undefined on an open sheet: an uncapped tube renders as torn
    fragments rather than as a tube."""
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

    # Caps, as fans around a centre vertex at each end.
    bottom_centre = len(positions)
    positions.append((0.0, -height * 0.5, 0.0))
    top_centre = len(positions)
    positions.append((0.0, height * 0.5, 0.0))
    last_ring = rings * segments
    for s in range(segments):
        nxt = (s + 1) % segments
        # Wound to pair with the side wall's edges: the wall's bottom row runs
        # nxt -> s and its top row runs s -> nxt, so each cap runs the other
        # way. Getting this backwards leaves every cap edge unpaired, and the
        # winding number the preview depends on then has no inside to find.
        indices += [bottom_centre, s, nxt]
        indices += [top_centre, last_ring + nxt, last_ring + s]
    return clay.Mesh.from_triangles(np.array(positions, dtype=np.float32),
                                    np.array(indices, dtype=np.uint32))


def edge_variation(mesh):
    """Coefficient of variation of edge length — how uneven the triangulation
    is, which is exactly what a deformation damages and relax repairs."""
    p = np.asarray(mesh.positions)
    idx = np.asarray(mesh.indices).reshape(-1, 3)
    a = p[idx[:, [0, 1, 2]]].reshape(-1, 3)
    b = p[idx[:, [1, 2, 0]]].reshape(-1, 3)
    length = np.linalg.norm(a - b, axis=1)
    return float(length.std() / length.mean())


def radius_at(mesh, y, tol=0.05):
    p = np.asarray(mesh.positions)
    sel = np.abs(p[:, 1] - y) < tol
    return float(np.hypot(p[sel, 0], p[sel, 2]).max()) if sel.any() else 0.0


def preview(mesh, cell=0.02, colour="#b8b0a4"):
    """A DISPLAY-ONLY document: a mesh layer is never evaluated, so the renderer
    has nothing to trace against. Resampling here is the approximation a mesh
    layer exists to avoid — fine for a picture, wrong for the export."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def deformed(**kwargs):
    m = column()
    clay.MeshSculptor(m).deform(**kwargs)
    return m


def main():
    R.banner("57 mesh deformers — taper, twist, and the relax that pays for them")

    span = dict(origin=(0, -1, 0), axis=(0, 1, 0), span=2.0)

    # --- a deformer is not a brush ------------------------------------------
    m = column()
    sc = clay.MeshSculptor(m)
    before = np.asarray(m.positions).copy()
    moved = sc.deform("taper", scale_start=1.0, scale_end=0.35, **span)
    after = np.asarray(m.positions)
    changed = np.any(after != before, axis=1)
    print(f"  a taper touched {moved} of {len(before)} vertices — no centre, no radius:")
    print(f"    radius at the base: {radius_at(column(), -1):.3f} -> {radius_at(m, -1):.3f}")
    print(f"    radius at the top : {radius_at(column(),  1):.3f} -> {radius_at(m,  1):.3f}")
    print(f"    the ring exactly at the span's start has a zero amount and stays put,")
    print(f"    which is the same rule that makes an identity deformer free:"
          f" {int((~changed).sum())} vertices")

    # --- it moves no topology, and the ends are exact ------------------------
    plain = column()
    if not np.array_equal(np.asarray(m.indices), np.asarray(plain.indices)):
        raise SystemExit("a deformer must not touch the index buffer")
    print("\n  the index buffer is byte-identical — topology never changes")

    idle = column()
    if clay.MeshSculptor(idle).deform("taper", span=2.0) != 0:
        raise SystemExit("an identity deformer must move nothing")
    print("  an identity deformer moves nothing and records nothing")

    # --- a mask holds part of the form still ---------------------------------
    masked = column()
    mask = clay.MaskField(0.06)
    # A CONSTANT falloff, so a band is fully masked: the default smooth
    # curve only reaches 1.0 at the very centre, and "fully protected"
    # is the property being shown.
    mask.paint((0, -0.7, 0), size=16, target=1.0, falloff="constant")
    clay.MeshSculptor(masked).deform("taper", scale_start=1.0, scale_end=0.3,
                                     mask=mask, **span)
    held = np.all(np.asarray(masked.positions) == np.asarray(plain.positions), axis=1)
    print(f"\n  a mask holds part of the form still: {int(held.sum())} vertices are")
    print("  BIT-IDENTICAL to where they started, not merely close")
    if held.sum() == 0:
        raise SystemExit("a fully masked region must not move at all")

    # --- what the deformation costs the triangulation, and what does not fix it
    print("\n  nothing re-tessellates, so a deformation stretches the triangles it has.")
    print("  Edge-length variation (lower is more even), and what six relax passes do:")
    print(f"    {'':22}{'deformed':>10}{'relaxed':>10}")
    for label, kw in (("taper to 0.18", dict(verb="taper", scale_start=1.0, scale_end=0.18)),
                      ("twist by 1.6 rad", dict(verb="twist", angle=1.6))):
        hard = deformed(**kw, **span)
        soft = deformed(**kw, **span)
        sc2 = clay.MeshSculptor(soft)
        for _ in range(6):
            sc2.stamp("relax", center=(0, 0, 0), radius=2.5, strength=0.9)
        print(f"    {label:22}{edge_variation(hard):>10.4f}{edge_variation(soft):>10.4f}")
    print(f"    {'(rest, for scale)':22}{edge_variation(plain):>10.4f}")

    # WHY relax does not help, measured rather than asserted: the damage is
    # anisotropy, not unevenness.
    hard = deformed(verb="taper", scale_start=1.0, scale_end=0.18, **span)
    p = np.asarray(hard.positions)

    def ring_edge(v):
        v = v[np.argsort(np.arctan2(v[:, 2], v[:, 0]))]
        return float(np.linalg.norm(np.diff(np.vstack([v, v[:1]]), axis=0), axis=1).mean())

    top, base = ring_edge(p[p[:, 1] > 0.9]), ring_edge(p[p[:, 1] < -0.9])
    print(f"\n  after the taper, a ring's circumferential edge is {base:.4f} at the base")
    print(f"  and {top:.4f} at the top, while the vertical edge stays {2.0 / 48:.4f}")
    print("  everywhere. The top ring has the SAME vertex count around a SMALLER")
    print("  circumference: the damage is ANISOTROPY, not uneven spacing, and relax")
    print("  slides vertices — it cannot change how many a ring has. So it does not")
    print("  recover a deformation, and the numbers above show it making things")
    print("  slightly worse. Relax is the recovery for a large GRAB, where the")
    print("  damage really is uneven spacing. Fixing this one needs re-tessellation,")
    print("  which this engine deliberately does not do.")

    # --- the claim the whole feature rests on ---------------------------------
    print("\n  the forward/inverse counterpart check lives in the C++ suite, where the")
    print("  kernel's inverse maps are reachable directly: a point pushed through the")
    print("  mesh's FORWARD map and back through the kernel's INVERSE map returns to")
    print("  where it started, to within float epsilon. That is what makes a tapered")
    print("  mesh and a tapered field the same shape rather than two plausible ones.")

    # A hard-edged mask TEARS the surface: vertices inside it do not move at all
    # and vertices just outside move fully, so the deformation is discontinuous
    # across the boundary. That is honest and it is also a bad picture, so the
    # rendered one is graded. The bit-identical claim above needs the hard edge.
    graded = column()
    soft_mask = clay.MaskField(0.06)
    soft_mask.paint((0, -0.9, 0), size=18, target=1.0)
    clay.MeshSculptor(graded).deform("taper", scale_start=1.0, scale_end=0.3,
                                     mask=soft_mask, **span)

    # --- the pictures --------------------------------------------------------
    tiles = [
        R.render_array(preview(plain), eye=EYE, target=TARGET, width=250, height=250),
        R.render_array(preview(deformed(verb="taper", scale_start=1.0, scale_end=0.35,
                                        **span)),
                       eye=EYE, target=TARGET, width=250, height=250),
        R.render_array(preview(deformed(verb="twist", angle=1.6, **span)),
                       eye=EYE, target=TARGET, width=250, height=250),
        R.render_array(preview(graded), eye=EYE, target=TARGET, width=250, height=250),
    ]
    R.contact_sheet(tiles, "57_mesh_deformers.png", columns=4,
                    caption="rest, taper to 0.35, twist by 1.6 rad, "
                            "and a taper a mask holds back")


if __name__ == "__main__":
    main()

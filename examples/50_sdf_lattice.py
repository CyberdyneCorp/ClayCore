"""A lattice cage on an SDF item — and the price of running it backwards.

`49_mesh_lattice` puts a cage on a mesh layer, where FFD runs *forward* and
nothing is approximated: a mesh knows where its vertices are, so you find each
one's parameters in the cage, evaluate, and move it. That is what ZBrush's Gizmo
Lattice and Blender's Lattice modifier do.

An SDF item has no vertices. Evaluating an implicit field means asking *what
material is at this point*, so every claycore deformer is an **inverse point
map** — `grab` returns `p - displacement * w`, `bend_curve` projects p back onto
its guide. And **forward FFD has no closed-form inverse**: pushing a point
through a cage is one tensor-product evaluation, but recovering which point
*landed* somewhere is a root find.

Three ways out, and claycore takes the third:

1. **Newton-invert per sample** — exact, and it puts iteration inside every
   backend's inner loop. The kernels are single-source across CPU, Metal, CUDA,
   OpenCL and Vulkan; that is not a price worth paying.
2. **Bake through a volume** — exact forward FFD, sampled once, and the cage
   stops being editable afterwards.
3. **Author the cage AS the inverse** — closed-form, portable, and not quite the
   exact inverse of the forward map.

The third is the house style: the basis is evaluated at the sample point rather
than at its preimage. What that costs is **not** `grab`'s character, and this
script measures it rather than borrowing the analogy — the first draft of this
file claimed "travels a little less than nominal" and the measurement disagreed.

The inverse cage is not the *exact* inverse of forward FFD. The two differ by a
term proportional to how the basis **varies** along the displacement, so the
error points the way the basis gradient does: it over-travels a drag toward
rising weight and under-travels one pointing away. `grab`'s weight always falls
off along its drag, which is why that one always under-travels; a lattice does
not inherit the sign. The measured difference below is under 1.5% of the drag.

Two things are exact even so, and both are worth knowing:

- an **untouched** cage is the undeformed item, bit for bit — offsets rather
  than positions buy that with no special case;
- a **uniform** cage is an exact translation, because the Bernstein basis is a
  partition of unity, and it costs no step scale at all.

Divisions are capped at 4 per axis here, against the mesh lattice's 32. This one
runs **per sample** inside the raymarcher at `nx*ny*nz` multiply-adds each time;
that one runs once per vertex.
"""

import numpy as np

import pyclay as clay

import _render as R

BOX = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
N = 3


def cage(**dragged):
    """An (N^3, 3) offset array. `dragged` maps "i_j_k" to a drag vector."""
    offsets = np.zeros((N * N * N, 3), dtype=np.float32)
    for key, v in dragged.items():
        i, j, k = (int(c) for c in key.split("_"))
        offsets[(k * N + j) * N + i] = v
    return offsets


def caged(offsets, r=0.6):
    """A SPHERE, for the measurements: bisecting along +X for its surface is
    unambiguous on a body of revolution."""
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=r).lattice(BOX, offsets), color="#b0784a")
    return doc


def caged_block(offsets):
    """A rounded BOX, for the pictures. A sphere is the wrong subject for a
    lattice render — it has no straight edge to bend and no corner to shear, so
    every cage looks like a slightly different sphere. A block shows what the
    cage is actually doing."""
    doc = clay.Document()
    doc.add_sdf_layer("l").add(
        clay.RoundBox(size=(1.0, 1.0, 1.0), r=0.06).lattice(BOX, offsets),
        color="#b0784a")
    return doc


def surface_along_x(doc, lo=0.0, hi=3.0, steps=60):
    """Where the surface crosses the +X axis, by bisection on the field."""
    for _ in range(steps):
        mid = 0.5 * (lo + hi)
        if doc.eval(np.array([[mid, 0.0, 0.0]], dtype=np.float32))[0] < 0.0:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def main():
    R.banner("50 sdf lattice — the cage, authored as its own inverse")

    r = 0.6
    plain = caged(cage(), r=r)

    # --- the identity is exact ----------------------------------------------
    rng = np.random.default_rng(50)
    pts = rng.uniform(-1.5, 1.5, size=(512, 3)).astype(np.float32)
    bare = clay.Document()
    bare.add_sdf_layer("l").add(clay.Sphere(r=r))
    worst = float(np.abs(plain.eval(pts) - bare.eval(pts)).max())
    print(f"  an untouched cage against no cage at all: worst difference {worst:.2e}")
    if worst > 1e-6:
        raise SystemExit("an untouched cage must be the undeformed item")

    # --- a uniform cage is an exact translation, and costs nothing -----------
    by = 0.35
    uniform = np.zeros((N * N * N, 3), dtype=np.float32)
    uniform[:, 0] = by
    moved = caged(uniform, r=r)
    reached = surface_along_x(moved)
    print(f"\n  a UNIFORM cage dragged {by:.2f} in +X:")
    print(f"    the surface reaches x={reached:.4f}, nominal {r + by:.4f} "
          f"— error {abs(reached - (r + by)):.2e}")
    print(f"    step scale {moved.safe_step_scale():.4f} — a translation distorts nothing")
    if abs(reached - (r + by)) > 1e-3:
        raise SystemExit("a uniform cage must be an exact translation")
    if moved.safe_step_scale() < 0.999:
        raise SystemExit("a rigid translation must not cost step scale")

    # --- a single control point: basis falloff, which is NOT the approximation
    # Worth separating, because it is easy to mistake one for the other. A
    # control point influences its neighbourhood, not the whole item, so the
    # surface picks up only a fraction of the drag — and a FORWARD cage does
    # exactly the same. This is the lattice working, not the inverse costing.
    print(f"\n  one control point on the +X face, dragged outward:")
    print(f"    {'drag':>7}{'reached':>10}{'travelled':>11}{'of drag':>10}"
          f"{'step scale':>12}")
    for drag in (0.10, 0.20, 0.35, 0.50):
        doc = caged(cage(**{"2_1_1": (drag, 0.0, 0.0)}), r=r)
        reached = surface_along_x(doc)
        travelled = reached - r
        print(f"    {drag:>7.2f}{reached:>10.4f}{travelled:>11.4f}"
              f"{travelled / drag * 100:>9.0f}%{doc.safe_step_scale():>12.4f}")
        if travelled <= 0.0:
            raise SystemExit("the surface did not move at all")
    print("    That fraction is the BASIS, not the inverse map: a forward cage")
    print("    falls off the same way. The cost of the inverse is below.")

    # --- what a 3x3x3 cage can say -------------------------------------------
    cases = [
        ("untouched", cage()),
        ("pinch", cage(**{f"{i}_1_{k}": ((1 - i) * 0.42, 0.0, (1 - k) * 0.42)
                          for i in (0, 2) for k in (0, 2)})),
        ("swell", cage(**{f"{i}_1_{k}": ((i - 1) * 0.45, 0.0, (k - 1) * 0.45)
                          for i in (0, 2) for k in (0, 2)})),
        ("shear", cage(**{f"{i}_2_{k}": (0.60, 0.0, 0.0)
                          for i in range(N) for k in range(N)})),
        ("twist", cage(**{f"{i}_2_{k}": (-(k - 1) * 0.55, 0.0, (i - 1) * 0.55)
                          for i in (0, 2) for k in (0, 2)})),
    ]

    tiles = []
    print("\n  shapes, and what each costs in step scale:")
    for name, offsets in cases:
        doc = caged_block(offsets)
        print(f"    {name:<10} step scale {doc.safe_step_scale():.4f}")
        eye, target = (2.2, 1.7, 2.6), (0.0, 0.0, 0.0)
        tiles.append(R.render_array(doc, eye=eye, target=target, width=250, height=250,
                                    colors_from_field=True))
    R.contact_sheet(tiles, "50_sdf_lattice.png", columns=5,
                    caption=", ".join(n for n, _ in cases))

    # --- the actual cost of the inverse: forward vs inverse, same cage -------
    # The same drag applied FORWARD on a mesh layer and INVERSELY here. This is
    # the only honest way to see what the approximation costs, because it holds
    # the basis falloff constant and varies only the direction the map runs.
    #
    # Both drag directions, because the error is SIGNED: it points the way the
    # basis gradient does. The probe is the +X extreme, where the basis rises
    # with x — so the inverse lands further out than the forward map whichever
    # way the control point was dragged.
    print("\n  the same cage, forward (mesh layer) and inverse (SDF item):")
    print(f"    {'drag':>7}{'forward':>10}{'inverse':>10}{'inverse-forward':>17}"
          f"{'of drag':>10}")
    mesh_source = bare.mesh(resolution=96)
    for drag in (-0.40, -0.20, 0.20, 0.40):
        m = clay.Mesh.from_triangles(np.array(mesh_source.positions, copy=True),
                                     np.array(mesh_source.indices, copy=True))
        mc = clay.Lattice(BOX, nx=N, ny=N, nz=N)
        mc.set_offset(2, 1, 1, (drag, 0.0, 0.0))
        clay.MeshSculptor(m).lattice(mc)
        forward = float(np.array(m.positions)[:, 0].max())
        inverse = surface_along_x(caged(cage(**{"2_1_1": (drag, 0.0, 0.0)}), r=r))
        gap = inverse - forward
        print(f"    {drag:>7.2f}{forward:>10.4f}{inverse:>10.4f}{gap:>+17.4f}"
              f"{abs(gap) / abs(drag) * 100:>9.1f}%")
        if abs(gap) > 0.03 * abs(drag):
            raise SystemExit("the inverse cage drifted further from forward FFD than expected")

    print("\n    Under 1.5% of the drag, and SIGNED — it lands outward of the")
    print("    forward map either way, because the basis rises with x at the")
    print("    probe. `grab` always under-travels because its weight always")
    print("    falls off along the drag; a lattice does not inherit that sign.")


if __name__ == "__main__":
    main()

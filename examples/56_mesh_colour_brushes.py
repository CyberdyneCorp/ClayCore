"""Paint and smear — the two mesh verbs that move nothing.

`45`–`47` and `55` cover fourteen mesh verbs, and every one of them moves
vertices. Nothing wrote `Mesh.colors`. A mesh layer could CARRY imported vertex
colours and export them, but not have them edited — the odd representation out,
since the SDF side paints through `Paint` strokes and the voxel side through its
palette. Blender's **Paint** and **Smear** are the pair that closes it.

**Paint** blends each vertex toward a target by the brush's own per-vertex
weight. That weight already carries the falloff, the mask gate and the alpha
stamp, so the verb composes with all three without a line of code about any of
them.

**Smear** drags existing colour along the stroke. For each vertex it blends
toward the one-ring neighbour lying most nearly OPPOSITE the drag — where the
colour under the cursor just came from. The one-ring rather than a spatial
query, because topology is fixed by contract here: the ring IS the
neighbourhood, and it cannot drift away from what the rest of the library means
by adjacency.

Three properties are worth demonstrating rather than asserting, and this script
measures all three:

- **they move nothing.** `positions` and `normals` come out byte-identical.
  That is the exact mirror of the guarantee the other fourteen verbs make about
  `colors`, and it is what lets a colour pass run over a finished sculpt
  without a diff on the geometry;
- **the ends of the blend are exact.** A fully-weighted dab lands on the target
  bit-identically rather than one ULP away, so a stroke leaves no seam along
  its rim;
- **a zero drag is no smear**, rather than degenerating into a smooth. A verb
  that silently becomes a different verb is worse than one that refuses.

The pictures come from `_render.render_mesh_array`, which rasterises a mesh's
own vertices — and that is the point rather than an implementation detail. The
ordinary preview path traces a DOCUMENT, and a mesh reaches one through
`Volume.from_mesh`, which carries a single colour for the whole item. A picture
of vertex colour has to come from the vertices.
"""

import numpy as np

import pyclay as clay

import _render as R

# Looking DOWN on the crown, which is where the dabs land. A camera at eye
# level puts them on the silhouette, where a smear that really moved colour
# still looks like nothing happened.
EYE, TARGET = (0.0, 2.5, 1.3), (0.0, 0.55, 0.0)
WHITE = (0.93, 0.91, 0.88)


def ball(resolution=44):
    """A closed sphere, meshed from a field, then rebuilt from bare triangles.

    Curvature matters: the one-ring directions a smear chooses between are only
    interesting on a surface that turns.

    The rebuild is not incidental. `Document.mesh` writes vertex colours — the
    layer's own colour — so a ball straight from the mesher ALREADY has the
    attribute, `ensure_colors` correctly declines to overwrite it, and a script
    that skipped this step would silently be painting over the mesher's grey
    while claiming to start from white."""
    doc = clay.Document()
    doc.add_sdf_layer("src").add(clay.Sphere(r=1.0))
    meshed = doc.mesh(resolution=resolution)
    return clay.Mesh.from_triangles(np.array(meshed.positions, copy=True),
                                    np.array(meshed.indices, copy=True))


def painted_ball(passes):
    """A fresh ball with `passes` applied, each a (verb, kwargs) pair."""
    mesh = ball()
    sculptor = clay.MeshSculptor(mesh)
    sculptor.ensure_colors(WHITE)
    for verb, kwargs in passes:
        sculptor.stamp(verb, **kwargs)
    return mesh


def main():
    R.banner("56 mesh colour brushes — paint and smear")

    crown = (0.0, 1.0, 0.0)

    # --- they move nothing ---------------------------------------------------
    print("  the property that separates them from the other fourteen verbs:")
    for verb, extra in (("paint", {"color": (0.85, 0.15, 0.12)}),
                        ("smear", {"direction": (0.25, 0.0, 0.0)})):
        mesh = ball()
        sculptor = clay.MeshSculptor(mesh)
        sculptor.ensure_colors(WHITE)
        sculptor.stamp("paint", center=(-0.35, 0.95, 0.0), radius=0.5, strength=1.0,
                       color=(0.85, 0.15, 0.12))
        before_p = np.array(mesh.positions, copy=True)
        before_n = np.array(mesh.normals, copy=True)
        before_c = np.array(mesh.colors, copy=True)

        touched = sculptor.stamp(verb, center=crown, radius=0.55, strength=0.9, **extra)
        moved_p = not np.array_equal(before_p, np.asarray(mesh.positions))
        moved_n = not np.array_equal(before_n, np.asarray(mesh.normals))
        changed_c = not np.array_equal(before_c, np.asarray(mesh.colors))
        print(f"    {verb:>6}: {touched:>5} classes | colours changed: {str(changed_c):>5}"
              f" | positions moved: {str(moved_p):>5} | normals moved: {str(moved_n):>5}")
        if moved_p or moved_n or not changed_c:
            raise SystemExit(f"{verb} must write colour and move nothing")

    # --- the ends of the blend are exact -------------------------------------
    mesh = ball()
    sculptor = clay.MeshSculptor(mesh)
    sculptor.ensure_colors(WHITE)
    target = (0.25, 0.50, 0.75)
    sculptor.stamp("paint", center=crown, radius=0.5, strength=1.0, falloff="constant",
                   color=target)
    cols = np.asarray(mesh.colors)
    exact = int(np.count_nonzero(np.all(cols == np.array(target), axis=1)))
    print(f"\n  a full-weight dab lands on the target BIT-IDENTICALLY: {exact} vertices")
    if exact == 0:
        raise SystemExit("mix(a, b, 1) must be b exactly, or every stroke leaves a seam")

    # --- paint falls off ------------------------------------------------------
    mesh = ball()
    sculptor = clay.MeshSculptor(mesh)
    sculptor.ensure_colors(WHITE)
    sculptor.stamp("paint", center=crown, radius=0.6, strength=1.0, color=(0.85, 0.15, 0.12))
    p, cols = np.asarray(mesh.positions), np.asarray(mesh.colors)
    d = np.linalg.norm(p - np.array(crown), axis=1)
    # Redness as r MINUS g. The red used here is (0.85, 0.15, 0.12) against a
    # (0.93, 0.91, 0.88) white, so the RED CHANNEL ALONE goes DOWN where paint
    # lands — a column of that reads backwards and looks like a bug.
    redness = cols[:, 0] - cols[:, 1]
    print("\n  paint's falloff, as distance from the brush centre (radius 0.6):")
    print(f"    {'distance':>12}{'redness':>10}")
    for lo, hi in ((0.0, 0.1), (0.2, 0.3), (0.4, 0.5), (0.55, 0.6), (0.7, 0.9)):
        band = (d >= lo) & (d < hi)
        if band.any():
            print(f"    {lo:.2f}-{hi:.2f}{redness[band].mean():>13.3f}")

    # --- smear has a direction -----------------------------------------------
    def red_centroid_x(m):
        """Redness as r MINUS g, not r alone: the red used here is (0.85, ...)
        and the white base is (0.93, ...), so the red channel actually goes
        DOWN where paint lands. r - g separates them cleanly."""
        c = np.asarray(m.colors)
        redness = np.clip(c[:, 0] - c[:, 1], 0.0, None)
        total = redness.sum()
        return float((redness * np.asarray(m.positions)[:, 0]).sum() / total) if total else 0.0

    base_passes = [("paint", dict(center=(-0.3, 0.95, 0.0), radius=0.5, strength=1.0,
                                  color=(0.85, 0.15, 0.12)))]
    still = painted_ball(base_passes)
    start = red_centroid_x(still)
    print(f"\n  smear drags colour, and the direction is the verb:")
    print(f"    {'drag':>14}{'red centroid x':>17}")
    print(f"    {'(none)':>14}{start:>17.4f}")
    for label, dx in (("+x", 0.25), ("-x", -0.25)):
        m = painted_ball(base_passes + [("smear", dict(center=crown, radius=0.6, strength=1.0,
                                                       direction=(dx, 0.0, 0.0)))] * 4)
        print(f"    {label:>14}{red_centroid_x(m):>17.4f}")

    zero = painted_ball(base_passes + [("smear", dict(center=crown, radius=0.6, strength=1.0,
                                                      direction=(0.0, 0.0, 0.0)))])
    if not np.array_equal(np.asarray(zero.colors), np.asarray(still.colors)):
        raise SystemExit("a zero drag must be no smear, not a smooth")
    print("    a zero drag changes nothing — it is not a smooth wearing smear's name")

    # --- and the attribute is created on purpose ------------------------------
    bare = ball(24)  # bare triangles: no colour attribute at all
    sc = clay.MeshSculptor(bare)
    print(f"\n  a mesh with no colour attribute: has_colors={sc.has_colors}")
    refused = sc.stamp("paint", center=crown, radius=0.5, color=(1, 0, 0))
    print(f"    painting it touches {refused} classes — it refuses rather than allocating")
    print(f"    ensure_colors created one: {sc.ensure_colors(WHITE)}")
    after = sc.stamp("paint", center=crown, radius=0.5, color=(1, 0, 0))
    print(f"    and now painting touches {after}")
    if refused != 0 or after == 0:
        raise SystemExit("the colour verbs must refuse a colourless mesh, then work")

    # --- the pictures ---------------------------------------------------------
    dab = dict(center=(-0.3, 0.95, 0.0), radius=0.5, strength=1.0, color=(0.85, 0.15, 0.12))
    blue = dict(center=(0.35, 0.9, 0.15), radius=0.42, strength=0.9, color=(0.15, 0.35, 0.85))
    tiles = [
        R.render_mesh_array(painted_ball([]), eye=EYE, target=TARGET, width=250, height=250),
        R.render_mesh_array(painted_ball([("paint", dab)]), eye=EYE, target=TARGET,
                            width=250, height=250),
        R.render_mesh_array(painted_ball([("paint", dab), ("paint", blue)]),
                            eye=EYE, target=TARGET, width=250, height=250),
        R.render_mesh_array(painted_ball([("paint", dab), ("paint", blue)]
                               + [("smear", dict(center=crown, radius=0.75, strength=1.0,
                                                 direction=(0.25, 0.0, 0.0)))] * 14),
                            eye=EYE, target=TARGET, width=250, height=250),
    ]
    R.contact_sheet(tiles, "56_mesh_colour_brushes.png", columns=4,
                    caption="white, one paint dab, a second colour, "
                            "then fourteen smears dragged +x")


if __name__ == "__main__":
    main()

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

The pictures are rasterised HERE rather than through `_render`, and that is the
point rather than an oversight: the preview path resamples a mesh into a field
through `Volume.from_mesh`, which carries one colour for the whole item. A
picture of vertex colour has to come from the vertices.
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


def rasterize(mesh, width=260, height=260, eye=EYE, target=TARGET):
    """A painter's-algorithm rasteriser with a z-buffer, so vertex colour is
    actually visible. Barycentric interpolation across each triangle, and a
    single lambert term so the sphere reads as a sphere."""
    p = np.asarray(mesh.positions, dtype=np.float64)
    idx = np.asarray(mesh.indices, dtype=np.int64).reshape(-1, 3)
    col = np.asarray(mesh.colors, dtype=np.float64)

    forward = np.array(target, dtype=np.float64) - np.array(eye, dtype=np.float64)
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, (0.0, 1.0, 0.0))
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)

    rel = p - np.array(eye, dtype=np.float64)
    cam = np.stack([rel @ right, rel @ up, rel @ forward], axis=1)
    depth = np.maximum(cam[:, 2], 1e-6)
    focal = 1.0 / np.tan(np.radians(35.0) * 0.5)
    sx = (cam[:, 0] / depth * focal * 0.5 + 0.5) * width
    sy = (0.5 - cam[:, 1] / depth * focal * 0.5) * height

    image = np.zeros((height, width, 3))
    image[:] = np.linspace(0.10, 0.16, height)[:, None, None]
    zbuf = np.full((height, width), np.inf)

    # Backface cull in screen space, then draw. Front faces only means the far
    # side of the ball never overwrites the near side, whatever the order.
    a, b, c = idx[:, 0], idx[:, 1], idx[:, 2]
    area = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a])
    front = np.where((area < 0) & (depth[a] > 0) & (depth[b] > 0) & (depth[c] > 0))[0]

    light = np.array([0.4, 0.8, 0.5])
    light /= np.linalg.norm(light)
    normals = p / np.linalg.norm(p, axis=1, keepdims=True)
    shade = np.clip(normals @ light, 0.0, 1.0) * 0.75 + 0.25

    for t in front:
        i, j, k = idx[t]
        xs, ys = np.array([sx[i], sx[j], sx[k]]), np.array([sy[i], sy[j], sy[k]])
        x0, x1 = int(max(0, np.floor(xs.min()))), int(min(width - 1, np.ceil(xs.max())))
        y0, y1 = int(max(0, np.floor(ys.min()))), int(min(height - 1, np.ceil(ys.max())))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        det = area[t]
        w0 = ((xs[1] - gx) * (ys[2] - gy) - (xs[2] - gx) * (ys[1] - gy)) / det
        w1 = ((xs[2] - gx) * (ys[0] - gy) - (xs[0] - gx) * (ys[2] - gy)) / det
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        z = w0 * depth[i] + w1 * depth[j] + w2 * depth[k]
        tile = zbuf[y0:y1 + 1, x0:x1 + 1]
        hit = inside & (z < tile)
        if not hit.any():
            continue
        rgb = (w0[..., None] * col[i] + w1[..., None] * col[j] + w2[..., None] * col[k])
        lit = (w0 * shade[i] + w1 * shade[j] + w2 * shade[k])[..., None]
        patch = image[y0:y1 + 1, x0:x1 + 1]
        patch[hit] = np.clip(rgb * lit, 0.0, 1.0)[hit]
        tile[hit] = z[hit]
    return np.clip(image, 0.0, 1.0)


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
        rasterize(painted_ball([])),
        rasterize(painted_ball([("paint", dab)])),
        rasterize(painted_ball([("paint", dab), ("paint", blue)])),
        rasterize(painted_ball([("paint", dab), ("paint", blue)]
                               + [("smear", dict(center=crown, radius=0.75, strength=1.0,
                                                 direction=(0.25, 0.0, 0.0)))] * 14)),
    ]
    R.contact_sheet(tiles, "56_mesh_colour_brushes.png", columns=4,
                    caption="white, one paint dab, a second colour, "
                            "then fourteen smears dragged +x")


if __name__ == "__main__":
    main()

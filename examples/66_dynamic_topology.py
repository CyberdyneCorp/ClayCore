"""A coarse sphere becomes a face: geometry made where the brush needs it.

THE GAP THIS CLOSES, stated precisely, because "add dyntopo" is not it: until
now there was **no representation in this library whose connectivity could
change.** `Mesh` is flat arrays a mutation renumbers, `Adjacency` is CSR that
goes stale on a count change, `Bvh.refit` refuses a topology change by design,
and `VertexDeltas` records no indices. Every one of those is a correct decision
for what it serves, and together they meant a mesh-layer snakehook stretched
triangles until the surface was unusable and the library's advice was to leave
and retopologise elsewhere.

So this is a different representation beside the fixed one, never a mode it
slips into. `MeshSculptor`'s contract — no verb creates, splits, deletes or
reorders a polygon, and `indices` and `quads` come out byte-identical — is
untouched, and the last section here proves it on the same model.

WHAT THE PICTURES SHOW, and what the numbers underneath them assert:

  - A NOSE pulled out of a sphere far coarser than the feature. The triangle
    count under the brush rises while the model's total rises by much less,
    because the refinement is local: a dab costs what it touches.

  - AN EAR, from the same coarse start, with the detail slider turned up. The
    target edge length is BRUSH-RELATIVE, so shrinking the brush makes finer
    geometry without a second control — which is what a sculptor means by
    "detail".

  - A HORN grown with Snakehook, the verb that made the old boundary a problem
    rather than a scope: it stretches triangles to the extreme by design, and on
    an adaptive surface it grows geometry as it pulls.

  - SMOOTH TAKING DETAIL BACK OUT. Collapse is not a repair operation bolted on
    beside splitting; it is half of what makes the density follow the brush.

Every one of those is asserted rather than illustrated, and the whole thing is
one undo step.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.6, 1.4, 2.9), (0.0, 0.15, 0.0)


def coarse_sphere(n=10, radius=1.0):
    """A cube-sphere, deliberately far too coarse for the features below.

    Built by hand rather than meshed from a field, because the point is to start
    with FEWER triangles than the detail needs — a mesher asked for a good
    sphere would hide exactly what this example is about.
    """
    positions, indices = [], []
    axes = [(0, 1, 2), (0, 1, 2), (1, 2, 0), (1, 2, 0), (2, 0, 1), (2, 0, 1)]
    signs = [1.0, -1.0, 1.0, -1.0, 1.0, -1.0]
    for f in range(6):
        base = len(positions)
        for v in range(n + 1):
            for u in range(n + 1):
                c = [0.0, 0.0, 0.0]
                c[axes[f][0]] = -1.0 + 2.0 * u / n
                c[axes[f][1]] = -1.0 + 2.0 * v / n
                c[axes[f][2]] = signs[f]
                length = float(np.linalg.norm(c))
                positions.append([x / length * radius for x in c])
        stride = n + 1
        for v in range(n):
            for u in range(n):
                a = base + v * stride + u
                b, c2, d = a + 1, a + stride, a + stride + 1
                if signs[f] > 0.0:
                    indices += [a, c2, b, b, c2, d]
                else:
                    indices += [a, b, c2, b, d, c2]
    return clay.Mesh.from_triangles(np.array(positions, dtype=np.float32),
                                    np.array(indices, dtype=np.uint32))


def preview(mesh, cell=0.012, colour="#b0784a"):
    """A DISPLAY-ONLY document. A mesh layer is never evaluated, so the
    renderer — which raycasts a field — has nothing to trace against;
    resampling here is exactly the approximation a mesh layer exists to avoid,
    and it is fine for a picture and wrong for the export."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def triangles_near(mesh, centre, radius):
    """How many triangles have a corner within `radius` of `centre`.

    THE MEASUREMENT THIS EXAMPLE IS BUILT AROUND. "The brush refined locally" is
    a claim about the ratio between this and the model's total, and nothing else
    distinguishes local refinement from global refinement that happens to look
    right from one camera.
    """
    p = np.array(mesh.positions)
    idx = np.array(mesh.indices).reshape(-1, 3)
    near = np.linalg.norm(p - np.array(centre, dtype=np.float32), axis=1) < radius
    return int(np.count_nonzero(near[idx].any(axis=1)))


def surface_of(mesh):
    surface = clay.DynamicSurface.from_mesh(mesh)
    report = surface.validate()
    if not report["ok"]:
        raise SystemExit(f"the imported surface is not valid: {report['summary']}")
    return surface


def topology(resolution, enabled=True):
    t = clay.TopologySettings()
    t.enabled = enabled
    t.detail_mode = clay.DetailMode.BRUSH_RELATIVE
    t.detail_resolution = resolution
    return t


def drag(surface, sculptor, verb, start, direction, steps, radius, strength, topo):
    """A stroke: several stamps walking along a direction."""
    totals = dict(moved=0, split=0, collapsed=0, flipped=0)
    for i in range(steps):
        t = i / max(steps - 1, 1)
        centre = tuple(float(s + d * t) for s, d in zip(start, direction))
        r = sculptor.stamp(verb, center=centre, radius=radius, strength=strength,
                           topology=topo, direction=tuple(float(d / steps) for d in direction))
        for k in totals:
            totals[k] += r[k]
        report = surface.validate()
        if not report["ok"]:
            raise SystemExit(f"{verb} broke the surface at stamp {i}: {report['summary']}")
    return totals


def main():
    R.banner("66 dynamic topology — geometry made where the brush needs it")

    base = coarse_sphere(10, 1.0)
    print(f"  the starting sphere is {base.triangle_count} triangles — deliberately far")
    print("  too coarse for anything below, which is the whole point.")

    tiles = [R.render_tile(preview(base), eye=EYE, target=TARGET, size=190)]
    labels = ["coarse sphere"]

    # --- the nose -------------------------------------------------------------
    surface = surface_of(base)
    sculptor = clay.DynamicSculptor(surface)
    nose_centre = (0.0, 0.05, 1.0)
    before_local = triangles_near(surface.to_mesh(), nose_centre, 0.35)
    before_total = surface.face_count

    stats = drag(surface, sculptor, "draw", (0.0, 0.05, 0.98), (0.0, 0.0, 0.30),
                 steps=5, radius=0.3, strength=0.5, topo=topology(7.0))
    nose = surface.to_mesh()
    after_local = triangles_near(nose, nose_centre, 0.35)
    print(f"\n  NOSE      {stats['split']:>5} splits, {stats['collapsed']:>4} collapses, "
          f"{stats['moved']:>5} vertices moved")
    print(f"            triangles under the brush {before_local} -> {after_local}, "
          f"model {before_total} -> {surface.face_count}")

    if stats["split"] == 0:
        raise SystemExit("the nose created no geometry — the remesher did not engage")
    local_growth = after_local / max(before_local, 1)
    total_growth = surface.face_count / max(before_total, 1)
    if not local_growth > total_growth * 1.5:
        raise SystemExit(
            f"the refinement was not local: triangles under the brush grew {local_growth:.2f}x "
            f"against {total_growth:.2f}x for the whole model")
    print(f"            {local_growth:.1f}x under the brush against {total_growth:.1f}x overall:")
    print("            a dab costs what it touches.")
    tiles.append(R.render_tile(preview(nose), eye=EYE, target=TARGET, size=190))
    labels.append("nose")

    # --- the ear, finer -------------------------------------------------------
    ear_surface = surface_of(base)
    ear_sculptor = clay.DynamicSculptor(ear_surface)
    ear_centre = (0.95, 0.1, 0.0)
    coarse_stats = drag(ear_surface, ear_sculptor, "draw", (0.92, 0.1, 0.0), (0.25, 0.05, 0.0),
                        steps=4, radius=0.28, strength=0.45, topo=topology(4.0))
    coarse_faces = ear_surface.face_count

    fine_surface = surface_of(base)
    fine_sculptor = clay.DynamicSculptor(fine_surface)
    fine_stats = drag(fine_surface, fine_sculptor, "draw", (0.92, 0.1, 0.0), (0.25, 0.05, 0.0),
                      steps=4, radius=0.28, strength=0.45, topo=topology(10.0))
    print(f"\n  EAR       detail 4 -> {coarse_stats['split']} splits, "
          f"{coarse_faces} faces")
    print(f"            detail 10 -> {fine_stats['split']} splits, "
          f"{fine_surface.face_count} faces")
    if not fine_stats["split"] > coarse_stats["split"]:
        raise SystemExit("turning the detail up made no more geometry")
    print("            the target edge length is BRUSH-RELATIVE, so the same gesture at")
    print("            a higher detail makes finer geometry with no second slider.")
    tiles.append(R.render_tile(preview(fine_surface.to_mesh()), eye=EYE, target=TARGET, size=190))
    labels.append("ear (fine)")

    # --- the horn -------------------------------------------------------------
    #
    # THE VERB THAT MADE THE OLD BOUNDARY A PROBLEM RATHER THAN A SCOPE. On a
    # fixed mesh a snakehook stretches triangles to the extreme by design, and
    # the library's advice was that the stretch is the artist's signal to leave.
    horn_surface = surface_of(base)
    horn_sculptor = clay.DynamicSculptor(horn_surface)
    horn_before = horn_surface.face_count
    horn_stats = drag(horn_surface, horn_sculptor, "snakehook", (0.0, 0.98, 0.0),
                      (0.0, 0.55, 0.10), steps=8, radius=0.26, strength=1.0,
                      topo=topology(8.0))
    horn = horn_surface.to_mesh()
    print(f"\n  HORN      {horn_stats['split']:>5} splits while it pulled, "
          f"{horn_before} -> {horn_surface.face_count} faces")
    if horn_stats["split"] == 0:
        raise SystemExit("the horn grew no geometry as it pulled")
    reach = float(np.array(horn.positions)[:, 1].max())
    print(f"            it reaches y={reach:.2f} from a sphere of radius 1.0, with")
    print("            geometry grown along the pull rather than stretched over it.")
    if reach <= 1.05:
        raise SystemExit("the snakehook did not actually pull anything out")
    tiles.append(R.render_tile(preview(horn), eye=EYE, target=TARGET, size=190))
    labels.append("horn")

    # --- smooth takes it back out --------------------------------------------
    #
    # Collapse is not a repair bolted on beside splitting; it is half of what
    # makes the density follow the brush.
    dense_faces = horn_surface.face_count
    smooth_stats = drag(horn_surface, horn_sculptor, "smooth", (0.0, 1.3, 0.05),
                        (0.0, 0.0, 0.0), steps=6, radius=0.4, strength=1.0,
                        topo=topology(2.5))
    print(f"\n  SMOOTH    {smooth_stats['collapsed']:>5} collapses, "
          f"{dense_faces} -> {horn_surface.face_count} faces")
    if smooth_stats["collapsed"] == 0:
        raise SystemExit("nothing was collapsed — the density only ever grows")
    if horn_surface.face_count >= dense_faces:
        raise SystemExit("smoothing at a coarse detail did not thin the region")
    print("            geometry is REMOVED where the brush no longer needs it.")
    tiles.append(R.render_tile(preview(horn_surface.to_mesh()), eye=EYE, target=TARGET,
                               size=190))
    labels.append("smoothed")

    R.contact_sheet(tiles, "66_dynamic_topology.png", columns=3,
                    caption=" | ".join(labels))

    # --- what is NOT affected -------------------------------------------------
    #
    # The fixed-topology contract is what makes a mesh layer worth holding after
    # a retopology pass, and none of the above changes it.
    fixed = clay.Mesh.from_triangles(np.array(base.positions, copy=True),
                                     np.array(base.indices, copy=True))
    indices_before = np.array(fixed.indices, copy=True)
    fixed_sculptor = clay.MeshSculptor(fixed)
    moved = fixed_sculptor.stamp("draw", center=nose_centre, radius=0.3, strength=0.5)
    if moved == 0:
        raise SystemExit("the fixed sculptor did nothing, so this proves nothing")
    if not np.array_equal(np.array(fixed.indices), indices_before):
        raise SystemExit("the FIXED sculptor changed the topology — the contract is broken")
    print(f"\n  the fixed sculptor is untouched: {moved} classes moved and the index")
    print("  buffer is byte-identical. Adaptive topology is a representation a caller")
    print("  converts into deliberately, never a mode the fixed one slips into.")

    # --- and a dynamic surface is triangles ----------------------------------
    if len(horn.quads) != 0:
        raise SystemExit("a dynamic surface exported quads; it is triangles by construction")
    print("\n  the export carries no quads and derives none: a quad workflow does not")
    print("  pass through this representation, which is stated rather than guessed at.")

    obj = R.output_path("66_dynamic_source.obj")
    horn_surface.to_mesh().save(obj)
    print(f"  wrote {os.path.basename(obj)} for inspection.")


if __name__ == "__main__":
    main()

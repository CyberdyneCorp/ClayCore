"""Groups: a sub-expression that combines as one thing.

An op applies to everything accumulated before it. That is the rule the whole
edit list runs on, and it is why `35_hard_surface_helmet.py` gives every
armour plate a layer of its own: a plate is *a shell INTERSECTED with a
cutter*, and on one shared field that intersect would trim the helmet as well
as the plate.

A **group** is the answer, and the engine has had one all along — it simply
was not reachable from a binding. A group's children compile as ONE
sub-expression: the intersect applies inside it, and the group's own op
combines the result with whatever the layer already holds. So the three plates
below live on the *same* layer as the core they ride on, which is what a layer
was always supposed to mean — a thing you show, hide, lock and transform, not
an expression-grouping trick.

The fourth tile is the same edits without a group, kept in the gallery on
purpose: it is what "the intersect applies to everything" actually looks like.

`Op.INLINE` is the other direction — a group whose children apply to the outer
chain exactly as if they had been added there. It buys naming and moving a
run of edits as a unit, not a different field, and the example checks that the
two are bit-identical rather than merely similar.
"""

import math

import numpy as np

import pyclay as clay

import _render as R

CORE = "#3f454d"      # the dark under-shell every panel gap shows
PLATE_A = "#8f97a1"
PLATE_B = "#6b737d"
TRIM = "#b3862f"

# A CutHollowSphere bowl opens +Y; these turn it into a cap whose material
# faces the named direction, the same convention 35_hard_surface_helmet uses.
CAP_UP = ((1, 0, 0), math.pi)
CAP_SIDE = ((0, 0, 1), math.pi / 2)
CAP_FWD = ((1, 0, 0), -math.pi / 2)

# Probes: a point inside the crown plate, a point its cutter throws away, and
# a point inside the CORE that the crown's cutter would reach if the intersect
# were not fenced in by the group — it is outboard of the cutter's own box.
KEPT = (0.0, 0.60, 0.0)
CUT_AWAY = (0.50, 0.32, 0.0)
CORE_OUTBOARD = (0.42, 0.0, 0.0)


def core(layer):
    """The under-shell. Every gap between plates is where this shows."""
    return layer.add(clay.Sphere(r=0.50), color=CORE)


def plate(layer, shell, cutter, color, parent=None):
    """One armour plate: a shell, trimmed by a cutter, as a single group.

    The intersect is INSIDE the group, so it reaches the shell and nothing
    else. The group then joins the assembly with its own op — here a hard
    union, so the plate edge steps against the core instead of melting into it.
    """
    group = layer.add_group(op=clay.Op.ADD, parent=parent)
    layer.add(shell, parent=group, color=color)
    layer.add(cutter, op=clay.Op.INTERSECT, parent=group)
    return group


def crown(layer):
    """The top plate: a dome, kept only as a central keel."""
    return plate(layer,
                 clay.CutHollowSphere(r=0.60, h=0.17, t=0.05,
                                      position=(0, 0.02, 0),
                                      rotation_axis_angle=CAP_UP),
                 clay.Box(size=(0.62, 2.0, 2.0)),
                 PLATE_A)


def cheek(layer):
    """A side plate, kept only above the jaw line."""
    return plate(layer,
                 clay.CutHollowSphere(r=0.585, h=0.15, t=0.05,
                                      position=(0.03, -0.02, 0),
                                      rotation_axis_angle=CAP_SIDE),
                 clay.Box(size=(2.0, 0.66, 1.10), position=(0, -0.06, 0)),
                 PLATE_B)


def visor(layer):
    """The front plate, kept only across the brow."""
    return plate(layer,
                 clay.CutHollowSphere(r=0.595, h=0.16, t=0.05,
                                      position=(0, -0.01, 0.03),
                                      rotation_axis_angle=CAP_FWD),
                 clay.Box(size=(0.90, 0.42, 2.0), position=(0, 0.10, 0)),
                 PLATE_A)


def vents(layer):
    """A carving group: one slot, repeated, cutting every plate at once.

    A group whose op carves needs something beneath it in the chain, and gets
    it — the core and three plates are already there. Put it FIRST and the
    compiler emits nothing at all, which is the same rule a carving item obeys.
    """
    group = layer.add_group(op=clay.Op.SUBTRACT, blend=clay.Chamfer(0.008))
    layer.add(clay.RoundBox(size=(0.42, 0.05, 0.035), r=0.012,
                            position=(0.0, 0.56, 0.10))
              .repeat_grid(spacing=0.10, counts=(0, 0, 2)),
              parent=group)
    return group


def assembled():
    """Core and three plates, on ONE layer, plus the vent cuts."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("helmet")
    core(layer)
    groups = [crown(layer), cheek(layer), visor(layer)]
    groups.append(vents(layer))
    return doc, layer, groups


def flattened():
    """The same edits with no group at all — the thing groups exist to fix."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("helmet")
    core(layer)
    layer.add(clay.CutHollowSphere(r=0.60, h=0.17, t=0.05, position=(0, 0.02, 0),
                                   rotation_axis_angle=CAP_UP), color=PLATE_A)
    layer.add(clay.Box(size=(0.62, 2.0, 2.0)), op=clay.Op.INTERSECT)
    return doc, layer


def inline_pair(grouped):
    """The same two edits, once under an Op.INLINE group and once loose."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.6))
    parent = layer.add_group(op=clay.Op.INLINE) if grouped else None
    layer.add(clay.Box(size=(0.5, 0.5, 2.0), position=(0.35, 0.15, 0)),
              op=clay.Op.SUBTRACT, parent=parent)
    layer.add(clay.Sphere(r=0.22, position=(0, 0.62, 0)), parent=parent, color=TRIM)
    return doc


def frame(azimuth=34.0, elevation=14.0):
    lo = np.array([-0.68, -0.62, -0.68], dtype=np.float32)
    hi = np.array([0.68, 0.72, 0.68], dtype=np.float32)
    return R.orbit_camera((lo, hi), azimuth=azimuth, elevation=elevation, margin=1.06)


def probe(doc, points):
    return doc.eval(np.array(points, dtype=np.float32))


def main():
    R.banner("36 groups — a sub-expression that combines as one thing")

    doc, layer, groups = assembled()
    flat_doc, _flat_layer = flattened()

    # -- what the group actually bought -------------------------------------
    kept, cut_away, back = probe(doc, [KEPT, CUT_AWAY, CORE_OUTBOARD])
    flat_back = probe(flat_doc, [CORE_OUTBOARD])[0]
    print(f"  plate kept        d = {kept:+.4f}  (inside the keel)")
    print(f"  plate trimmed     d = {cut_away:+.4f}  (the cutter took this)")
    print(f"  core, grouped     d = {back:+.4f}  (untouched by the intersect)")
    print(f"  core, no group    d = {flat_back:+.4f}  (the intersect reached it)")
    if not (kept < 0.0 and cut_away > 0.0):
        raise SystemExit("the group's intersect no longer trims its own shell")
    if not back < 0.0:
        raise SystemExit("the intersect escaped its group and cut the core")
    if not flat_back > 0.0:
        raise SystemExit("the ungrouped intersect no longer reaches the core — "
                         "the comparison this example rests on is stale")

    # -- one layer instead of one per plate ---------------------------------
    plates, counts = groups[:3], [len(layer.children(g)) for g in groups]
    print(f"  1 layer holds {len(groups)} groups with {counts} children "
          f"({len(plates)} plates and a carve); the layer-per-plate technique "
          f"needs {len(plates)} layers for the same thing")
    if counts[:3] != [2, 2, 2]:
        raise SystemExit("a plate is a shell and a cutter — two children each")

    # -- Op.INLINE is the same field, not a similar one ---------------------
    points = np.random.default_rng(36).uniform(-1.5, 1.5, size=(4096, 3)).astype(np.float32)
    inline_d = inline_pair(True).eval(points)
    loose_d = inline_pair(False).eval(points)
    worst = float(np.max(np.abs(inline_d - loose_d)))
    print(f"  Op.INLINE vs the same edits loose: worst gap {worst:g} over "
          f"{points.shape[0]} points")
    if not np.array_equal(inline_d, loose_d):
        raise SystemExit("an inline group is supposed to be bit-identical to its "
                         "children added directly")

    # -- groups survive a save and reload unchanged -------------------------
    # save -> load -> save: the strict form, since a tree that came back with
    # the same field but a different order would still fail this.
    path = R.output_path("36_groups.clayspace")
    doc.save(path)
    with open(path, "rb") as f:
        first = f.read()
    reloaded = clay.load(path)
    reloaded.save(path)
    with open(path, "rb") as f:
        again = f.read()
    print(f"  {len(first)} bytes of groups; reserialises identically: {again == first}")
    if again != first:
        raise SystemExit("a document of groups did not reserialise identically")
    if not np.array_equal(reloaded.eval(points), doc.eval(points)):
        raise SystemExit("a reloaded document of groups evaluates differently")

    # -- pictures ------------------------------------------------------------
    eye, target = frame()
    stages = []
    for build in ("core", "crown", "all", "vents"):
        staged = clay.Document()
        staged_layer = staged.add_sdf_layer("helmet")
        core(staged_layer)
        if build != "core":
            crown(staged_layer)
        if build in ("all", "vents"):
            cheek(staged_layer)
            visor(staged_layer)
        if build == "vents":
            vents(staged_layer)
        stages.append(R.render_tile(staged, eye=eye, target=target, size=220,
                                    colors_from_field=True, ao=8, ao_reach=0.08))
    R.contact_sheet(stages, "36_groups_stages.png", columns=4,
                    caption="core, one plate group, three, vents carved")

    # Seen from the side, where a slab reads as a slab: the ungrouped intersect
    # trims the CORE too, so what is left is the cutter's own box.
    side_eye, side_target = frame(azimuth=88.0, elevation=10.0)
    grouped_tile = R.render_tile(doc, eye=side_eye, target=side_target, size=300,
                                 colors_from_field=True, ao=10, ao_reach=0.08)
    flat_tile = R.render_tile(flat_doc, eye=side_eye, target=side_target, size=300,
                              colors_from_field=True, ao=10, ao_reach=0.08)
    R.side_by_side(grouped_tile, flat_tile, "36_groups_vs_flat.png",
                   caption="the intersect inside a group, and the same intersect loose")

    # The hero shot carries ambient occlusion, which render() does not take —
    # panel edges are what a plated model is read through, and without it the
    # steps between plate and core disappear.
    eye, target = frame(azimuth=24.0, elevation=10.0)
    image = R.render_array(doc, eye=eye, target=target, colors_from_field=True,
                           ao=12, ao_reach=0.08)
    R.write_png(R.output_path("36_groups.png"), image)
    print("  wrote output/36_groups.png  (four groups, one layer)")
    print(f"  step scale {doc.safe_step_scale():.3f}")
    R.export_model(doc, "36_groups.ply", resolution=88, decimate=0.08)


if __name__ == "__main__":
    main()

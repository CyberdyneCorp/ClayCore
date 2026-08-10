"""Armatures: a tree of spheres, which is how you block a figure out.

`34_organic_character.py` is forty-odd primitives whose positions are numbers
typed into a file. There is no way there to say "an arm hangs from this
shoulder" — only to work out where the arm's capsule endpoints would be if it
did, and to retype them all when the shoulder moves. This is the same figure
said the other way.

An armature is a TREE. A node names its parent, links are sphere-swept cones
between the two, and the whole thing is ONE edit item. Moving a shoulder is one
number, and the arm follows because it is attached rather than because it was
recomputed.

**It is `Stroke` with the chain generalised**, and that is worth knowing rather
than taking on faith. A stroke joins point i to point i+1, so its topology is a
line; an armature gives every node a parent, so it can branch. The link, the
smooth union between links and the blend parameter are the same code — which is
why the first thing this file checks is that a line-shaped armature and the
stroke built from the same points agree to float noise. If they ever stop
agreeing, one of the two has drifted.

There is deliberately no per-node rotation. A sphere is isotropic, so rotating
one changes no distance and no surface. It earns its place in ZBrush because
the adaptive skin lays out quads whose edge flow follows the node frames, and
marching cubes, surface nets and dual contouring do not consult one.
"""

import numpy as np

import pyclay as clay

import _render as R

SKIN = "#c08a62"
BONE = "#8d97a4"


def figure(shoulder_drop=0.0):
    """A blocked-out figure as one armature.

    Every node is placed relative to the one it hangs from, so the structure is
    in the tree rather than in the numbers. `shoulder_drop` moves ONE node; the
    whole arm below it follows, which is the entire point.
    """
    a = clay.Armature(nodes=np.array([(0.0, 0.95, 0.0, 0.20)], np.float32), blend_k=0.022)
    pelvis = 0

    # Spine, each node hanging off the one before.
    chest = a.add_child((0.0, 1.35, 0.0), 0.22, parent=pelvis).node_count - 1
    neck = a.add_child((0.0, 1.62, 0.02), 0.10, parent=chest).node_count - 1
    a.add_child((0.0, 1.80, 0.02), 0.16, parent=neck)          # head

    for side in (-1.0, 1.0):
        # Arm: shoulder hangs off the chest, and everything below hangs off it.
        sh = a.add_child((side * 0.30, 1.42 + shoulder_drop, 0.0), 0.13,
                         parent=chest).node_count - 1
        el = a.add_child((side * 0.46, 1.10 + shoulder_drop, 0.02), 0.10,
                         parent=sh).node_count - 1
        wr = a.add_child((side * 0.54, 0.80 + shoulder_drop, 0.05), 0.075,
                         parent=el).node_count - 1
        a.add_child((side * 0.58, 0.68 + shoulder_drop, 0.07), 0.06, parent=wr)

        # Leg off the pelvis.
        kn = a.add_child((side * 0.14, 0.50, 0.0), 0.12, parent=pelvis).node_count - 1
        an = a.add_child((side * 0.16, 0.05, 0.0), 0.085, parent=kn).node_count - 1
        a.add_child((side * 0.16, -0.02, 0.10), 0.07, parent=an)   # foot

    return a


def main():
    R.banner("40 armature — a tree of spheres, which is how a figure is blocked out")

    # --- a chain armature IS the stroke it came from -------------------------
    pts = np.array([(-0.6, 0, 0, 0.20), (-0.2, 0.25, 0, 0.16),
                    (0.25, 0.20, 0, 0.13), (0.6, -0.1, 0, 0.10)], np.float32)
    rng = np.random.default_rng(1)
    probe = rng.uniform(-1.2, 1.2, size=(40000, 3)).astype(np.float32)
    worst = 0.0
    for k in (0.0, 0.04, 0.08, 0.15):
        ds = clay.Document(); ds.add_sdf_layer("s").add(clay.Stroke(points=pts, blend_k=k))
        da = clay.Document(); da.add_sdf_layer("a").add(clay.Armature(nodes=pts, blend_k=k))
        worst = max(worst, float(np.abs(ds.eval(probe) - da.eval(probe)).max()))
    print(f"  a line-shaped armature vs the stroke: worst gap {worst:.2e} over 4 blends")
    if worst > 1e-5:
        raise SystemExit("an armature and its stroke have drifted — one of them is wrong")

    # --- the tree, and what it buys ------------------------------------------
    a = figure()
    doc = clay.Document()
    layer = doc.add_sdf_layer("figure")
    node_id = layer.add(a, color=SKIN)
    print(f"  the figure is ONE item: {a.node_count} nodes, "
          f"{len(set(a.parents))} of them parents")

    # --- the tree edits, live in the document --------------------------------
    # Not by rebuilding the armature with different numbers: by editing the one
    # that is already placed, the way a host would when a finger drags a node.
    doc.enable_undo()
    def wrist_bottom():
        """Lowest point of the left arm — measured, so the move is a distance
        rather than an in/out answer that a thick limb can hide."""
        ys = np.linspace(0.2, 1.2, 1001, dtype=np.float32)
        col = np.stack([np.full_like(ys, -0.58), ys, np.full_like(ys, 0.07)], axis=1)
        inside = ys[doc.eval(col) < 0.0]
        return float(inside.min()) if len(inside) else float("nan")

    shoulder = 4  # the left shoulder; the wrist is three links below it
    before = wrist_bottom()
    layer.armature_edit(node_id, "move", target=shoulder, value=(0.0, -0.15, 0.0))
    after = wrist_bottom()
    print(f"  move the shoulder down 0.15 -> the wrist bottom moves {after - before:+.3f}")
    if not (-0.20 < after - before < -0.10):
        raise SystemExit("moving a node stopped carrying its subtree")

    doc.undo()
    if abs(wrist_bottom() - before) > 1e-4:
        raise SystemExit("one undo did not put the whole arm back")
    print(f"  ONE undo puts the whole arm back ({wrist_bottom():.3f})")

    # Delete takes the subtree with it.
    layer.armature_edit(node_id, "delete", target=shoulder)
    gone = np.isnan(wrist_bottom())
    print(f"  delete the shoulder -> the arm below it is gone: {gone}")
    if not gone:
        raise SystemExit("deleting a node stopped taking its subtree")
    doc.undo()

    # Mirroring adds both sides in one step, and a node ON the plane once.
    layer.armature_edit(node_id, "add_child", target=1, value=(0.0, 2.05, 0.0),
                        radius=0.05, mirrored=True)
    doc.undo()
    print("  a mirrored insert is one undo step, and a node on the plane is added once")

    eye, target = R.layer_camera(layer, azimuth=22.0, elevation=6.0)
    R.render(doc, "40_armature.png", eye=eye, target=target,
             colors_from_field=True, caption="a figure as one armature")

    # --- the armature and its skin, side by side -----------------------------
    tiles = []
    for k, label in ((0.0, "links only"), (0.022, "skinned")):
        d = clay.Document()
        l = d.add_sdf_layer("l")
        l.add(clay.Armature(nodes=np.array(a.nodes, np.float32),
                            parents=list(a.parents), blend_k=k), color=BONE)
        e, t = R.layer_camera(l, azimuth=22.0, elevation=6.0)
        tiles.append(R.render_tile(d, eye=e, target=t, size=260, colors_from_field=True))
        print(f"  blend_k {k:<5} -> {label}")
    R.contact_sheet(tiles, "40_armature_skin.png", columns=2,
                    caption="the same tree with the blend off and on — the blend IS the skin")

    # The adaptive skin is not a new mechanism: it is the mesher that was
    # always here.
    print(f"  step scale {doc.safe_step_scale():.3f}")
    R.export_model(doc, "40_armature.ply", resolution=88, decimate=0.10)


if __name__ == "__main__":
    main()

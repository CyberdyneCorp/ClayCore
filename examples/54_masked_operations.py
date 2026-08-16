"""Masking that gates any operation — including a boolean.

Masks have worked here for a while, but what they gated was **authoring**: a
voxel edit consumes one per cell as it writes, and an SDF edit consumes one when
the stroke engine turns a stroke into items. Neither touches an item already in
the edit list.

So masking covered brushes and nothing else — and the operation an artist most
wants protection from is not a brush at all. It is the next **boolean**. Mask an
ear, cut a hole through the head, and the mask did nothing, because a subtract is
an item in the edit list rather than a stroke passing through the mask. That is
what `docs/sculpt_comparison.md` called the single biggest missing concept.

**A gate is per item, and the tape already had the shape for it.** "Keep the old
field where the mask protects" reads as `mix(d_before, d_after, mask)`, and the
tempting implementation snapshots `d_before` — which is the baked,
un-re-editable thing masking exists to save you from. No snapshot is needed: the
tape *accumulates*, so at the moment an item combines, the accumulator already
holds exactly "everything before this item".

    d = mix(combine(acc, item, op), acc, gate(p))

At full protection that is `acc` bit for bit; with no gate it is the ungated
result bit for bit. Both ends exact, not approximate — a gate that only *nearly*
restored the accumulator would leave a seam along the mask's border.

**A gate is not cheap**, and the table below is the honest version of that
rather than a footnote. Mixing two fields that differ by the item's whole reach,
across a band a tenth of a unit wide, genuinely has a Lipschitz constant in the
tens — so a narrow gate costs an order of magnitude of step scale. Use the widest
gate the shape tolerates.

**The gate carries a distance, not paint**, and this script measures why that
matters. A painted mask is a [0,1] scalar whose steepness is whatever the
artist's brush edge happened to be; the *signed distance* to the masked region
is 1-Lipschitz, so the mix's cost is set by a falloff width **you** choose. What
that costs is that painted softness is re-derived rather than preserved — a real
loss, traded for a bound you can state in advance and a falloff you can change
without repainting.
"""

import numpy as np

import pyclay as clay

import _render as R

BODY = 1.0
# Looking nearly down the channel, so the hole faces the viewer and the mask
# cuts across it. A camera off to the side sees the bar end-on and the whole
# demonstration becomes an unmarked sphere.
EYE, TARGET = (0.75, 0.85, 3.10), (0.0, 0.0, 0.0)


def protect_plus_x(from_x=0.0):
    """A mask over the +X half of the body — the 'ear' being protected.

    It splits the bar down the middle, so the picture is a hole with half of it
    still filled in rather than two spheres you have to compare."""
    m = clay.MaskField(0.04)
    # Generous in y and z: a probe within the gate's FADE of the mask's own
    # edge is only partly protected, which reads as the gate not working when
    # it is the fixture that is too tight.
    m.fill(((from_x, -2.0, -2.0), (2.0, 2.0, 2.0)), 1.0)
    return m


def body_with_channel(gate=None, width=0.15, op=clay.Op.SUBTRACT):
    """A ball with a bar combined into it, the bar optionally gated."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=BODY), color="#c08a5a")
    # Wide enough that a probe either side of the mask's border clears the
    # gate's FADE. At 0.45 the probes sat exactly at the saturation edge and
    # "fully protected" was 99%-protected, which fails an exactness check for
    # a fixture reason rather than a real one.
    bar = clay.Box(size=(0.8, 0.8, 3.0))  # along Z, toward the camera
    if gate is not None:
        bar = bar.gate(gate, width=width)
    layer.add(bar, op=op, color="#7a8fb0")
    return doc


def solid_body():
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Sphere(r=BODY), color="#c08a5a")
    return doc


def main():
    R.banner("54 masked operations — a mask that gates a boolean")

    solid = solid_body()
    ungated = body_with_channel()
    gated = body_with_channel(protect_plus_x())

    # --- the motivating case --------------------------------------------------
    protected = np.array([[0.3, 0.0, 0.0]], dtype=np.float32)
    open_side = np.array([[-0.3, 0.0, 0.0]], dtype=np.float32)
    print("  a bar subtracted through a ball, with the +X half masked:")
    print(f"    {'probe':<22}{'solid':>10}{'ungated':>10}{'gated':>10}")
    for name, p in (("in the hole, +X: masked", protected),
                    ("in the hole, -X: open", open_side)):
        print(f"    {name:<22}{float(solid.eval(p)[0]):>10.4f}"
              f"{float(ungated.eval(p)[0]):>10.4f}{float(gated.eval(p)[0]):>10.4f}")

    if float(gated.eval(protected)[0]) != float(solid.eval(protected)[0]):
        raise SystemExit("a fully protected region must be EXACTLY the field without the item")
    if float(gated.eval(open_side)[0]) != float(ungated.eval(open_side)[0]):
        raise SystemExit("an unprotected region must be EXACTLY the ungated result")
    print("\n    Both ends EXACT, not approximate: the protected side is the solid")
    print("    body bit for bit, and the open side is the ungated cut bit for bit.")

    # --- it gates any op, not just subtract ----------------------------------
    # A gate composes with every combine mode rather than being one — which is
    # the point, since a boolean IS a mode. Each op is probed where it actually
    # acts: adding a thin bar INSIDE a ball changes nothing there.
    print("\n  the same gate, on three different operations:")
    print(f"    {'op':<12}{'probe':<18}{'solid':>9}{'ungated':>9}{'gated':>9}")
    cases = [
        (clay.Op.SUBTRACT, "inside both", (0.3, 0.0, 0.0)),
        (clay.Op.ADD, "outside the ball", (0.3, 0.0, 1.2)),
        (clay.Op.INTERSECT, "outside the bar", (0.3, 0.7, 0.0)),
    ]
    for op, why, probe in cases:
        p = np.array([probe], dtype=np.float32)
        s = float(solid.eval(p)[0])
        u = float(body_with_channel(op=op).eval(p)[0])
        g = float(body_with_channel(protect_plus_x(), op=op).eval(p)[0])
        print(f"    {str(op).split('.')[-1]:<12}{why:<18}{s:>9.4f}{u:>9.4f}{g:>9.4f}")
        if u == s:
            raise SystemExit(f"the fixture for {op} proves nothing — the ungated form matched")
        if g != s:
            raise SystemExit(f"{op} was not gated")
    print("    Every one of them stopped at the mask. A gate lives on the combine")
    print("    RECORD rather than in the mode enum, which is what lets it do that.")

    # --- what a gate costs ----------------------------------------------------
    # Mixing two fields by a spatially varying weight is not a distance, and
    # saying otherwise lets a raymarcher walk through the surface. The weight is
    # a smoothstep of a DISTANCE across the gate's width, so the cost is known
    # in advance — and falls as the width grows.
    print("\n  what a gate costs in step scale:")
    print(f"    {'width':>8}{'step scale':>13}")
    print(f"    {'(none)':>8}{ungated.safe_step_scale():>13.4f}")
    previous = 0.0
    for width in (0.05, 0.1, 0.2, 0.4, 0.8):
        scale = body_with_channel(protect_plus_x(), width=width).safe_step_scale()
        print(f"    {width:>8.2f}{scale:>13.4f}")
        if scale >= ungated.safe_step_scale():
            raise SystemExit("a gate is never free")
        if scale <= previous:
            raise SystemExit("a wider gate must cost less")
        previous = scale
    print("    A gate is NOT cheap, and the table is the honest version of that:")
    print("    even the widest here is a ~7x slowdown, and the narrowest is ~90x.")
    print("    That is not overhead, it is the arithmetic — mixing two fields that")
    print("    differ by the item's whole reach, across a band that narrow, really")
    print("    does have that Lipschitz constant. Prefer the widest gate the shape")
    print("    tolerates, and expect a masked boolean to cost more than a plain one.")
    print("    What the design buys is that the cost is YOURS to set rather than")
    print("    the mask's: the gate carries a 1-Lipschitz DISTANCE, so it follows")
    print("    the width you chose, not how hard the brush edge that painted it was.")

    # --- the pictures ---------------------------------------------------------
    tiles = [
        R.render_array(solid, eye=EYE, target=TARGET, width=250, height=250,
                       colors_from_field=True),
        R.render_array(ungated, eye=EYE, target=TARGET, width=250, height=250,
                       colors_from_field=True),
    ]
    for width in (0.1, 0.3, 0.6):
        tiles.append(R.render_array(body_with_channel(protect_plus_x(), width=width),
                                    eye=EYE, target=TARGET, width=250, height=250,
                                    colors_from_field=True))
    R.contact_sheet(tiles, "54_masked_operations.png", columns=5,
                    caption="solid, bored through, then gated at width 0.10, 0.30, 0.60")

    # --- and it survives the file ---------------------------------------------
    path = R.output_path("54_masked_operations.clayspace")
    gated.save(str(path))
    back = clay.load(str(path))
    rng = np.random.default_rng(54)
    pts = rng.uniform(-1.6, 1.6, size=(512, 3)).astype(np.float32)
    if not np.array_equal(gated.eval(pts), back.eval(pts)):
        raise SystemExit("a gate must survive the file exactly")
    if back.safe_step_scale() != gated.safe_step_scale():
        raise SystemExit("the reloaded bound must match")
    print("\n  saved and reloaded: the field is identical and so is the bound.")
    print("  An older reader loses the GATE rather than the item — the channel")
    print("  is cut all the way through, which is what that build could always")
    print("  have shown, instead of a document it cannot open.")


if __name__ == "__main__":
    main()

"""Transition morphs: blending between two shapes across space.

Unlike a blend, which merges two fields everywhere, a transition interpolates
between them along an axis or radius. That makes them non-local: the weight at
a point depends on where the point is, not just on the two distances, so these
ops report infinite influence and are never culled.
"""

import pyclay as clay

import _render as R


def morph(transition, op):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(1.1, 1.1, 1.1)), color="#38a6cf")
    layer.add(clay.Sphere(r=0.78), op=op, transition=transition, color="#e0574a")
    return doc, layer


def main():
    R.banner("06 transitions — morphing box into sphere")

    # Linear: the morph runs along an axis between two planes.
    tiles = []
    for a, b in [(-0.8, 0.8), (-0.4, 0.4), (-0.1, 0.1)]:
        doc, layer = morph(clay.TransitionLinear(a=(0, a, 0), b=(0, b, 0)),
                           clay.Op.TRANSITION_LINEAR)
        tiles.append(R.render_tile(doc, eye=(2.6, 1.9, 3.2), target=(0, 0, 0), size=180))
    R.contact_sheet(tiles, "06_transition_linear.png", columns=3,
                    caption="linear morph, widening to sharpening band")

    # Easing changes how the weight ramps across that band.
    tiles = []
    for ease in (0, 3, 9, 17):
        doc, layer = morph(clay.TransitionLinear(a=(0, -0.9, 0), b=(0, 0.9, 0), ease=ease),
                           clay.Op.TRANSITION_LINEAR)
        tiles.append(R.render_tile(doc, eye=(2.6, 1.9, 3.2), target=(0, 0, 0), size=180))
    R.contact_sheet(tiles, "06_transition_easing.png", columns=4,
                    caption="the same morph under four easing curves")

    # Radial: the morph runs outward from a centre.
    tiles = []
    for r0, r1 in [(0.0, 1.4), (0.4, 1.0), (0.7, 0.9)]:
        doc, layer = morph(clay.TransitionRadial(r0=r0, r1=r1), clay.Op.TRANSITION_RADIAL)
        tiles.append(R.render_tile(doc, eye=(2.6, 1.9, 3.2), target=(0, 0, 0), size=180))
    R.contact_sheet(tiles, "06_transition_radial.png", columns=3,
                    caption="radial morph, widening to sharpening shell")

    # A morph is non-local: it has no finite influence bound, which is what
    # keeps per-brick culling from dropping it.
    doc, layer = morph(clay.TransitionLinear(a=(0, -0.9, 0), b=(0, 0.9, 0)),
                       clay.Op.TRANSITION_LINEAR)
    R.render(doc, "06_transition_hero.png", eye=(2.9, 2.0, 3.4), target=(0, 0, 0),
             colors_from_field=True, caption="colour morphs across the band too")
    R.export_model(doc, "06_transition.ply", resolution=64)


if __name__ == "__main__":
    main()

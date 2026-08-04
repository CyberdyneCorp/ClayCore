"""Blend kinds and extended combine modes, side by side.

Each tile is the *same* two shapes — a sphere and an overlapping box — so the
only thing that varies is how they are combined. That makes the differences
between quadratic, cubic, circular and chamfer blending legible, and shows
what the eight extended modes actually do to a seam.
"""

import pyclay as clay

import _render as R


def pair(op=clay.Op.ADD, blend=None, rounding=0.0):
    """A sphere plus an overlapping box, combined however the caller says."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.8))
    layer.add(clay.Box(size=(0.9, 0.9, 0.9), position=(0.65, 0.35, 0.2)),
              op=op, blend=blend, rounding=rounding)
    return doc, layer


BLENDS = [
    ("hard union", clay.Op.ADD, None),
    ("smooth k=0.15", clay.Op.ADD, clay.Smooth(0.15)),
    ("smooth k=0.35", clay.Op.ADD, clay.Smooth(0.35)),
    ("cubic k=0.35", clay.Op.ADD, clay.Cubic(0.35)),
    ("circular k=0.35", clay.Op.ADD, clay.Circular(0.35)),
    ("chamfer k=0.3", clay.Op.ADD, clay.Chamfer(0.3)),
    ("subtract", clay.Op.SUBTRACT, None),
    ("subtract smooth", clay.Op.SUBTRACT, clay.Smooth(0.2)),
    ("intersect", clay.Op.INTERSECT, None),
    ("intersect smooth", clay.Op.INTERSECT, clay.Smooth(0.2)),
]

# The extended modes all shape the seam rather than the volumes.
EXTENDED = [
    ("groove", clay.Op.GROOVE),
    ("tongue", clay.Op.TONGUE),
    ("pipe", clay.Op.PIPE),
    ("engrave", clay.Op.ENGRAVE),
    ("emboss", clay.Op.EMBOSS),
    ("inset", clay.Op.INSET),
    ("shell", clay.Op.SHELL),
    ("replace", clay.Op.REPLACE),
]


def main():
    R.banner("02 blends — every combine mode on the same two shapes")

    tiles = []
    for name, op, blend in BLENDS:
        doc, layer = pair(op=op, blend=blend)
        tiles.append(R.render_tile(doc, layer=layer, size=160))
    R.contact_sheet(tiles, "02_blends.png", columns=5,
                    caption=", ".join(n for n, _, _ in BLENDS))

    tiles = []
    for name, op in EXTENDED:
        # The extended modes need a blend radius to have a seam to work on.
        doc, layer = pair(op=op, blend=clay.Smooth(0.25), rounding=0.06)
        tiles.append(R.render_tile(doc, layer=layer, size=160))
    R.contact_sheet(tiles, "02_blends_extended.png", columns=4,
                    caption=", ".join(n for n, _ in EXTENDED))

    # Colour blends across a smooth seam too — PAINT recolours without
    # changing the field.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.9), color="#38a6cf")
    layer.add(clay.Sphere(r=0.7, position=(0.9, 0.2, 0.1)),
              blend=clay.Smooth(0.35), color="#e0574a")
    layer.add(clay.Sphere(r=0.5, position=(-0.6, 0.5, 0.4)),
              op=clay.Op.PAINT, blend=clay.Smooth(0.3), color="#f2c14e")
    eye, target = R.layer_camera(layer)
    R.render(doc, "02_blend_colors.png", eye=eye, target=target,
             colors_from_field=True, caption="smooth colour blending + PAINT")


if __name__ == "__main__":
    main()

"""Consolidation keeps the colours it bakes.

Consolidating a layer is advertised as changing what it COSTS rather than what
it looks like: the surface is preserved at the resolution you chose, and the
chain of parametric edits behind it is replaced by samples.

Colour did not survive that, and could not. A sampled volume had nowhere to put
one — a Node carries exactly one colour — so a consolidated character came back
in a single colour, losing the distinction between skin and armour. That is a
change in what the layer looks like, from an operation that promised not to be.

A volume now carries an optional colour per sample, so the bake writes what it
used to discard. The two renders below are the same layer, consolidated, before
and after: the parametric original, and the baked result that keeps its
colours.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.02
BAND = 0.06
EYE = (2.2, 1.4, 2.6)
TARGET = (0.0, 0.0, 0.0)


def figure():
    """A layer whose colours are the point: three parts, three colours."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("figure")
    layer.add(clay.Sphere(r=0.42), color="#c8a882")                      # head
    layer.add(clay.Capsule(a=(0, -0.95, 0), b=(0, -0.30, 0), r=0.34),
              color="#3f6ea8", blend=clay.Smooth(0.10))                  # coat
    layer.add(clay.Box(size=(0.62, 0.10, 0.30), position=(0, -0.34, 0)),
              color="#b8483c", blend=clay.Smooth(0.05))                  # sash
    return doc, layer


def main():
    doc, layer = figure()

    # What the layer costs before, so the picture is not the only claim.
    quote = layer.consolidation_cost(cell=CELL, band=BAND)
    print(f"  the parametric layer consolidates to {quote['megabytes']:.2f} MB")

    probes = np.array([[0, 0.1, 0.30], [0, -0.62, 0.30], [0, -0.34, 0.26]],
                      dtype=np.float32)
    before = doc.colors(probes)
    print(f"  before: head={_hex(before[0])} coat={_hex(before[1])} sash={_hex(before[2])}")

    tiles = [R.render_array(doc, eye=EYE, target=TARGET, width=250, height=250,
                            colors_from_field=True)]

    layer.consolidate(cell=CELL, band=BAND)
    state = layer.consolidation_state
    print(f"  consolidated: {state is not None}"
          + (f", {state['megabytes']:.2f} MB of samples" if state else ""))

    after = doc.colors(probes)
    print(f"  after:  head={_hex(after[0])} coat={_hex(after[1])} sash={_hex(after[2])}")

    # The claim of this example, as an assertion and not only a picture: each
    # part still reports its OWN colour after the bake. Within a tolerance,
    # because the colour is stored to 8 bits a channel and read back
    # interpolated between samples.
    for i, name in enumerate(("head", "coat", "sash")):
        assert np.allclose(before[i], after[i], atol=0.10), \
            f"{name} changed colour through the bake: {before[i]} -> {after[i]}"

    tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=250, height=250,
                                colors_from_field=True))
    R.contact_sheet(tiles, "43_consolidation_keeps_colour.png", columns=2)
    print("  wrote output/43_consolidation_keeps_colour.png  "
          "(parametric | consolidated — the same colours)")


def _hex(c):
    return "#{:02x}{:02x}{:02x}".format(*(int(round(v * 255)) for v in c))


if __name__ == "__main__":
    main()

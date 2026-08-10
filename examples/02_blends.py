"""Blend kinds and extended combine modes, side by side.

Each tile is the *same* two shapes — a sphere and an overlapping box — so the
only thing that varies is how they are combined. That makes the differences
between quadratic, cubic, circular and chamfer blending legible, and shows
what the eight extended modes actually do to a seam.
"""

import numpy as np

import pyclay as clay

import _render as R


def op_name(op):
    """The enum member's name, so coverage is checked against the enum itself
    rather than against a list of strings that can drift from it."""
    return str(op).rsplit(".", 1)[-1]


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

# An op covered by a different example is named here rather than exempted
# silently, so adding one stays a decision on the record — the same rule
# 01_primitives.py applies to primitive classes.
COVERED_ELSEWHERE = {
    "RELIEF": ("25_relief.py", "the item is a region, so it needs a surface to move"),
    "INCISE": ("25_relief.py", "relief's exact inverse, measured against it there"),
    "TRANSITION_LINEAR": ("06_transitions.py", "a morph needs a sweep to be legible"),
    "TRANSITION_RADIAL": ("06_transitions.py", "likewise"),
    "INLINE": ("37_groups.py", "a group op: it has no meaning on a lone pair"),
}


def sample(doc, n=4000, seed=7):
    """A fixed cloud of field samples — the tile's numbers rather than its pixels."""
    rng = np.random.default_rng(seed)
    pts = rng.uniform(-1.6, 1.6, size=(n, 3)).astype(np.float32)
    return doc.eval(pts)


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

    # --- what the pictures cannot check ---------------------------------------
    # A contact sheet proves every mode RAN. It does not prove they did
    # different things: two ops that quietly resolve to the same field give two
    # identical tiles, and identical tiles are exactly what this page looks
    # like when it is right. So measure the fields.
    fields = {}
    for name, op, blend in BLENDS:
        fields[name] = sample(pair(op=op, blend=blend)[0])
    for name, op in EXTENDED:
        fields[name] = sample(pair(op=op, blend=clay.Smooth(0.25), rounding=0.06)[0])

    same = [(a, b) for i, a in enumerate(fields) for b in list(fields)[i + 1:]
            if np.allclose(fields[a], fields[b], atol=1e-6)]
    if same:
        raise SystemExit(f"these combine modes produce the same field: {same}")
    print(f"  {len(fields)} combine modes, all {len(fields)} fields distinct")

    # PAINT is the one op that must NOT move the surface: it recolours a
    # region and leaves the field alone. That is the invariant the tile above
    # is drawn to show and cannot demonstrate.
    base = clay.Document()
    bl = base.add_sdf_layer("l")
    bl.add(clay.Sphere(r=0.9))
    painted = clay.Document()
    pl = painted.add_sdf_layer("l")
    pl.add(clay.Sphere(r=0.9))
    pl.add(clay.Sphere(r=0.5, position=(0.6, 0, 0)), op=clay.Op.PAINT,
           blend=clay.Smooth(0.3), color="#f2c14e")
    moved = float(np.abs(sample(base) - sample(painted)).max())
    if moved > 1e-6:
        raise SystemExit(f"PAINT moved the surface by {moved:.2e} — it must only recolour")
    print(f"  PAINT left the field identical (max |delta| {moved:.1e})")

    # A wider blend radius makes a fuller seam, so the union's material grows
    # with k. If the radius ever stopped reaching the field this ordering is
    # what breaks, and it breaks before anything is visible in a tile.
    inside = [float((sample(pair(blend=clay.Smooth(k))[0]) < 0).sum())
              for k in (0.0, 0.15, 0.35)]
    if not (inside[0] < inside[1] < inside[2]):
        raise SystemExit(f"a wider smooth blend stopped adding material: {inside}")
    print(f"  smooth k=0/0.15/0.35 -> {inside[0]:.0f}/{inside[1]:.0f}/{inside[2]:.0f} samples inside")

    # Coverage: an op with no example anywhere is a gap in the gallery.
    shown = {op_name(op) for _, op, _ in BLENDS} | {op_name(op) for _, op in EXTENDED}
    shown.add("PAINT")
    missing = {o for o in dir(clay.Op) if not o.startswith("_")} - shown - set(COVERED_ELSEWHERE)
    if missing:
        raise SystemExit(f"combine ops with no example: {sorted(missing)}")
    for name, (where, why) in sorted(COVERED_ELSEWHERE.items()):
        if name in shown:
            raise SystemExit(f"{name} has a tile here now — drop it from COVERED_ELSEWHERE")
        print(f"  {name} is covered by {where}: {why}")
    print(f"  covered all {len(shown) + len(COVERED_ELSEWHERE)} combine ops")


if __name__ == "__main__":
    main()

"""Every primitive the module exposes, rendered as a contact sheet.

This is the example that keeps the gallery honest: it enumerates `pyclay`'s
primitive classes rather than a hand-written list, so a primitive added to the
bindings without an example here shows up as a missing tile.
"""

import pyclay as clay

import _render as R

# One representative instance per primitive class. The sizes are chosen so
# each shape roughly fills its tile.
PRIMITIVES = [
    ("Sphere", clay.Sphere(r=0.8)),
    ("Box", clay.Box(size=(1.2, 1.2, 1.2))),
    ("RoundBox", clay.RoundBox(size=(1.1, 1.1, 1.1), r=0.18)),
    ("Torus", clay.Torus(R=0.75, r=0.28)),
    ("Capsule", clay.Capsule(a=(-0.45, -0.3, 0), b=(0.45, 0.4, 0), r=0.32)),
    ("Cylinder", clay.Cylinder(r=0.6, h=0.7)),
    ("Cone", clay.Cone(h=0.8, r1=0.7, r2=0.15)),
    ("RoundCone", clay.RoundCone(r1=0.5, r2=0.2, h=0.85)),
    ("Ellipsoid", clay.Ellipsoid(r=(0.9, 0.5, 0.65))),
    ("Octahedron", clay.Octahedron(s=0.9)),
    ("HexPrism", clay.HexPrism(hx=0.7, hy=0.45)),
    ("Pyramid", clay.Pyramid(h=1.1)),
    ("CappedTorus", clay.CappedTorus(aperture=1.2, ra=0.75, rb=0.24)),
    ("Link", clay.Link(length=0.35, r1=0.55, r2=0.2)),
    ("ExactCone", clay.ExactCone(half_angle=0.55, h=1.0)),
    ("CutSphere", clay.CutSphere(r=0.9, h=0.25)),
    ("CutHollowSphere", clay.CutHollowSphere(r=0.9, h=0.2, t=0.07)),
    ("SolidAngle", clay.SolidAngle(angle=0.8, ra=0.9)),
    ("Tetrahedron", clay.Tetrahedron(r=0.85)),
    ("Dodecahedron", clay.Dodecahedron(r=0.75)),
    ("Icosahedron", clay.Icosahedron(r=0.75)),
    ("TriPrism", clay.TriPrism(hx=0.8, hy=0.45)),
    ("OctahedronCheap", clay.OctahedronCheap(s=0.9)),
    ("LNormSphere", clay.LNormSphere(r=0.85, n=4.0)),
    # Lifts: a 2D profile swept into 3D.
    ("Extrude", clay.Extrude(profile=clay.Profile.hexagon(r=0.7), half_depth=0.35)),
    ("Revolve", clay.Revolve(profile=clay.Profile.circle(r=0.3), offset=0.7)),
    # A swept chain of spheres — one edit, not one per segment.
    ("Stroke", clay.Stroke(points=[(-0.8, -0.3, 0, 0.3), (0, 0.4, 0, 0.22),
                                   (0.8, -0.3, 0, 0.3)], blend_k=0.05)),
]

# The two unbounded primitives cannot be framed from their own bounds (they
# have none), so they get a fixed camera and something finite to cut into.
UNBOUNDED = [
    ("Plane", clay.Plane(normal=(0.2, 1.0, 0.15), offset=0.0)),
    ("CylinderInfinite", clay.CylinderInfinite(r=0.45)),
]


# Prim classes whose example lives elsewhere because a contact-sheet tile
# cannot show them: Cut resolves against a frame and a region, so a tile with
# neither would be showing an extruded box and calling it a cut.
COVERED_ELSEWHERE = {
    "Cut": ("14_cut.py", "it resolves against a frame and a region, so a tile "
                         "with neither would be an extruded box"),
    "Loft": ("16_loft.py", "it needs two or more profiles, and one tile of one "
                           "would be an extrusion"),
}


def primitive_classes():
    """Every PyPrim subclass the module exposes, by name."""
    out = set()
    for name in dir(clay):
        obj = getattr(clay, name)
        if isinstance(obj, type) and issubclass(obj, clay.Prim) and obj is not clay.Prim:
            out.add(name)
    return out


def main():
    R.banner("01 primitives — every shape in the kernel set")

    tiles = []
    for name, prim in PRIMITIVES:
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(prim)
        tiles.append(R.render_tile(doc, layer=layer, size=160))
    R.contact_sheet(tiles, "01_primitives.png", columns=6,
                    caption=f"{len(tiles)} bounded primitives")

    # Unbounded ones, shown by what they carve out of a sphere.
    # These have no finite bounds to frame from, so the camera is explicit.
    unbounded_tiles = []
    for name, prim in UNBOUNDED:
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Sphere(r=1.0))
        layer.add(prim, op=clay.Op.SUBTRACT)
        unbounded_tiles.append(
            R.render_tile(doc, eye=(2.1, 1.6, 2.6), target=(0, 0, 0), size=160)
        )
    R.contact_sheet(unbounded_tiles, "01_primitives_unbounded.png", columns=2,
                    caption="plane and infinite cylinder subtracted from a sphere")

    # Coverage: a primitive class with no example anywhere is a gap in the
    # gallery. A class covered by a different example is named here rather
    # than exempted silently, so adding one stays a decision on the record.
    shown = {name for name, _ in PRIMITIVES} | {name for name, _ in UNBOUNDED}
    missing = primitive_classes() - shown - set(COVERED_ELSEWHERE)
    if missing:
        raise SystemExit(f"primitive classes with no example: {sorted(missing)}")
    for name, (where, why) in sorted(COVERED_ELSEWHERE.items()):
        if name in shown:
            raise SystemExit(f"{name} has a tile here now — drop it from COVERED_ELSEWHERE")
        print(f"  {name} is covered by {where}: {why}")
    print(f"  covered all {len(shown)} primitive classes")

    # A single primitive is still a document: mesh and export one.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Icosahedron(r=0.9))
    R.export_model(doc, "01_icosahedron.ply")


if __name__ == "__main__":
    main()

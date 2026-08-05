"""Deformers: twist, bend, taper and displace, including chained order.

Deformers warp the point before the primitive is evaluated, so they are
metric breakers — the field stops being a true distance function and the
library tracks a Lipschitz bound instead. Each tile prints the resulting safe
step scale, which is the number sphere tracing has to slow down by.
"""

import pyclay as clay

import _render as R


def deformed(make):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(make())
    return doc, layer


CASES = [
    ("undeformed", lambda: clay.Box(size=(0.7, 1.8, 0.7))),
    ("twist 1.5", lambda: clay.Box(size=(0.7, 1.8, 0.7)).twist(1.5)),
    ("twist 3.0", lambda: clay.Box(size=(0.7, 1.8, 0.7)).twist(3.0)),
    ("bend 0.8", lambda: clay.Box(size=(0.7, 1.8, 0.7)).bend(0.8)),
    ("taper", lambda: clay.Box(size=(0.9, 1.8, 0.9)).taper(
        y0=-0.9, y1=0.9, s0=1.0, s1=0.25)),
    ("displace", lambda: clay.Sphere(r=0.9).displace(amplitude=0.08, frequency=7.0)),
    # wrap_around bends a flat slab around a cylinder — the deformer that
    # cannot be faked with the others. The interval fixes the radius.
    ("wrap_around", lambda: clay.Box(size=(6.283, 0.35, 1.2)).wrap_around(-3.1416, 3.1416)),
    # elongate inserts flat sections: a sphere becomes a capsule, and unlike
    # every other deformer it is exact, so it costs nothing in step scale.
    ("elongate", lambda: clay.Sphere(r=0.45).elongate((0.8, 0.0, 0.0))),
    # the ramped bends: displacement eased across a span, not a rotation
    ("bend_linear", lambda: clay.Box(size=(0.5, 1.8, 0.5)).bend_linear(
        a=(0, -0.9, 0), b=(0, 0.9, 0), v=(0.9, 0, 0), ease=3)),
    ("bend_radial", lambda: clay.Cylinder(r=1.0, h=0.12).bend_radial(
        r0=0.1, r1=1.0, dz=0.7, ease=5)),
    # elongate_axis stretches an asymmetric primitive, which elongate cannot
    ("elongate_axis", lambda: clay.Cone(h=0.6, r1=0.5, r2=0.1).elongate_axis((0.7, 0, 0))),
    # The region deformers are the only ones with finite support: outside the
    # radius the field is untouched, which is what makes them behave like a
    # sculpting brush rather than a whole-item modifier.
    ("grab", lambda: clay.Sphere(r=0.8).grab(
        center=(0.8, 0, 0), radius=0.7, displacement=(0.5, 0.25, 0), ease=3)),
    # pose_line ramps the rotation along a segment, which is how a limb tapers
    ("pose_line", lambda: clay.Capsule(a=(0, -0.9, 0), b=(0, 0.9, 0), r=0.22).pose_line(
        a=(0, -0.9, 0), b=(0, 0.9, 0), axis=(0, 0, 1), angle=1.1, ease=3)),
    ("pose", lambda: clay.Cylinder(r=0.25, h=0.9).pose(
        center=(0, 0.7, 0), radius=0.9, axis=(0, 0, 1), angle=1.0, ease=2)),
    # Chains apply in authoring order — twist then bend is not bend then twist.
    ("twist then bend", lambda: clay.Box(size=(0.7, 1.8, 0.7)).twist(2.0).bend(0.6)),
    ("bend then twist", lambda: clay.Box(size=(0.7, 1.8, 0.7)).bend(0.6).twist(2.0)),
]


def main():
    R.banner("03 deformers — domain warps and what they cost")

    tiles = []
    for name, make in CASES:
        doc, layer = deformed(make)
        step = doc.safe_step_scale()
        print(f"  {name:18s} safe step scale {step:.3f}")
        tiles.append(R.render_tile(doc, layer=layer, size=160))
    R.contact_sheet(tiles, "03_deformers.png", columns=4,
                    caption=", ".join(n for n, _ in CASES))

    # A displaced sphere makes a decent rock; mesh and export one.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0).displace(amplitude=0.12, frequency=5.0),
              color="#8a7f6d")
    eye, target = R.layer_camera(layer)
    R.render(doc, "03_displaced.png", eye=eye, target=target,
             colors_from_field=True, caption="displacement as surface detail")
    R.export_model(doc, "03_displaced.ply", resolution=64)


if __name__ == "__main__":
    main()

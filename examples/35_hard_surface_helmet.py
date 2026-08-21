"""Hard-surface sculpting: a layered combat helmet.

The opposite discipline to the organic figure next door. There, a smooth blend
fuses primitives into one body and its radius is chosen to be *invisible*.
Here the seam IS the design: a hard-surface model is read almost entirely
through plate edges, panel gaps and cavities.

The structural idea, and the thing that makes this look built rather than
grown: **one layer per armour plate.** A layer is its own field and the
document unions them, so a subtraction inside a plate's layer trims THAT plate
and nothing else. On a single accumulated field it could not — an op applies to
everything already there, so a cut meant for one panel would saw through the
whole helmet. Plates therefore get carved in isolation and assembled by the
document, which is how the real object is made too.

That is the LAYER technique, and it is not the only one: `37_groups.py` builds
the same plate as a **group** on one shared layer. A group is the sharper tool
of the two — it rejoins the assembly with any op and any blend, where layers
only ever union hard — and it leaves a layer meaning what it means everywhere
else. This file keeps the layer form deliberately, as the technique it came
from; read 36 for the group form and the difference between them.

Each plate is a `CutHollowSphere` — a spherical shell with real wall thickness,
not a surface — riding a few millimetres proud of a dark under-shell. The gaps
between plates are not modelled at all: they are where the under-shell shows
through, which is why the panel lines read as depth rather than as scratches.

Everything shares one `elongate_axis`, so the whole assembly stretches from a
sphere into an ovoid skull while the plates stay conformal to what is under
them.

Scope: a technique showcase, not a production asset. The plate structure and
the seam vocabulary are the point; the detail density of a hand-sculpted
helmet is not reachable from a few dozen scripted primitives.
"""

import math

import numpy as np

import pyclay as clay

import _render as R

# One elongation, applied to every shell, turns a sphere into a skull: longer
# front-to-back than it is wide. Applied identically so plates stay conformal.
ELONG = (0.0, 0.06, 0.20)

R_UNDER = 0.505          # dark under-shell, well inside the plates
R_PLATE = 0.600          # plates ride ~0.1 proud of it, so each edge steps
T_PLATE = 0.055          # wall thickness
FLIP = ((1, 0, 0), math.pi)   # a bowl becomes a dome

UNDER = "#474e57"
PLATE_A = "#767e88"
PLATE_B = "#646c76"
PLATE_C = "#565d66"
TRIM = "#3d434b"
GLASS = "#3f7f9c"
RUBBER = "#25282d"
BRASS = "#9c7f47"
SKIN = "#8a7566"


# A bowl opens +Y; these rotations turn it into a cap whose MATERIAL faces the
# named direction, which is how each plate is aimed at the skull under it.
CAP_UP = ((1, 0, 0), math.pi)
CAP_FWD = ((1, 0, 0), -math.pi / 2)
CAP_BACK = ((1, 0, 0), math.pi / 2)
CAP_SIDE = ((0, 0, 1), math.pi / 2)


def cap(centre, r, h, t, rot):
    """A plate: a thick shell cap centred on its OWN point, not the skull's.

    This is the whole difference between armour and a smooth ball. Concentric
    shells hide one another — the outer simply covers the inner and every edge
    lies tangent to the surface, so nothing steps. Give each plate its own
    centre and it intersects what is under it in a real circle: a raised edge
    on one side, an undercut on the other, and a gap where it does not reach.
    """
    return clay.CutHollowSphere(r=r, h=h, t=t, position=centre,
                                rotation_axis_angle=rot).elongate_axis(ELONG)


def box(size, position, rot=None):
    return clay.Box(size=size, position=position, rotation_axis_angle=rot)


def plate(doc, name, prim, cuts, color, mirror=False):
    """One armour plate, trimmed in its own layer so the cuts stay local."""
    layer = doc.add_sdf_layer(name)
    if mirror:
        layer.mirror("x")
    layer.add(prim, color=color)
    for cut in cuts:
        layer.add(cut, op=clay.Op.SUBTRACT, blend=clay.Chamfer(0.012))
    return layer


def aperture():
    """The visor opening. Every plate crossing the face subtracts THIS.

    A layer-local cut only trims its own plate, which is the point — but it
    also means the opening has to be spelled once and applied to each plate
    that reaches the face, or the first one that does covers it back up.
    """
    # NOTE the convention, which is a genuine footgun: Box/RoundBox `size` is a
    # FULL extent (the surface sits at size/2) and `r` rounds INWARD, while
    # Sphere/Ellipsoid/Cylinder take a radius. Sized as a half-extent this cut
    # spanned z 0.25-0.67 and the shell wall is at z 0.765, so it silently
    # missed the helmet entirely.
    return clay.RoundBox(size=(0.66, 0.30, 0.90), r=0.06,
                         position=(0, -0.19, 0.62))


def under_shell(doc):
    """The dark core. Every panel gap is where THIS shows through."""
    layer = doc.add_sdf_layer("under")
    layer.add(cap((0, 0, 0), 0.500, 0.30, 0.075, CAP_UP), color=UNDER)
    layer.add(aperture(), op=clay.Op.SUBTRACT, blend=clay.Chamfer(0.02))
    return layer


def crown(doc):
    """The top plate, riding proud and falling away at the sides."""
    return plate(doc, "crown", cap((0, 0.030, -0.015), 0.532, 0.16, 0.050, CAP_UP), [
        box((1.8, 1.0, 1.8), (0, -0.62, 0)),
        box((1.8, 1.8, 0.8), (0, 0, -1.10)),
        aperture(),
    ], PLATE_A)


def crown_ridge(doc):
    """A raised keel down the centre line."""
    return plate(doc, "ridge", cap((0, 0.052, -0.015), 0.545, 0.14, 0.042, CAP_UP), [
        box((1.8, 1.2, 1.8), (0, -0.74, 0)),
        box((0.76, 1.8, 1.8), (0.85, 0, 0)),
        box((0.76, 1.8, 1.8), (-0.85, 0, 0)),
        box((1.8, 1.8, 0.7), (0, 0, -1.06)),
    ], PLATE_B)


def temple_plates(doc):
    """Side panels carrying the louvers. Own centre, pushed outboard."""
    layer = plate(doc, "temples",
                  cap((0.040, -0.03, 0.01), 0.536, 0.12, 0.048, CAP_SIDE), [
                      box((1.8, 1.0, 1.8), (0, -0.72, 0)),
                      box((1.8, 0.9, 1.8), (0, 0.92, 0)),
                      box((1.8, 1.8, 0.7), (0, 0, -1.02)),
                      box((1.8, 1.8, 0.6), (0, 0, 1.10)),
                      aperture(),
                  ], PLATE_B, mirror=True)
    # Louvers cut clean through the plate: one slot, repeated by the tape.
    layer.add(clay.RoundBox(size=(0.34, 0.016, 0.11), r=0.011,
                            position=(0.42, 0.10, 0.04))
              .repeat_grid(spacing=0.058, counts=(0, 3, 0)),
              op=clay.Op.SUBTRACT, blend=clay.Chamfer(0.009))
    return layer


def occiput(doc):
    """The rear plate, overlapping the crown from behind."""
    return plate(doc, "occiput", cap((0, 0.01, -0.045), 0.534, 0.10, 0.048, CAP_BACK), [
        box((1.8, 1.0, 1.8), (0, -0.74, 0)),
        box((1.8, 0.8, 1.8), (0, 0.90, 0)),
    ], PLATE_C)


def brow(doc):
    """The visor housing: the piece that juts and casts the shadow."""
    layer = plate(doc, "brow", cap((0, -0.020, 0.048), 0.540, 0.12, 0.055, CAP_FWD), [
        box((1.8, 0.90, 1.8), (0, -0.47, 0)),   # everything below the brow line
        box((1.8, 0.80, 1.8), (0, 0.92, 0)),
        box((1.8, 1.8, 1.2), (0, 0, -1.02)),
        aperture(),
    ], PLATE_A, mirror=True)
    # A blade sweeping back over each temple.
    layer.add(clay.RoundBox(size=(0.055, 0.030, 0.24), r=0.022,
                            position=(0.33, 0.045, 0.26),
                            rotation_axis_angle=((0, 1, 0), math.radians(-24))),
              blend=clay.Chamfer(0.016), color=PLATE_C)
    return layer


def cheek_and_jaw(doc):
    """Lower plates, wrapping under the ear module."""
    layer = doc.add_sdf_layer("jaw")
    layer.mirror("x")
    layer.add(clay.RoundBox(size=(0.15, 0.34, 0.30), r=0.04,
                            position=(0.345, -0.30, 0.30),
                            rotation_axis_angle=((0, 0, 1), math.radians(-9))),
              blend=clay.Chamfer(0.03), color=PLATE_C)
    layer.add(clay.RoundBox(size=(0.11, 0.26, 0.38), r=0.035,
                            position=(0.395, -0.34, -0.06)),
              blend=clay.Chamfer(0.03), color=PLATE_B)
    layer.add(clay.Capsule(a=(-0.24, -0.42, 0.34), b=(0.24, -0.42, 0.34), r=0.030),
              blend=clay.Chamfer(0.02), color=TRIM)
    return layer


def ear_module(doc):
    """The side unit: housing, bezel, lens, radial fins, bolt ring."""
    layer = doc.add_sdf_layer("ear")
    layer.mirror("x")
    axis = ((0, 0, 1), math.pi / 2)
    layer.add(clay.Cylinder(r=0.175, h=0.075, position=(0.565, -0.12, 0.02),
                            rotation_axis_angle=axis),
              blend=clay.Chamfer(0.035), color=PLATE_C)
    layer.add(clay.Torus(R=0.140, r=0.030, position=(0.630, -0.12, 0.02),
                         rotation_axis_angle=axis),
              blend=clay.Chamfer(0.018), color=TRIM)
    layer.add(clay.Torus(R=0.090, r=0.022, position=(0.650, -0.12, 0.02),
                         rotation_axis_angle=axis),
              blend=clay.Chamfer(0.014), color=TRIM)
    layer.add(clay.Cylinder(r=0.066, h=0.035, position=(0.660, -0.12, 0.02),
                            rotation_axis_angle=axis),
              blend=clay.Smooth(0.010), color=GLASS)
    layer.add(clay.Box(size=(0.022, 0.060, 0.016),
                       position=(0.645, -0.012, 0.02))
              .repeat_radial(count=8, offset=0.0),
              op=clay.Op.ENGRAVE, blend=clay.Smooth(0.013), rounding=0.015)
    layer.add(clay.Sphere(r=0.018, position=(0.595, 0.048, 0.02))
              .repeat_radial(count=10, offset=0.0),
              op=clay.Op.EMBOSS, blend=clay.Smooth(0.013), rounding=0.016,
              color=BRASS)
    return layer


def cable(doc):
    """A ribbed cable from the ear module down to the collar."""
    layer = doc.add_sdf_layer("cable")
    layer.mirror("x")
    pts = [(0.560, -0.28, -0.02), (0.615, -0.42, -0.14), (0.585, -0.57, -0.28),
           (0.480, -0.68, -0.40), (0.330, -0.74, -0.48)]
    guide = np.array([(x, y, z, 0.0) for x, y, z in pts], dtype=np.float32)
    layer.add(clay.Swept(guide,
                         [clay.Profile.circle(r=0.040),
                          clay.Profile.circle(r=0.032)],
                         types="spline", tolerance=0.010),
              color=RUBBER)
    layer.add(clay.Torus(R=0.042, r=0.012, position=(0.590, -0.46, -0.18),
                         rotation_axis_angle=((1, 0, 0), math.radians(48)))
              .repeat_grid(spacing=0.052, counts=(0, 1, 0)),
              blend=clay.Smooth(0.008), color=TRIM)
    layer.add(clay.Sphere(r=0.068, position=(0.550, -0.27, -0.01)),
              op=clay.Op.PIPE, blend=clay.Smooth(0.028), color=RUBBER)
    return layer


def face(doc):
    """A head inside the shell, so the helmet has something to be worn by."""
    layer = doc.add_sdf_layer("head")
    layer.add(clay.Ellipsoid(r=(0.270, 0.32, 0.32), position=(0, -0.15, 0.055)),
              color=SKIN)
    layer.add(clay.Ellipsoid(r=(0.185, 0.145, 0.19), position=(0, -0.33, 0.215)),
              blend=clay.Smooth(0.09), color=SKIN)
    layer.add(clay.Ellipsoid(r=(0.15, 0.033, 0.06), position=(0, -0.14, 0.28)),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.020), rounding=0.05)
    layer.add(clay.Capsule(a=(0, -0.18, 0.28), b=(0, -0.29, 0.31), r=0.028),
              blend=clay.Smooth(0.030), color=SKIN)
    layer.add(clay.Capsule(a=(-0.085, -0.34, 0.28), b=(0.085, -0.34, 0.28), r=0.011),
              op=clay.Op.INCISE, blend=clay.Smooth(0.014), rounding=0.03)
    layer.add(clay.Capsule(a=(0, -0.44, 0.02), b=(0, -0.80, -0.02), r=0.19),
              blend=clay.Smooth(0.07), color=SKIN)
    return layer


STAGES = [
    ("under-shell", "the dark core every gap shows", [under_shell, face]),
    ("crown", "a plate on its OWN centre, so its edge steps", [crown, crown_ridge]),
    ("temples", "side plates + louvers, occiput behind", [temple_plates, occiput]),
    ("brow + jaw", "the jutting visor housing", [brow, cheek_and_jaw]),
    ("finish", "ear module, ribbed cable", [ear_module, cable]),
]


def build(stage):
    doc = clay.Document()
    for i, (_, _, parts) in enumerate(STAGES):
        if i > stage:
            break
        for part in parts:
            part(doc)
    return doc


def frame(azimuth, elevation=12.0):
    """Frame the whole assembly — it spans several layers, not one."""
    lo = np.array([-0.72, -0.88, -0.78], dtype=np.float32)
    hi = np.array([0.72, 0.74, 0.82], dtype=np.float32)
    return R.orbit_camera((lo, hi), azimuth=azimuth, elevation=elevation,
                          margin=1.05)


def trimmed_block():
    """The voxel half of hard surface: no seams to shape, so trim instead."""
    grid = clay.VoxelGrid(voxel_size=0.05)
    steel = grid.palette_add(PLATE_A)
    grid.fill_box((-10, -6, -8), (10, 6, 8), steel)
    before = grid.change_count
    grid.set_brush((10, 6, 8), 11, steel, shape="sphere", falloff="smooth",
                   strength=0.55)
    grid.sculpt_flatten((10, 6, 0), 13, normal=(1, 1, 0), offset=-0.05)
    grid.sculpt_scrape((-10, 6, 0), 13, normal=(-1, 1, 0), offset=-0.05)
    grid.sculpt_fill_cavities((0, 6, 0), 15, passes=2)
    grid.sculpt_smooth((0, 6, 8), 9)
    print(f"  voxel trim changed {grid.change_count - before} cells "
          f"({grid.occupied_count} occupied)")
    return grid


def main():
    R.banner("35 hard surface — a helmet assembled from plates")

    tiles = []
    for i, (name, caption, _) in enumerate(STAGES):
        doc = build(i)
        eye, target = frame(38.0)
        tiles.append(R.render_tile(doc, eye=eye, target=target, size=220,
                                   colors_from_field=True, ao=8, ao_reach=0.09))
        print(f"  {name:12s} {caption}")
    R.contact_sheet(tiles, "35_stages.png", columns=5,
                    caption="under-shell, crown, temples, brow, finish")

    doc = build(len(STAGES) - 1)

    # The hero shot is rendered larger than the gallery default on purpose:
    # hard-surface detail is panel-edge width, and at 480 across it is not
    # resolved at all.
    eye, target = frame(34.0, elevation=10.0)
    image = R.render_array(doc, eye=eye, target=target, width=760, height=620,
                           colors_from_field=True, ao=12, ao_reach=0.09)
    R.write_png(R.output_path("35_hard_surface_helmet.png"), image)
    print("  wrote output/35_hard_surface_helmet.png  (the finished helmet)")

    tiles = []
    for azimuth in (0.0, 45.0, 90.0, 180.0):
        eye, target = frame(azimuth, elevation=8.0)
        tiles.append(R.render_tile(doc, eye=eye, target=target, size=220,
                                   colors_from_field=True, ao=8, ao_reach=0.09))
    R.contact_sheet(tiles, "35_turnaround.png", columns=4,
                    caption="front, three-quarter, profile, back")

    grid = trimmed_block()
    eye, target = R.voxel_camera(grid, 0.05, elevation=20.0)
    R.render_voxels(grid, "35_voxel_trim.png", eye=eye, target=target,
                    caption="flatten, scrape, fill-cavities on a voxel block")

    print(f"  step scale {doc.safe_step_scale():.3f}")
    R.export_model(doc, "35_hard_surface_helmet.ply", resolution=96, decimate=0.06)


if __name__ == "__main__":
    main()

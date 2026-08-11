"""Organic character sculpting: a heroic figure, blockout to finish.

The order a character artist actually works in, one render per stage. Block the
masses out with primitives, pose them with deformers, build muscle with RELIEF
and cut the seams between with INCISE, then lay hair and cloth in as strokes
and sweeps. The sheet reads as the process, not as a list of features.

Three things carry the organic look, and only one of them is a brush.

**Symmetry is one call.** `layer.mirror("x")` puts every item through the
mirror plane (the `mirror=True` below spells the default out; `mirror=False`
opts a one-sided detail out), so the left arm IS the right arm and cannot
drift from it — the same reason a sculptor works with symmetry on and turns it
off only for the last pass.

**Smooth blending is what makes a pile of primitives read as one body.** The
radius has to stay under the size of the feature it joins: over it, the
smooth-min swallows the feature and leaves an egg.

**RELIEF and INCISE use an item as a REGION, not as a shape.** A pec is not a
sphere unioned onto the chest; it is an offset of the chest's own field,
weighted by where the sphere covers it. That is why it swells the surface
instead of sitting on it as a lump, and why its amplitude rides on the blend.

Scope, stated plainly: this is a BLOCKOUT built from a few dozen primitives,
not character art. Hand-placed primitives blended together are excellent for
reading a technique and structurally unable to produce believable anatomy —
a credible hand alone needs more forms than this whole figure has. For
reference-quality geometry the path is an imported sculpt
(`load_mesh` + a volume item), not more spheres.
"""

import math

import numpy as np

import pyclay as clay

import _render as R

# Box/RoundBox `size` is a FULL extent — the surface sits at size/2, and
# RoundBox's `r` rounds INWARD rather than growing it. Sphere, Ellipsoid and
# Capsule take a radius. Mixing the two conventions up silently halves things.
SKIN = "#c08a62"
SKIN_DEEP = "#9c6a47"
CLOTH = "#3c4a63"
LEATHER = "#6b4630"
METAL = "#9aa4b0"
GOLD = "#d2a24c"
CAPE = "#a8332f"


def blockout(layer):
    """The masses, fused. Nothing here is a detail yet."""
    # Ribcage and pelvis as two masses with a waist between them. One ellipsoid
    # reads as a barrel; two with a gap read as a torso.
    layer.add(clay.Ellipsoid(r=(0.295, 0.34, 0.195), position=(0, 1.36, 0)),
              color=SKIN)
    layer.add(clay.Ellipsoid(r=(0.200, 0.205, 0.140), position=(0, 0.775, 0)),
              blend=clay.Smooth(0.035), color=SKIN)

    layer.add(clay.Ellipsoid(r=(0.170, 0.135, 0.120), position=(0, 1.06, 0)),
              blend=clay.Smooth(0.040), color=SKIN)

    # Neck and head, set slightly forward of the spine — what stops a figure
    # reading as a mannequin at attention.
    layer.add(clay.Capsule(a=(0, 1.60, 0.005), b=(0, 1.82, 0.03), r=0.078),
              blend=clay.Smooth(0.028), color=SKIN)
    layer.add(clay.Ellipsoid(r=(0.150, 0.185, 0.160), position=(0, 1.99, 0.02)),
              blend=clay.Smooth(0.028), color=SKIN)
    # Jaw: a smaller mass low and forward, so the head is not a ball.
    layer.add(clay.Ellipsoid(r=(0.100, 0.082, 0.100), position=(0, 1.895, 0.055)),
              blend=clay.Smooth(0.025), color=SKIN)

    # Deltoids, placed OUTSIDE the ribcage so the blend makes a shoulder rather
    # than being absorbed into it.
    layer.add(clay.Sphere(r=0.140, position=(0.360, 1.47, 0)),
              blend=clay.Smooth(0.035), color=SKIN, mirror=True)


def limbs(layer):
    """Arms and legs. `taper` does the work a constant-radius capsule cannot."""
    # Upper arm: thick at the shoulder, thinner at the elbow. Held clear of the
    # ribcage — an arm tucked inside the torso's own radius is swallowed by the
    # blend no matter how small the radius is.
    layer.add(clay.Capsule(a=(0.385, 1.42, 0), b=(0.475, 0.96, 0.02), r=0.100)
              .taper(y0=0.96, y1=1.42, s0=0.78, s1=1.06),
              blend=clay.Smooth(0.025), color=SKIN, mirror=True)
    layer.add(clay.Capsule(a=(0.475, 0.96, 0.02), b=(0.525, 0.52, 0.10), r=0.082)
              .taper(y0=0.52, y1=0.96, s0=0.70, s1=1.0),
              blend=clay.Smooth(0.020), color=SKIN, mirror=True)
    layer.add(clay.Ellipsoid(r=(0.070, 0.098, 0.078), position=(0.532, 0.455, 0.125)),
              blend=clay.Smooth(0.022), color=SKIN, mirror=True)

    # Legs. The thigh is the heaviest mass on the figure and takes the largest
    # blend into the pelvis.
    layer.add(clay.Capsule(a=(0.122, 0.62, 0), b=(0.165, -0.04, 0.01), r=0.125)
              .taper(y0=-0.04, y1=0.58, s0=0.66, s1=1.0),
              blend=clay.Smooth(0.035), color=CLOTH, mirror=True)
    layer.add(clay.Capsule(a=(0.170, -0.05, 0.005), b=(0.180, -0.70, 0.02), r=0.090)
              .taper(y0=-0.70, y1=-0.05, s0=0.62, s1=1.0),
              blend=clay.Smooth(0.022), color=CLOTH, mirror=True)
    # Knee and calf: the two masses that stop a leg reading as a pipe.
    layer.add(clay.Sphere(r=0.088, position=(0.170, -0.05, 0.020)),
              blend=clay.Smooth(0.030), color=CLOTH, mirror=True)
    layer.add(clay.Ellipsoid(r=(0.072, 0.135, 0.080),
                             position=(0.172, -0.22, -0.030)),
              blend=clay.Smooth(0.045), color=CLOTH, mirror=True)
    layer.add(clay.RoundBox(size=(0.19, 0.09, 0.32), r=0.035,
                            position=(0.185, -0.775, 0.06)),
              blend=clay.Smooth(0.04), color=LEATHER, mirror=True)


def musculature(layer):
    """RELIEF builds a form up; INCISE cuts the seam between two.

    The ClayBuildup / DamStandard pair. For both, the blend radius IS the
    amplitude and `rounding` is the falloff width — the item is a region
    weighting an offset, so there is no shape to give a size to.
    """
    # Pectorals.
    layer.add(clay.Ellipsoid(r=(0.140, 0.100, 0.120), position=(0.130, 1.465, 0.100)),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.026), rounding=0.058,
              mirror=True)
    # Sternum, and the linea alba down the abdomen.
    layer.add(clay.Capsule(a=(0, 1.53, 0.190), b=(0, 1.28, 0.186), r=0.015),
              op=clay.Op.INCISE, blend=clay.Smooth(0.013), rounding=0.030)
    layer.add(clay.Capsule(a=(0, 1.26, 0.186), b=(0, 0.96, 0.150), r=0.013),
              op=clay.Op.INCISE, blend=clay.Smooth(0.010), rounding=0.028)
    # Abdominal segments: one region repeated down the belly.
    layer.add(clay.Ellipsoid(r=(0.080, 0.044, 0.082), position=(0.072, 1.245, 0.150))
              .repeat_grid(spacing=0.118, counts=(0, 2, 0)),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.017), rounding=0.034,
              mirror=True)
    # Deltoid, bicep and trapezius swells.
    layer.add(clay.Ellipsoid(r=(0.108, 0.100, 0.108), position=(0.368, 1.463, 0.012)),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.024), rounding=0.050,
              mirror=True)
    layer.add(clay.Ellipsoid(r=(0.066, 0.092, 0.070), position=(0.437, 1.195, 0.042)),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.022), rounding=0.045,
              mirror=True)
    layer.add(clay.Capsule(a=(0.05, 1.615, -0.03), b=(0.29, 1.515, -0.02), r=0.062),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.020), rounding=0.050,
              mirror=True)
    # Scapular hollow: the back needs a cut as much as the front needs a swell.
    layer.add(clay.Capsule(a=(0, 1.58, -0.160), b=(0, 1.16, -0.150), r=0.015),
              op=clay.Op.INCISE, blend=clay.Smooth(0.014), rounding=0.036)


def cape_guide():
    """(N, 4) guide for the cape: a curve falling away from the shoulders."""
    pts = [(0, 1.56, -0.14), (0, 1.18, -0.26), (0, 0.66, -0.34),
           (0, 0.10, -0.39), (0, -0.38, -0.37), (0, -0.66, -0.30)]
    return np.array([(x, y, z, 0.0) for x, y, z in pts], dtype=np.float32)


def wardrobe(layer):
    """Cape, belt and bracers — swept and lofted, not blocked out."""
    # The cape is a swept profile. A sweep distributes its profiles by ARC
    # LENGTH, so the flare is smooth rather than stepped at the guide vertices.
    layer.add(clay.Swept(cape_guide(),
                         [clay.Profile.box(0.28, 0.020),
                          clay.Profile.box(0.46, 0.017),
                          clay.Profile.box(0.38, 0.014)],
                         types="spline", tolerance=0.02),
              color=CAPE)

    # Belt and buckle.
    layer.add(clay.Torus(R=0.215, r=0.040, position=(0, 0.80, 0)),
              blend=clay.Smooth(0.018), color=LEATHER)
    layer.add(clay.RoundBox(size=(0.12, 0.10, 0.07), r=0.012,
                            position=(0, 0.80, 0.165)),
              blend=clay.Smooth(0.02), color=GOLD)

    # Shoulder discs: the piece of silhouette that reads as armour.
    layer.add(clay.Cylinder(r=0.058, h=0.03, position=(0.335, 1.555, 0.02),
                            rotation_axis_angle=((1, 0, 0), math.pi / 2)),
              blend=clay.Smooth(0.02), color=GOLD, mirror=True)

    # Bracers, as a tube of varying radius down the forearm.
    layer.add(clay.Stroke(points=np.array([(0.487, 0.86, 0.038, 0.082),
                                           (0.510, 0.70, 0.068, 0.086),
                                           (0.522, 0.58, 0.088, 0.078)],
                                          dtype=np.float32), blend_k=0.02),
              blend=clay.Smooth(0.025), color=LEATHER, mirror=True)


def hair_and_skin(layer):
    """Strokes for hair, a displaced region for skin, PAINT for colour.

    A stroke resolves to stamps from a preset — the same call a host makes when
    a stylus drags across a surface. Taper is what makes a lock read as drawn
    rather than extruded.
    """
    preset = clay.StrokePreset(radius=0.038, spacing=0.30,
                               taper_start=0.20, taper_end=0.85)
    for i in range(7):
        # centred on pi, so cos(a) is negative and the locks fall down the
        # BACK of the skull rather than across the face
        a = math.pi - 1.15 + i * 0.383
        samples = []
        for t in range(9):
            u = t / 8.0
            # Start just proud of the crown and sweep down and back, so the
            # locks sit ON the skull instead of inside it.
            r = 0.175 + 0.045 * u
            samples.append((
                r * math.sin(a) * (0.30 + 0.85 * u),
                2.02 - 0.40 * u - 0.14 * u * u,
                -0.03 - 0.16 * u + r * math.cos(a) * (0.30 + 0.55 * u),
                1.0 - 0.35 * u,
            ))
        layer.apply_stroke(np.array(samples, dtype=np.float32), preset,
                           clay.Sphere(r=0.038), blend=clay.Smooth(0.028),
                           color=GOLD)

    # Skin: a low-amplitude displacement over the torso only, so there is
    # surface without the silhouette turning to noise.
    layer.add(clay.Ellipsoid(r=(0.33, 0.62, 0.24), position=(0, 1.12, 0))
              .displace(amplitude=0.006, frequency=26.0),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.009), rounding=0.12)

    # PAINT is colour only: it tints where it covers and changes no distance.
    layer.add(clay.Ellipsoid(r=(0.21, 0.155, 0.185), position=(0, 1.95, 0.06)),
              op=clay.Op.PAINT, blend=clay.Smooth(0.04), color=SKIN_DEEP)


def face_features(layer):
    """A face, built the way the muscles were: regions, not shapes.

    The head is at (0, 1.94, 0.02) with radii (0.155, 0.19, 0.165), so its front
    is near z = 0.185. Every feature here sits just inside that and works by
    RELIEF or INCISE, which is why it reads as bone and socket rather than as
    beads glued to a ball.
    """
    # Brow ridge, and the sockets beneath it.
    layer.add(clay.Ellipsoid(r=(0.115, 0.024, 0.055), position=(0, 2.032, 0.128)),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.014), rounding=0.030)
    layer.add(clay.Ellipsoid(r=(0.030, 0.021, 0.026), position=(0.055, 2.005, 0.145)),
              op=clay.Op.INCISE, blend=clay.Smooth(0.012), rounding=0.020,
              mirror=True)
    # Nose: a small mass, blended on.
    layer.add(clay.Capsule(a=(0, 2.008, 0.152), b=(0, 1.948, 0.172), r=0.027),
              blend=clay.Smooth(0.012), color=SKIN)
    # Mouth line and the cheekbones either side of it.
    layer.add(clay.Capsule(a=(-0.042, 1.908, 0.146), b=(0.042, 1.908, 0.146),
                           r=0.009),
              op=clay.Op.INCISE, blend=clay.Smooth(0.011), rounding=0.022)
    layer.add(clay.Ellipsoid(r=(0.052, 0.034, 0.045), position=(0.080, 1.968, 0.118)),
              op=clay.Op.RELIEF, blend=clay.Smooth(0.013), rounding=0.030,
              mirror=True)
    # Jawline, cut back under the cheek.
    layer.add(clay.Capsule(a=(0.052, 1.906, 0.070), b=(0.112, 1.956, 0.010),
                           r=0.020),
              op=clay.Op.INCISE, blend=clay.Smooth(0.012), rounding=0.026,
              mirror=True)


def hammer(layer):
    """A prop, so the figure carries weight. Hard surface on an organic body.

    The one deliberately one-sided thing on the figure: a hammer is held in ONE
    hand, so every piece opts out of the layer mirror with mirror=False.
    """
    layer.add(clay.Capsule(a=(0.534, 0.60, 0.118), b=(0.560, -0.10, 0.150), r=0.025),
              blend=clay.Smooth(0.015), color=LEATHER, mirror=False)
    layer.add(clay.RoundBox(size=(0.175, 0.155, 0.155), r=0.018,
                            position=(0.552, 0.06, 0.140)),
              blend=clay.Smooth(0.012), color=METAL, mirror=False)
    # ENGRAVE cuts a shallow mark weighted by the region, the way a maker's
    # stamp sits in metal rather than being a boolean notch.
    layer.add(clay.Box(size=(0.17, 0.016, 0.016), position=(0.552, 0.06, 0.232))
              .repeat_grid(spacing=0.035, counts=(0, 1, 0)),
              op=clay.Op.ENGRAVE, blend=clay.Smooth(0.010), rounding=0.014,
              mirror=False)


def build(stage):
    """Build up to `stage`, so each render shows exactly one more step."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("figure")
    layer.mirror("x")

    blockout(layer)
    if stage >= 1:
        limbs(layer)
    if stage >= 2:
        musculature(layer)
    if stage >= 3:
        wardrobe(layer)
    if stage >= 4:
        hair_and_skin(layer)
        face_features(layer)
        hammer(layer)
    return doc, layer


def main():
    R.banner("34 organic character — blockout to finish")

    stages = [
        ("blockout", "primitives + smooth blend"),
        ("limbs", "taper, mirrored"),
        ("muscle", "RELIEF + INCISE"),
        ("wardrobe", "swept cape, belt, bracers"),
        ("finish", "strokes, displace, paint"),
    ]
    tiles = []
    for i, (name, caption) in enumerate(stages):
        doc, layer = build(i)
        eye, target = R.layer_camera(layer, azimuth=28.0, elevation=6.0)
        tiles.append(R.render_tile(doc, eye=eye, target=target, size=200,
                                   colors_from_field=True, ao=8, ao_reach=0.07))
        print(f"  {name:10s} {caption}")
    R.contact_sheet(tiles, "34_stages.png", columns=5,
                    caption="blockout, limbs, muscle, wardrobe, finish")

    doc, layer = build(4)
    eye, target = R.layer_camera(layer, azimuth=32.0, elevation=8.0)
    image = R.render_array(doc, eye=eye, target=target, width=560, height=680,
                           colors_from_field=True, ao=12, ao_reach=0.07)
    R.write_png(R.output_path("34_organic_character.png"), image)
    print("  wrote output/34_organic_character.png  (the finished figure)")

    # The two views a sculptor checks silhouette in, plus the back.
    tiles = []
    for azimuth in (0.0, 55.0, 90.0, 160.0):
        eye, target = R.layer_camera(layer, azimuth=azimuth, elevation=5.0)
        tiles.append(R.render_tile(doc, eye=eye, target=target, size=200,
                                   colors_from_field=True, ao=8, ao_reach=0.07))
    R.contact_sheet(tiles, "34_turnaround.png", columns=4,
                    caption="front, three-quarter, profile, back")

    print(f"  step scale {doc.safe_step_scale():.3f}")
    R.export_model(doc, "34_organic_character.ply", resolution=72, decimate=0.10)


if __name__ == "__main__":
    main()

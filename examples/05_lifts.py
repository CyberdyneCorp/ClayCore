"""Profile lifts: 2D shapes swept into 3D by extrusion and revolution.

A lift is exact — the 3D field inherits the 2D profile's exactness — which is
why these are cheaper to trace than a deformed primitive. The last case builds
a polygon profile from points, which is how the app draws custom cross
sections.
"""

import numpy as np

import pyclay as clay

import _render as R

PROFILES = [
    ("circle", clay.Profile.circle(r=0.7)),
    ("box", clay.Profile.box(half_x=0.7, half_y=0.45)),
    ("hexagon", clay.Profile.hexagon(r=0.7)),
    ("triangle", clay.Profile.triangle(r=0.7)),
    ("trapezoid", clay.Profile.trapezoid(bottom=0.75, top=0.35, half_height=0.5)),
    ("vesica", clay.Profile.vesica(r=0.9, d=0.45)),
]


def star_polygon(points=5, outer=0.8, inner=0.36):
    """A concave star — polygon profiles handle concavity via even-odd."""
    verts = []
    for i in range(points * 2):
        angle = np.pi * i / points
        radius = outer if i % 2 == 0 else inner
        verts.append((float(radius * np.cos(angle)), float(radius * np.sin(angle))))
    return verts


def main():
    R.banner("05 lifts — extrude and revolve a 2D profile")

    # Extrude every built-in profile.
    tiles = []
    for name, profile in PROFILES:
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Extrude(profile=profile, half_depth=0.35))
        tiles.append(R.render_tile(doc, layer=layer, size=160))
    R.contact_sheet(tiles, "05_extrude_profiles.png", columns=3,
                    caption=", ".join(n for n, _ in PROFILES))

    # Revolve the same profiles about the Y axis, offset from it.
    tiles = []
    for name, profile in PROFILES:
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        layer.add(clay.Revolve(profile=profile, offset=1.1))
        tiles.append(R.render_tile(doc, layer=layer, size=160))
    R.contact_sheet(tiles, "05_revolve_profiles.png", columns=3,
                    caption="the same profiles swept about the axis")

    # A polygon profile: concave, authored from points.
    star = clay.Profile.polygon(points=star_polygon())
    print(f"  star profile has {star.point_count} vertices")

    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Extrude(profile=star, half_depth=0.3), color="#f2c14e")
    eye, target = R.layer_camera(layer)
    R.render(doc, "05_extrude_star.png", eye=eye, target=target,
             colors_from_field=True, caption="extruded concave polygon")
    R.export_model(doc, "05_star.obj", resolution=48, decimate=0.08)

    # Lifts stay exact, so the step scale is untouched.
    print(f"  extruded star safe step scale: {doc.safe_step_scale():.3f}")

    # A revolved polygon makes a turned profile — a vase.
    vase = clay.Profile.polygon(points=[
        (0.10, -1.00), (0.55, -0.95), (0.62, -0.55), (0.40, -0.10),
        (0.34, 0.35), (0.52, 0.80), (0.58, 1.00), (0.44, 1.00),
        (0.38, 0.80), (0.22, 0.35), (0.26, -0.10), (0.46, -0.55),
        (0.40, -0.80), (0.10, -0.85),
    ])
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Revolve(profile=vase, offset=0.0), color="#5b8266")
    eye, target = R.layer_camera(layer)
    R.render(doc, "05_revolve_vase.png", eye=eye, target=target,
             colors_from_field=True, caption="revolved polygon profile")
    R.export_model(doc, "05_vase.ply", resolution=64)


if __name__ == "__main__":
    main()

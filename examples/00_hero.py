"""The README hero: an SDF sculpt and a voxel sculpt, side by side.

Both halves come out of the same library and the same document model — the
left is a smooth-blended distance field traced by sphere marching, the right
is a palette-indexed voxel grid traced by DDA. One image, both halves of what
claycore does.
"""

import pyclay as clay

import _render as R

TILE = 300
VOXEL_SIZE = 0.09


def sdf_sculpt():
    """A blended, deformed, coloured form — the SDF half.

    Every feature has to sit *outside* the body it blends onto, otherwise the
    smooth-min swallows it and the result is a plain egg. Blend radii are kept
    well under the size of the feature they join.
    """
    doc = clay.Document()
    layer = doc.add_sdf_layer("sculpt")

    # Body: a sphere with a smaller head merged on top.
    layer.add(clay.Sphere(r=0.72), color="#4f8fbf")
    layer.add(clay.Sphere(r=0.44, position=(0, 0.86, 0)),
              blend=clay.Smooth(0.18), color="#79bde0")

    # A collar that stands proud of the body: ring radius beyond the sphere.
    layer.add(clay.Torus(R=0.86, r=0.13, position=(0, 0.28, 0)),
              blend=clay.Smooth(0.06), color="#e8b04b")

    # Radial array of spokes. The capsule is authored already offset from the
    # axis and repeated with offset=0, so the array simply rotates it — much
    # easier to reason about than offsetting inside the repeat.
    layer.add(
        clay.Capsule(a=(0.55, 0, 0), b=(1.2, 0.12, 0), r=0.1)
        .repeat_radial(count=8, offset=0.0),
        blend=clay.Smooth(0.09),
        color="#c85a4a",
    )

    # A displaced base, so surface detail is visible against the smooth body.
    layer.add(clay.Sphere(r=0.46, position=(0, -0.74, 0)).displace(
        amplitude=0.07, frequency=11.0), blend=clay.Smooth(0.14), color="#37678a")

    # Bore a hole straight through the head. Rotating the cylinder onto Z
    # makes it a through-hole visible in silhouette, rather than a dimple.
    layer.add(clay.Cylinder(r=0.16, h=1.2, position=(0, 0.9, 0),
                            rotation_axis_angle=((1, 0, 0), 1.5707964)),
              op=clay.Op.SUBTRACT, blend=clay.Smooth(0.04))
    return doc, layer


def voxel_sculpt():
    """A palette-coloured voxel figure — the voxel half.

    Deliberately the same *kind* of subject as the SDF sculpt: one form, in
    the round. A blocky character reads far better at this size than a
    landscape would, where each voxel would be a handful of pixels.
    """
    grid = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    shell = grid.palette_add("#5f93bf")
    plate = grid.palette_add("#c85a4a")
    trim = grid.palette_add("#e8b04b")
    eye = grid.palette_add("#f2f5f7")
    base = grid.palette_add("#8a8780")

    # A plinth, so the figure is standing on something.
    grid.fill_box((-7, -10, -6), (7, -9, 6), base)

    # Legs, authored on one side only: set_mirrored places the matching cell
    # across X, which is how a symmetric model is actually sculpted.
    for y in range(-8, -3):
        for z in range(-2, 3):
            grid.set_mirrored((4, y, z), shell, axes="x")
            grid.set_mirrored((3, y, z), shell, axes="x")

    # Torso, with a contrasting chest plate.
    grid.fill_box((-5, -3, -3), (5, 3, 3), shell)
    grid.fill_box((-3, -1, -4), (3, 2, -4), plate)
    grid.fill_box((-5, 3, -3), (5, 3, 3), trim)      # collar

    # Arms, mirrored again.
    for y in range(-2, 3):
        for z in range(-2, 3):
            grid.set_mirrored((6, y, z), plate, axes="x")
            grid.set_mirrored((7, y, z), plate, axes="x")

    # Head, sunk slightly into the collar.
    grid.fill_box((-4, 4, -3), (4, 9, 3), shell)
    grid.fill_box((-4, 9, -3), (4, 9, 3), trim)      # cap

    # Eyes on the front face, mirrored so they stay aligned.
    for y in (6, 7):
        grid.set_mirrored((2, y, -4), eye, axes="x")

    # Hollow the back of the head — erase_brush is the sculpting eraser.
    grid.erase_brush((0, 7, 4), 3)

    return grid


def main():
    R.banner("00 hero — SDF and voxels side by side")

    doc, layer = sdf_sculpt()
    eye, target = R.layer_camera(layer, azimuth=32.0, elevation=18.0, margin=1.0)
    sdf_image = R.render_array(doc, eye=eye, target=target,
                               colors_from_field=True, width=TILE, height=TILE)

    grid = voxel_sculpt()
    veye, vtarget = R.voxel_camera(grid, VOXEL_SIZE, azimuth=208.0, elevation=14.0)
    voxel_image = R.render_voxels_array(grid, eye=veye, target=vtarget,
                                        width=TILE, height=TILE)

    R.side_by_side(sdf_image, voxel_image, "00_hero.png",
                   caption="left: SDF sculpt, right: voxel sculpt")

    # Both halves are real content, so both can be exported.
    R.export_model(doc, "00_hero_sdf.ply", resolution=64)
    R.save_model(grid.mesh(), "00_hero_voxels.ply")


if __name__ == "__main__":
    main()

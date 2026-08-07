"""Masks: freezing a region against an edit.

A mask is a scalar field in [0, 1] that scales how strongly a brush may act:
the effective strength at a cell is `strength * (1 - mask)`, so a fully masked
cell is frozen and an unmasked one behaves as it always did. It is painted
with the same brush vocabulary as the sculpting itself — size, shape, falloff,
strength — because masking is the same gesture.

The part worth stating out loud is the addressing. A mask lives on its own
lattice in WORLD units, not in a layer's voxel cells. That is what lets it
survive a resolution change or a move between the SDF and voxel
representations: the same painted region means the same thing to a grid at
0.05 units per cell and to one at 0.2. Masks silently dying on voxelization is
the single most complained-about bug in the tool this engine is measured
against, and world addressing is what makes it unrepresentable here.

One boundary, so nothing is expected that is not delivered: a mask gates edits
where they are AUTHORED. Voxel edits consume it per cell at edit time. It does
not retroactively protect a region from SDF items already in the edit list.

Masking is also a GESTURE, not just a data structure. A mask is painted along a
drag by the same stroke engine that resolves a sculpting stroke — same spacing,
pressure, taper, steady stroke and jitter — so the last sections here are about
the brush rather than the field: painting a band, taking the complement over a
region (which plain `invert` structurally cannot do), and freezing an SDF layer
against `relax`.
"""

import numpy as np

import pyclay as clay

import _render as R

VOXEL_SIZE = 0.1


def slab():
    """A block of material with a raised band, so an edit is easy to read."""
    grid = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    stone = grid.palette_add("#8d97a4")
    accent = grid.palette_add("#c07a52")
    grid.fill_box((-10, -4, -6), (10, 1, 6), stone)
    grid.fill_box((-10, 2, -2), (10, 3, 2), accent)
    return grid


def main():
    R.banner("11 masks — freezing a region against an edit")

    # --- the same cut, masked and not --------------------------------------
    # A mask over the left half. Everything else about the two edits is
    # identical, including the dither seed.
    freeze = clay.MaskField(cell_size=VOXEL_SIZE)
    for x in range(-12, 0):
        for y in range(-6, 6):
            for z in range(-8, 9):
                freeze.set((x, y, z), 1.0)
    print(f"  mask covers {freeze.painted_count} cells, bounds {freeze.bounds()}")

    tiles = []
    plain = slab()
    before = plain.occupied_count
    tiles.append(("untouched", plain))

    cut = slab()
    cut.erase_brush((0, 0, 0), 21, shape="sphere", falloff="smooth", seed=4)
    tiles.append(("unmasked cut", cut))

    masked = slab()
    masked.erase_brush((0, 0, 0), 21, shape="sphere", falloff="smooth", seed=4,
                       mask=freeze)
    tiles.append(("masked cut", masked))

    for name, grid in tiles:
        print(f"  {name:14s} {grid.occupied_count:6d} cells")
    if not (cut.occupied_count < masked.occupied_count < before):
        raise SystemExit("the mask did not spare anything")
    # Nothing on the masked side moved at all.
    if any(masked.get((x, 0, 0)) != plain.get((x, 0, 0)) for x in range(-10, 0)):
        raise SystemExit("the mask leaked")

    images = []
    for _name, grid in tiles:
        eye, target = R.voxel_camera(grid, VOXEL_SIZE, azimuth=34.0, elevation=24.0)
        images.append(R.render_voxels_array(grid, eye=eye, target=target,
                                            width=230, height=200))
    R.contact_sheet(images, "11_mask_freeze.png", columns=3,
                    caption="the same brush, unmasked and masked over the left half")

    # --- a soft mask attenuates rather than freezing ------------------------
    # Painting the mask with a falloff gives a graded gate, so the cut fades
    # out across the boundary instead of stopping at a wall.
    tiles = []
    for strength in (0.0, 0.35, 0.7, 1.0):
        soft = clay.MaskField(cell_size=VOXEL_SIZE)
        if strength > 0.0:
            soft.paint((-0.5, 0.0, 0.0), size=25, target=strength,
                       shape="sphere", falloff="smooth")
        grid = slab()
        grid.erase_brush((0, 0, 0), 21, shape="sphere", falloff="smooth", seed=4,
                         mask=soft if strength > 0.0 else None)
        print(f"  mask peak {strength:<4} -> {grid.occupied_count:6d} cells survive")
        eye, target = R.voxel_camera(grid, VOXEL_SIZE, azimuth=34.0, elevation=24.0)
        tiles.append(R.render_voxels_array(grid, eye=eye, target=target,
                                           width=200, height=185))
    R.contact_sheet(tiles, "11_mask_falloff.png", columns=4,
                    caption="a soft mask at peak 0.0, 0.35, 0.7, 1.0")

    # --- region operations --------------------------------------------------
    stages = []
    m = clay.MaskField(cell_size=VOXEL_SIZE)
    m.paint((0, 0, 0), size=15, shape="sphere", falloff="constant")
    stages.append(("painted", m.painted_count))
    m.expand(2)
    stages.append(("expand 2", m.painted_count))
    m.contract(1)
    stages.append(("contract 1", m.painted_count))
    m.smooth(2)
    stages.append(("smooth 2", m.painted_count))
    m.invert()
    stages.append(("invert", m.painted_count))
    for name, count in stages:
        print(f"  {name:11s} {count:6d} cells")

    # --- the invariant: a mask outlives a resolution change -----------------
    doc = clay.Document()
    doc.add_voxel_layer("clay", voxel_size=VOXEL_SIZE)
    mask = doc.add_mask("clay", cell_size=VOXEL_SIZE)
    mask.paint((0.4, 0.2, 0.0), size=13, shape="sphere", falloff="smooth")

    probes = np.array([[0.4, 0.2, 0.0], [0.15, 0.2, 0.0], [3.0, 3.0, 3.0]], np.float32)
    reference = mask.sample_many(probes)

    for voxel_size in (0.05, 0.1, 0.4):
        grid = clay.VoxelGrid(voxel_size)
        grid.fill_box((0, 0, 0), (16, 16, 16), grid.palette_add("#ffffff"))
        grid.erase_brush((4, 2, 0), 9, shape="sphere", mask=mask)
        after = mask.sample_many(probes)
        ok = bool(np.allclose(after, reference))
        print(f"  at {voxel_size:<5} units/cell the mask still reads "
              f"{after[0]:.3f} at its centre: {ok}")
        if not ok:
            raise SystemExit("the mask did not survive a resolution change")

    # ...and through the document format.
    path = R.output_path("11_masked.clayspace")
    doc.save(str(path))
    reloaded = clay.load(str(path))
    if not np.allclose(reloaded.mask("clay").sample_many(probes), reference):
        raise SystemExit("the mask did not survive a save/load round trip")
    print("  and through a save/load round trip: True")

    # --- masking as a gesture: a stroke paints the mask ---------------------
    # The same resolve_stroke a sculpting stroke goes through, so a mask stroke
    # tapers and spaces the way a brush stroke does. The footprint comes from
    # each stamp's WORLD radius, which is why the same stroke covers the same
    # region on two masks at different resolutions.
    preset = clay.StrokePreset(radius=0.35, spacing=0.25, taper_start=0.2,
                               taper_end=0.2)
    samples = [(x, 0.0, 0.0, 1.0, 0.0)
               for x in np.linspace(-1.0, 1.0, 24, dtype=np.float32)]

    widths = []
    for cell in (VOXEL_SIZE, VOXEL_SIZE / 2.0):
        painted = clay.MaskField(cell_size=cell)
        stamps = painted.apply_stroke(samples, preset, target=1.0)
        covered = painted.painted_count
        volume = covered * cell ** 3
        widths.append(volume)
        print(f"  a stroke of {stamps} stamps on a {cell:.3f} mask covers "
              f"{covered} cells = {volume:.4f} cubic units")
    if abs(widths[0] - widths[1]) / widths[0] > 0.2:
        raise SystemExit("the stroke's width tracked the mask's resolution, "
                         "which is the bug the world-radius conversion prevents")

    stroked = clay.MaskField(cell_size=VOXEL_SIZE)
    stroked.apply_stroke(samples, preset, target=1.0)
    band = slab()
    band.erase_brush((0, 0, 0), 21, shape="sphere", falloff="smooth", seed=4,
                     mask=stroked)
    print(f"  a stroke-painted mask spares {band.occupied_count - cut.occupied_count} "
          f"cells the same cut would have taken")
    if band.occupied_count <= cut.occupied_count:
        raise SystemExit("the stroked mask spared nothing")

    # ...and erasing is the same call, with the target at the other end.
    stroked.apply_stroke(samples, preset, target=0.0)
    if stroked.sample((0.0, 0.0, 0.0)) > 0.1:
        raise SystemExit("target=0 did not release the mask")
    print("  and the same call with target=0 releases it again")

    # --- the complement invert() cannot take --------------------------------
    # invert() flips only what has been PAINTED — a sparse unbounded lattice has
    # no finite complement — so "mask this, edit everything else" needs the
    # region from the caller, who always has one.
    limb = clay.MaskField(cell_size=VOXEL_SIZE)
    limb.paint((0.0, 0.0, 0.0), size=9, shape="sphere", falloff="constant")
    region = ((-1.2, -0.6, -0.8), (1.2, 0.4, 0.8))
    limb.invert_within(region)
    inside = limb.sample((0.0, 0.0, 0.0))
    beside = limb.sample((0.8, 0.0, 0.0))
    outside = limb.sample((3.0, 0.0, 0.0))
    print(f"  invert_within: what was painted reads {inside:.2f}, the rest of the "
          f"region {beside:.2f}, past the region {outside:.2f}")
    if not (inside < 0.1 < beside and outside < 0.1):
        raise SystemExit("the bounded complement is not bounded, or not a complement")

    # --- the freeze reaches the SDF verbs too -------------------------------
    # relax and flatten rewrite a sampled field, and until now nothing gated
    # them: a masked region inside their sphere was not actually frozen.
    bumpy = clay.Volume.from_document(
        _rippled(), cell=0.03, band=0.12,
        bounds=((-1.1, -1.1, -1.1), (1.1, 1.1, 1.1)))
    settings = dict(strength=1.0, radius_cells=2, iterations=3,
                    centre=(0.55, 0.0, 0.0), region_radius=0.5, falloff=0.2)

    freeze_right = clay.MaskField(cell_size=0.05)
    freeze_right.fill(((0.0, -1.2, -1.2), (1.4, 1.2, 1.2)), 1.0)

    probe = np.array([[0.55, 0.0, 0.0]], np.float32)
    original = float(bumpy.eval(probe)[0])
    smoothed = float(bumpy.relaxed(**settings).eval(probe)[0])
    held = float(bumpy.relaxed(**settings, mask=freeze_right).eval(probe)[0])
    print(f"  relax at the probe: {original:+.5f} -> {smoothed:+.5f} unmasked, "
          f"{held:+.5f} frozen")
    if held != original:
        raise SystemExit("a frozen sample moved — freezing has to be exact, "
                         "not merely close")
    if smoothed == original:
        raise SystemExit("the unmasked relax did nothing, so the test proves nothing")

    R.save_model(masked.mesh(), "11_masked.ply")


def _rippled():
    """A sphere with a fine ripple: something relax can visibly remove."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    for i in range(12):
        a = i * 6.283 / 12.0
        layer.add(clay.Sphere(0.14).at(
            (0.62 * float(np.cos(a)), 0.0, 0.62 * float(np.sin(a)))))
    layer.add(clay.Sphere(0.6))
    return doc


if __name__ == "__main__":
    main()

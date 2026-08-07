"""Mask extrude — masking a patch of a surface and pulling it off as a solid.

ZBrush calls it Extract, 3DCoat reaches it through Extrude from a frozen area,
and it is how plates, panels, straps, pockets and shells get made. It is also
what a mask is *for* once it can do more than freeze: up to here a mask only
ever prevented something.

Three things are worth reading before looking.

**THE MASK IS THE REGION.** Relax and flatten both take a `region_radius`,
because a sphere is the only way they have of saying where to act. This takes
none: the painted region bounds itself. That is not a convenience, it is why
this samples a smaller volume than either of them.

**The one new mechanism is measuring the mask.** A mask is a [0, 1] scalar on a
lattice, not a distance field — composing one into a field expression directly
would put a near-vertical step in the result and the Lipschitz bound the
raymarcher depends on would stop meaning anything. `MaskField.to_field()` runs
a Euclidean distance transform over the masked region and hands back an ordinary
narrow-band Volume: 1-Lipschitz, and after that the extrude is ordinary op
composition — the shell of the source intersected with the masked region.

**Both representations do it, and they agree.** SDF layers sample; voxel grids
stay in cell space and keep their palette. A document must not mean something
different depending on which one it is stored in, so the last section measures
the two against each other rather than asserting it.
"""

import numpy as np

import pyclay as clay

import _render as R

RADIUS = 0.6
VOXEL_SIZE = 0.03


def body():
    """The form a plate gets extracted from."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.Sphere(RADIUS), color="#6f7d8c")
    return doc, layer


def cap_mask(doc, cap_radius=0.32, cell_size=VOXEL_SIZE):
    """A round patch masked over the north pole, painted with a brush."""
    mask = doc.add_mask("body", cell_size=cell_size)
    size = int(round(2.0 * cap_radius / cell_size))
    mask.paint((0.0, RADIUS, 0.0), size=size, target=1.0,
               shape="sphere", falloff="constant")
    return mask


def surface_along_y(volume, lo=0.0, hi=1.2, step=0.002):
    """Every crossing of the isosurface along +Y, outermost first."""
    ys = np.arange(hi, lo, -step, dtype=np.float32)
    pts = np.stack([np.zeros_like(ys), ys, np.zeros_like(ys)], axis=1)
    d = volume.eval(pts)
    crossings = np.nonzero(np.diff(np.signbit(d)))[0]
    return [float(ys[i]) for i in crossings]


def with_plate(plate, color="#c8843f"):
    """The body with an extracted plate laid back on it, for the render."""
    doc, _ = body()
    shell = doc.add_sdf_layer("plate")
    shell.add(plate, color=color)
    return doc


def plate_only(plate, color="#c8843f"):
    """The extract on its own — the only way to SEE an inward one, which is
    otherwise buried in the body it came out of."""
    doc = clay.Document()
    doc.add_sdf_layer("plate").add(plate, color=color)
    return doc


def main():
    R.banner("25 mask extrude — a plate pulled off a masked patch")

    EYE, TARGET = (1.6, 1.5, 1.9), (0.0, 0.15, 0.0)

    # --- the mask, measured --------------------------------------------------
    # The conversion everything else rests on. Sampled with a wide band so the
    # numbers below are distances rather than the conservative bound a volume
    # reports outside its band.
    doc, _layer = body()
    mask = cap_mask(doc)
    measured = mask.to_field(band=0.25, pad=0.3)
    pole = np.array([[0.0, RADIUS, 0.0]], np.float32)
    probes = np.array([[0.0, RADIUS, 0.0],
                       [0.20, RADIUS, 0.0],
                       [0.45, RADIUS, 0.0]], np.float32)
    values = measured.eval(probes)
    print(f"  mask covers {mask.painted_count} cells; measured as a distance:")
    for (x, _y, _z), v in zip(probes, values):
        where = "inside" if v < 0 else "outside"
        print(f"    {x:+.2f} from the pole -> {v:+.3f} ({where})")
    if not (values[0] < 0 < values[2]):
        raise SystemExit("the measured mask does not change sign across its border")
    # It is a distance, not a step: unit rate across the border.
    near = measured.eval(np.array([[0.36, RADIUS, 0.0], [0.44, RADIUS, 0.0]], np.float32))
    if abs((near[1] - near[0]) - 0.08) > 0.03:
        raise SystemExit("the measured mask is not changing at a distance's rate")

    # --- a plate comes off ---------------------------------------------------
    plate = doc.mask_extrude(mask, thickness=0.12, side="outward")
    crossings = surface_along_y(plate)
    print(f"  the plate's surfaces along +Y: {[f'{c:.3f}' for c in crossings]}")
    if len(crossings) != 2:
        raise SystemExit("an outward plate should have an outer and an inner surface")
    outer, inner = crossings
    print(f"    it sits on the sphere at {inner:.3f} (radius {RADIUS}) and is "
          f"{outer - inner:.3f} thick")
    if abs(inner - RADIUS) > 0.05:
        raise SystemExit("the plate did not land on the surface it was extracted from")
    if abs((outer - inner) - 0.12) > 0.04:
        raise SystemExit("the plate is not the thickness it was asked for")
    # And nothing away from the mask.
    if float(plate.eval(np.array([[0.0, -RADIUS, 0.0]], np.float32))[0]) <= 0.0:
        raise SystemExit("the extract leaked onto the unmasked side")

    R.render(with_plate(plate), "25_extract.png", eye=EYE, target=TARGET,
             colors_from_field=True)

    # --- each side means what it says ----------------------------------------
    tiles, labels = [], []
    for side in ("outward", "inward", "centred"):
        piece = doc.mask_extrude(mask, thickness=0.12, side=side)
        marks = surface_along_y(piece)
        mid = sum(marks) / len(marks)
        print(f"  {side:<8} -> surfaces at {[f'{c:.3f}' for c in marks]}, "
              f"centred on {mid:.3f}")
        # On its own, not on the body: an inward extract is inside the form it
        # came from, so a composite render would show three spheres and one
        # difference.
        tiles.append(R.render_array(plate_only(piece), eye=(0.9, 1.35, 1.1),
                                    target=(0.0, 0.55, 0.0), width=230, height=215,
                                    colors_from_field=True))
        labels.append(side)
    R.contact_sheet(tiles, "25_extract_sides.png", columns=3,
                    caption="the extract alone, " + ", ".join(labels) +
                            " — each on its own side of the sphere's surface")

    mids = [sum(surface_along_y(doc.mask_extrude(mask, thickness=0.12, side=s))) / 2.0
            for s in ("outward", "inward", "centred")]
    if not (mids[1] < mids[2] < mids[0]):
        raise SystemExit("the three sides are not on the sides they claim")

    # --- the rim ------------------------------------------------------------
    # A rounded intersection softens the border; smoothing a COPY of the mask
    # first softens where the border is. Neither touches the caller's mask.
    before = mask.painted_count
    tiles = []
    for label, kwargs in (("hard", {}),
                          ("rounded rim", dict(border_round=0.05)),
                          ("blurred border", dict(border_round=0.05, border_smooth=3))):
        piece = doc.mask_extrude(mask, thickness=0.12, **kwargs)
        print(f"  {label:<15} -> {piece.brick_count:3d} bricks, "
              f"lipschitz {piece.sample_lipschitz:.3f}")
        tiles.append(R.render_array(plate_only(piece), eye=(0.9, 1.35, 1.1),
                                    target=(0.0, 0.55, 0.0), width=230, height=215,
                                    colors_from_field=True))
    R.contact_sheet(tiles, "25_extract_rim.png", columns=3,
                    caption="hard rim, rounded rim, and a rim from a blurred mask")
    if mask.painted_count != before:
        raise SystemExit("the extrude modified the mask it was given")

    # --- the two representations agree ---------------------------------------
    # The claim that makes this one verb rather than two: a document must not
    # mean something different depending on how it is stored.
    grid = clay.VoxelGrid(VOXEL_SIZE)
    stone = grid.palette_add("#6f7d8c")
    n = int(np.ceil(RADIUS / VOXEL_SIZE)) + 2
    span = np.arange(-n, n + 1)
    gx, gy, gz = np.meshgrid(span, span, span, indexing="ij")
    centres = (np.stack([gx, gy, gz], axis=-1).reshape(-1, 3) + 0.5) * VOXEL_SIZE
    inside = np.linalg.norm(centres, axis=1) <= RADIUS
    cells = np.stack([gx, gy, gz], axis=-1).reshape(-1, 3)[inside].astype(np.int32)
    grid.set_many(cells, stone)

    extract = grid.mask_extrude(mask, thickness=0.12, side="outward")
    print(f"  voxel extract: {extract.occupied_count} cells, "
          f"palette carried over ({extract.palette_size - 1} colour)")
    if extract.occupied_count == 0:
        raise SystemExit("the voxel extrude produced nothing")

    # Every cell the voxel extract claims is inside the SDF extract, or within a
    # voxel of it. One is a lattice of cubes and the other an isosurface, so a
    # voxel is the honest tolerance.
    (lo, hi) = extract.bounds()
    span_x = np.arange(lo[0], hi[0] + 1)
    span_y = np.arange(lo[1], hi[1] + 1)
    span_z = np.arange(lo[2], hi[2] + 1)
    ex, ey, ez = np.meshgrid(span_x, span_y, span_z, indexing="ij")
    coords = np.stack([ex, ey, ez], axis=-1).reshape(-1, 3).astype(np.int32)
    occupied = np.array([extract.get(tuple(int(v) for v in c)) for c in coords]) != 0
    claimed = (coords[occupied] + 0.5).astype(np.float32) * VOXEL_SIZE
    agreeing = float(np.mean(plate.eval(claimed) < VOXEL_SIZE))
    print(f"  {agreeing * 100:.1f}% of them land inside the SDF extract, "
          f"to within a voxel")
    if agreeing < 0.95:
        raise SystemExit("the two representations no longer agree")

    eye, target = R.voxel_camera(extract, VOXEL_SIZE, azimuth=34.0, elevation=24.0)
    R.render_voxels(extract, "25_extract_voxels.png", eye=eye, target=target)

    # --- refusals ------------------------------------------------------------
    # A mask that misses the surface is the common mistake, and an empty result
    # would read as a bug in the painting rather than in the aim.
    for label, build in (
            ("an empty mask", lambda: clay.MaskField(VOXEL_SIZE)),
            ("a mask nowhere near the surface", _far_mask)):
        try:
            doc.mask_extrude(build(), thickness=0.12)
        except ValueError as exc:
            print(f"  {label:<32} -> refused ({str(exc).split(':')[0]})")
        else:
            raise SystemExit(f"{label} should not have produced an extract")

    R.export_model(with_plate(plate), "25_extract.ply", resolution=72)


def _far_mask():
    m = clay.MaskField(VOXEL_SIZE)
    m.fill(((4.0, 4.0, 4.0), (4.4, 4.4, 4.4)), 1.0)
    return m


if __name__ == "__main__":
    main()

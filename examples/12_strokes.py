"""The stroke engine: a drag becomes stamps, and a stamp becomes an edit.

Resolving a stroke is pure — samples and a preset go in, stamps come out, and
no document is read or touched. Applying the result produces *ordinary* edits:
voxel brush stamps, or nodes appended to a layer's edit list. That is the whole
design, and it is not a stylistic preference. Because a stroked edit is an
ordinary edit, undo, stroke coalescing, `.clayspace` serialization and picking
all apply to it without any of them knowing the stroke engine exists.

Two things this example asserts rather than merely shows:

  * Spacing follows the *path*, not the sample rate. The same gesture drawn
    with a slow finger and a fast one lays down the same stamps.
  * Jitter is a hash of the stamp index and a seed, never a random source, so
    a preset reproduces its stroke exactly — on every platform, through every
    binding, and in these committed renders.

Presets carry a schema version from the first release. A preset library
outlives the engine build that wrote it; a newer schema is refused rather than
read as a prefix and silently reinterpreted.
"""

import pathlib

import numpy as np

import pyclay as clay

import _render as R

VOXEL_SIZE = 0.05


def arc(turns=1.0, radius=1.0, count=120, pressure=None):
    """A helix-ish sweep, sampled densely, with optional pressure ramp."""
    t = np.linspace(0.0, turns * 2.0 * np.pi, count)
    xs = np.cos(t) * radius
    ys = np.linspace(-0.7, 0.7, count)
    zs = np.sin(t) * radius
    ps = np.linspace(0.15, 1.0, count) if pressure is None else np.full(count, pressure)
    return np.stack([xs, ys, zs, ps], axis=1).astype(np.float32)


def main():
    R.banner("12 strokes — samples in, edit items out")

    straight = np.array([[-1.2 + i * 0.04, 0.0, 0.0, 1.0] for i in range(61)], np.float32)

    # --- spacing follows the path, not the sample rate ----------------------
    preset = clay.StrokePreset(radius=0.18, spacing=0.5)
    sparse = preset.resolve(straight[::10])
    dense = preset.resolve(straight)
    same = (sparse["positions"].shape == dense["positions"].shape
            and np.allclose(sparse["positions"], dense["positions"], atol=1e-5))
    print(f"  6 samples and 61 samples over the same path both give "
          f"{len(dense['radii'])} stamps: {same}")
    if not same:
        raise SystemExit("spacing depends on the sample rate")

    # --- what the preset knobs do -------------------------------------------
    variants = [
        ("plain", clay.StrokePreset(radius=0.18, spacing=0.35)),
        ("tapered", clay.StrokePreset(radius=0.18, spacing=0.35,
                                      taper_start=0.3, taper_end=0.3)),
        ("pressure", clay.StrokePreset(radius=0.18, spacing=0.35, pressure_size=1.0)),
        ("jittered", clay.StrokePreset(radius=0.18, spacing=0.35,
                                       jitter_position=0.6, jitter_size=0.4, seed=5)),
    ]
    ramp = np.array([[-1.2 + i * 0.04, 0.0, 0.0, 0.1 + 0.9 * i / 60] for i in range(61)],
                    np.float32)

    tiles = []
    for name, p in variants:
        grid = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
        stamps = grid.apply_stroke(ramp, p, grid.palette_add("#5f93bf"))
        print(f"  {name:9s} {stamps:3d} stamps -> {grid.occupied_count:6d} cells")
        eye, target = R.voxel_camera(grid, VOXEL_SIZE, azimuth=28.0, elevation=22.0)
        tiles.append(R.render_voxels_array(grid, eye=eye, target=target,
                                           width=200, height=185))
    R.contact_sheet(tiles, "12_stroke_presets.png", columns=4,
                    caption=", ".join(n for n, _ in variants))

    # --- steady stroke -------------------------------------------------------
    shaky = np.array([[-1.2 + i * 0.04, 0.10 if i % 2 else -0.10, 0.0, 1.0]
                      for i in range(61)], np.float32)
    tiles = []
    for steady in (0.0, 0.5, 0.85):
        p = clay.StrokePreset(radius=0.12, spacing=0.35, steady=steady)
        grid = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
        grid.apply_stroke(shaky, p, grid.palette_add("#c85a4a"))
        wobble = float(np.abs(p.resolve(shaky)["positions"][:, 1]).mean())
        print(f"  steady {steady:<4} -> mean wobble {wobble:.4f}")
        eye, target = R.voxel_camera(grid, VOXEL_SIZE, azimuth=0.0, elevation=70.0)
        tiles.append(R.render_voxels_array(grid, eye=eye, target=target,
                                           width=260, height=180))
    R.contact_sheet(tiles, "12_stroke_steady.png", columns=3,
                    caption="steady stroke at 0.0, 0.5, 0.85, seen from above")

    # --- a stroked SDF edit is an ordinary edit ------------------------------
    doc = clay.Document()
    doc.enable_undo()
    body = doc.add_sdf_layer("body")
    sweep = arc()
    # pressure_strength is off here on purpose: an add stamp's strength scales
    # its whole deposit (issue #61), and this section is about undo grouping,
    # so the sweep's 0.15-pressure start should still deposit a full stamp for
    # the probe below to sit inside.
    p = clay.StrokePreset(radius=0.16, spacing=0.3, pressure_size=1.0,
                          pressure_strength=0.0,
                          taper_start=0.15, taper_end=0.15)
    ids = body.apply_stroke(sweep, p, clay.Sphere(r=1.0),
                            blend=clay.Smooth(0.12), color="#d8b26a")
    print(f"  {len(ids)} nodes appended as {doc.undo_depth - 1} undo step: "
          f"one stroke, not one per stamp")

    probe = np.array([[1.0, -0.7, 0.0]], np.float32)
    on = float(doc.eval(probe)[0])
    doc.undo()
    off = float(doc.eval(probe)[0])
    doc.redo()
    # Off the surface the field reads as "far away" rather than as a distance,
    # so report whether it is inside rather than printing the sentinel.
    print(f"  the probe is inside before the undo ({on:.3f}) and outside after")
    if not (on < 0 < off):
        raise SystemExit("the stroke did not undo as one step")

    R.render(doc, "12_stroke_sdf.png", eye=(2.6, 1.6, 3.0), target=(0, 0, 0),
             caption="one stroke, one undo step, ordinary edit-list nodes")

    # --- masks freeze what a stroke may author ------------------------------
    # An SDF item has no per-point strength, so a mask cannot gate it where it
    # is evaluated. It gates it here instead, where the item is authored: a
    # stamp in a frozen region simply produces no node.
    frozen = clay.Document()
    layer = frozen.add_sdf_layer("body")
    mask = frozen.add_mask("body", cell_size=VOXEL_SIZE)
    for x in range(0, 40):                       # world x in [0, 2)
        for y in range(-20, 21):
            for z in range(-30, 31):
                mask.set((x, y, z), 1.0)
    all_ids = clay.Document()
    open_layer = all_ids.add_sdf_layer("body")
    total = len(open_layer.apply_stroke(sweep, p, clay.Sphere(r=1.0)))
    gated = len(layer.apply_stroke(sweep, p, clay.Sphere(r=1.0), mask=mask))
    print(f"  a mask over the +x half turns {total} stamps into {gated} items")
    if not 0 < gated < total:
        raise SystemExit("the mask did not gate the stroke")

    # --- presets outlive the engine that wrote them -------------------------
    data = p.serialize()
    back = clay.StrokePreset.deserialize(data)
    print(f"  preset is {len(data)} bytes at schema version "
          f"{clay.StrokePreset.version}; round trip exact: "
          f"{back.serialize() == data}")
    newer = bytes([clay.StrokePreset.version + 1]) + data[1:]
    try:
        clay.StrokePreset.deserialize(newer)
        raise SystemExit("a newer preset schema was accepted")
    except ValueError:
        print("  a preset from a newer schema is refused, not reinterpreted")


    # --- the four knobs no page exercised -------------------------------------
    # docs/07 lists pressure.strength, pressure.curve, jitter_rotation and
    # rotate_along_stroke as brush features. Nothing measured them, which means
    # any of the four could have stopped reaching the resolver without a single
    # picture changing. Each is checked against the knob turned OFF, so the test
    # is "does this control anything" rather than "does it run".
    ramp_up = np.array([[-1.2 + i * 0.04, 0.0, 0.0, 0.05 + 0.95 * i / 60]
                        for i in range(61)], np.float32)
    base = clay.StrokePreset(radius=0.18, spacing=0.35)

    # pressure_strength scales each stamp's strength off the pressure channel.
    # Note the asymmetry, which is easy to get backwards: pressure_SIZE defaults
    # to 0 (off) and pressure_STRENGTH to 1 (on), so a default preset already
    # varies strength with pressure and only the radius needs opting in.
    off = clay.StrokePreset(radius=0.18, spacing=0.35,
                            pressure_strength=0.0).resolve(ramp_up)["strengths"]
    on = clay.StrokePreset(radius=0.18, spacing=0.35,
                           pressure_strength=1.0).resolve(ramp_up)["strengths"]
    if np.allclose(off, on):
        raise SystemExit("pressure_strength changed no stamp strength")
    if not (on[0] < on[-1]):
        raise SystemExit("pressure rose along the stroke but strength did not follow")
    print(f"  pressure_strength: first/last stamp strength {on[0]:.3f} -> {on[-1]:.3f}")

    # pressure_curve is the exponent on the normalized channel, so it bends the
    # ramp without changing its endpoints.
    curved = clay.StrokePreset(radius=0.18, spacing=0.35, pressure_size=1.0,
                               pressure_curve=2.5).resolve(ramp_up)["radii"]
    linear = clay.StrokePreset(radius=0.18, spacing=0.35,
                               pressure_size=1.0).resolve(ramp_up)["radii"]
    if np.allclose(curved, linear):
        raise SystemExit("pressure_curve did not bend the pressure ramp")
    mid = len(linear) // 2
    if not curved[mid] < linear[mid]:
        raise SystemExit("an exponent above 1 should pull the middle of the ramp down")
    print(f"  pressure_curve 2.5 vs 1.0 at mid-stroke: {curved[mid]:.3f} vs {linear[mid]:.3f}")

    # jitter_rotation and rotate_along_stroke turn each stamp, and `resolve`
    # reports no angle — rotation is only OBSERVABLE through a stamp that is
    # not rotationally symmetric, which is what docs/07 says about both knobs.
    # So these use a BOX stamp and compare the resulting fields; a sphere stamp
    # would make a working knob look dead.
    def stroked(preset, samples, prim):
        d = clay.Document()
        d.add_sdf_layer("l").apply_stroke(samples, preset, prim)
        rng = np.random.default_rng(4)
        pts = rng.uniform(-1.5, 1.5, size=(4000, 3)).astype(np.float32)
        return d.eval(pts)

    box = clay.Box(size=(0.20, 0.07, 0.07))
    plain = stroked(base, ramp_up, box)

    jr = clay.StrokePreset(radius=0.18, spacing=0.35, jitter_rotation=1.0, seed=5)
    jittered = stroked(jr, ramp_up, box)
    if np.allclose(jittered, plain, atol=1e-6):
        raise SystemExit("jitter_rotation rotated nothing")
    if not np.allclose(jittered, stroked(jr, ramp_up, box), atol=0):
        raise SystemExit("jitter_rotation is not deterministic — two resolves disagreed")
    other = clay.StrokePreset(radius=0.18, spacing=0.35, jitter_rotation=1.0, seed=6)
    if np.allclose(jittered, stroked(other, ramp_up, box), atol=1e-6):
        raise SystemExit("two seeds gave the same rotations")
    print("  jitter_rotation: turns the stamps, deterministic per seed, two seeds differ")

    # rotate_along_stroke needs a CURVED path: on a straight line every tangent
    # is identical and the knob would look dead while working correctly.
    curve = arc(turns=0.5, radius=0.9, count=90)
    follow = clay.StrokePreset(radius=0.14, spacing=0.35, rotate_along_stroke=True)
    fixed = clay.StrokePreset(radius=0.14, spacing=0.35)
    if np.allclose(stroked(follow, curve, box), stroked(fixed, curve, box), atol=1e-6):
        raise SystemExit("rotate_along_stroke did not turn the stamps along the path")
    # ...and on a STRAIGHT path it must change nothing, which is the other half
    # of the claim and the half a curved-only check would let rot.
    if not np.allclose(stroked(follow, ramp_up, box),
                       stroked(fixed, ramp_up, box), atol=1e-5):
        raise SystemExit("rotate_along_stroke turned stamps on a straight path")
    print("  rotate_along_stroke: turns stamps along an arc, leaves a straight path alone")

    # accumulation: BUILDUP lets overlapping stamps deposit twice, CLAMPED
    # reaches the stroke's strength once however many overlap. A dense stroke
    # is the case that separates them — at wide spacing nothing overlaps and
    # the two are identical, which is the trap in testing this at all.
    dense = clay.StrokePreset(radius=0.18, spacing=0.12, accumulation=clay.Accumulation.BUILDUP)
    clamped = clay.StrokePreset(radius=0.18, spacing=0.12, accumulation=clay.Accumulation.CLAMPED)
    b = dense.resolve(ramp_up)["strengths"]
    c = clamped.resolve(ramp_up)["strengths"]
    if np.allclose(b, c):
        raise SystemExit("buildup and clamped accumulation resolved identically")
    if not b.max() >= c.max():
        raise SystemExit("clamped deposited more than buildup")
    print(f"  accumulation: buildup peaks at {b.max():.3f}, clamped at {c.max():.3f}")

    # Coverage: a preset field nothing exercises is a brush knob nobody checks.
    src = pathlib.Path(__file__).read_text()
    fields = {a for a in dir(clay.StrokePreset)
              if not a.startswith("_") and not callable(getattr(clay.StrokePreset, a, None))}
    missing = {f for f in fields if f not in src}
    if missing:
        raise SystemExit(f"StrokePreset fields with no example: {sorted(missing)}")
    print(f"  covered all {len(fields)} StrokePreset fields")

    R.export_model(doc, "12_stroke.ply", resolution=64)


if __name__ == "__main__":
    main()

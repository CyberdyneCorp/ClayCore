"""ClayBuildup and Smooth — building mass, then softening it.

The two brushes an artist reaches for first, and the pair that makes a blockout:
deposit material along a stroke, then smooth what you deposited.

**ClayBuildup is `Op::Relief` carried along a stroke.** Relief is the op whose
item is a *region* rather than geometry — it offsets the accumulated field by an
amplitude wherever the region weighs, so the surface already there moves along
its own normal. Put one under every stamp of a stroke and the surface builds up
along the path, which is the brush. The stroke engine's `accumulation` is the
buildup control ZBrush spells the same way: `Buildup` lets overlapping stamps
act twice, `Clamped` divides each stamp's strength by the expected overlap so a
stroke reaches its depth once.

**Smooth is `field::relax`.** It bakes: sample the field into a narrow-band
volume and average it under a region. Averaging destroys *exactness* — the field
no longer reports the true distance to its own surface — but it cannot break the
**Lipschitz** bound, because an average cannot vary faster than the thing it
averages, and a 1-Lipschitz field is automatically a conservative bound on the
distance to its own zero set. So the raymarcher stays correct, which the step
scale in the last section shows.

Two things are worth reading before looking.

**Rounding is the relief falloff, and a stroke has to carry it.** Building this
found that `apply_stroke` dropped it: relief then declared an amplitude over
~1e-6, the step scale collapsed to zero, and the geometry was there but nothing
could march it. The fix is the `rounding` argument used below, and the first
section prints the step scale so a regression is visible rather than silent.

**Every relief stamp costs the marcher, so a stroke is a budget.** The cost is
amplitude over falloff width, per stamp, compounded. A dense stroke of deep
stamps through a narrow falloff is the expensive corner; the table in the first
section is what to steer by.
"""

import numpy as np

import pyclay as clay

import _render as R


def ball(colour="#b0784a"):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.7), color=colour)
    return doc, layer


def ridge(amplitude=0.06, rounding=0.30, spacing=0.6, accumulation="buildup",
          radius=0.16, n=9):
    """A ClayBuildup stroke: relief stamps along a path across the top."""
    doc, layer = ball()
    samples = np.array([[x, 0.72, 0.0, 1.0, 0.0] for x in np.linspace(-0.35, 0.35, n)],
                       np.float32)
    preset = clay.StrokePreset()
    preset.radius = radius
    preset.spacing = spacing
    preset.accumulation = (clay.Accumulation.BUILDUP if accumulation == "buildup"
                           else clay.Accumulation.CLAMPED)
    stamps = layer.apply_stroke(samples, preset, clay.Sphere(r=1.0), op=clay.Op.RELIEF,
                                blend=clay.Smooth(amplitude), rounding=rounding)
    return doc, len(stamps)


def top(doc, x, z=0.0):
    ys = np.arange(1.4, -0.2, -0.002, dtype=np.float32)
    pts = np.stack([np.full_like(ys, x), ys, np.full_like(ys, z)], axis=1)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(ys[inside[0]]) if len(inside) else float("nan")


def main():
    R.banner("29 ClayBuildup and Smooth — building mass, then softening it")

    EYE, TARGET = (2.0, 1.5, 2.2), (0, 0.35, 0)
    plain, _ = ball()
    base = top(plain, 0.0)

    # --- ClayBuildup: relief carried along a stroke ---------------------------
    print(f"  the ball's top is at y = {base:.3f}; building a ridge across it")
    print(f"    {'amplitude':>10}{'falloff':>9}{'spacing':>9}{'stamps':>8}"
          f"{'ridge':>8}{'step scale':>12}")
    tiles, labels = [], []
    for amp, rnd, sp in ((0.12, 0.14, 0.35), (0.08, 0.22, 0.5), (0.06, 0.30, 0.6),
                         (0.04, 0.40, 1.0)):
        doc, stamps = ridge(amp, rnd, sp)
        print(f"    {amp:>10}{rnd:>9}{sp:>9}{stamps:>8}{top(doc, 0.0) - base:>+8.3f}"
              f"{doc.safe_step_scale():>12.4f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"amp {amp}, falloff {rnd}")
    R.contact_sheet(tiles, "29_claybuildup_stroke.png", columns=4, caption=", ".join(labels) +
                    " — deeper stamps through a narrower falloff cost the marcher more")

    # The regression this example exists to keep visible: rounding IS the relief
    # falloff, and a stroke that drops it declares an amplitude over ~1e-6.
    lost, _ = ridge(0.06, 0.0, 0.6)
    kept, _ = ridge(0.06, 0.30, 0.6)
    print(f"  a stroke that loses its rounding declares a step scale of "
          f"{lost.safe_step_scale():.4f} against {kept.safe_step_scale():.4f}")
    if kept.safe_step_scale() <= lost.safe_step_scale():
        raise SystemExit("rounding stopped reaching the stroke's relief stamps")

    # --- buildup against clamped ---------------------------------------------
    # The same control ZBrush spells the same way: overlapping stamps either act
    # twice or reach the depth once.
    dense = dict(amplitude=0.05, rounding=0.30, spacing=0.25, n=13)
    build, n_build = ridge(accumulation="buildup", **dense)
    clamp, _ = ridge(accumulation="clamped", **dense)
    print(f"  {n_build} overlapping stamps: buildup lifts {top(build, 0.0) - base:+.3f}, "
          f"clamped {top(clamp, 0.0) - base:+.3f}")
    if top(build, 0.0) <= top(clamp, 0.0):
        raise SystemExit("buildup stopped accumulating past clamped")

    # --- Smooth: relax over what was built ------------------------------------
    # The classic follow-up. Relax BAKES, so this samples the ridge into a volume
    # and averages it under a region.
    #
    # The control is the averaging RADIUS against the size of the feature, not
    # the pass count. A kernel much smaller than the ridge barely touches it
    # however many passes it runs — measured, 4 cells at 0.012 (0.048 world)
    # against a ridge 0.16 wide left the prominence where it started. Sized to
    # the feature it softens immediately, and past it the ridge is gone.
    built, _ = ridge(0.08, 0.22, 0.5)
    CELL = 0.02
    rough = clay.Volume.from_document(built, cell=CELL)
    print("  smoothing the ridge — the averaging radius against the feature size:")
    print(f"    {'radius_cells':>13}{'world':>8}{'passes':>8}{'ridge':>9}"
          f"{'lipschitz':>11}{'step scale':>12}")
    doc0 = clay.Document()
    doc0.add_sdf_layer("s").add(rough, color="#b0784a")
    tiles = [R.render_array(doc0, eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["built"]
    lifts = [top(doc0, 0.0) - base]
    print(f"    {'-':>13}{'-':>8}{0:>8}{lifts[0]:>+9.3f}"
          f"{rough.sample_lipschitz:>11.3f}{doc0.safe_step_scale():>12.3f}")
    for cells in (3, 6, 10):
        smoothed = rough.relaxed(radius_cells=cells, iterations=4, centre=(0, 0.80, 0),
                                 region_radius=0.55, falloff=0.2)
        doc = clay.Document()
        doc.add_sdf_layer("s").add(smoothed, color="#b0784a")
        lifts.append(top(doc, 0.0) - base)
        print(f"    {cells:>13}{cells * CELL:>8.2f}{4:>8}{lifts[-1]:>+9.3f}"
              f"{smoothed.sample_lipschitz:>11.3f}{doc.safe_step_scale():>12.3f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"radius {cells * CELL:.2f}")
    R.contact_sheet(tiles, "29_smooth_passes.png", columns=4, caption=", ".join(labels) +
                    " — a kernel sized to the ridge softens it; a wider one removes it")

    # A wider kernel must soften more, monotonically.
    if not all(a >= b - 1e-3 for a, b in zip(lifts, lifts[1:])):
        raise SystemExit(f"a wider averaging radius stopped softening more: {lifts}")

    # Averaging destroys EXACTNESS but cannot RAISE the LIPSCHITZ bound, so the
    # raymarcher stays correct — which is why the step scale does not collapse.
    #
    # The comparison is against the SOURCE, not against 1. A relief through a
    # narrow rounding is a steep field by construction, so the samples `rough`
    # took from it are steep before relax sees them; what relax owes is not to
    # make them worse.
    smoothed = rough.relaxed(radius_cells=6, iterations=4, centre=(0, 0.80, 0),
                             region_radius=0.55, falloff=0.2)
    print(f"  relax cannot raise the bound: {rough.sample_lipschitz:.3f} in, "
          f"{smoothed.sample_lipschitz:.3f} out, so the step scale holds at "
          f"{doc0.safe_step_scale():.3f}")
    if smoothed.sample_lipschitz > rough.sample_lipschitz + 1e-3:
        raise SystemExit("relax raised the Lipschitz bound, which it must not")

    # --- the pair, side by side -----------------------------------------------
    final = clay.Document()
    final.add_sdf_layer("s").add(
        rough.relaxed(radius_cells=6, iterations=4, centre=(0, 0.80, 0),
                      region_radius=0.55, falloff=0.2), color="#b0784a")
    R.contact_sheet(
        [R.render_array(plain, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(built, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(final, eye=EYE, target=TARGET, width=205, height=195)],
        "29_claybuildup_smooth.png", columns=3,
        caption="the ball, a ClayBuildup stroke across it, and the same after Smooth — "
                "the blockout pair")

    R.export_model(built, "29_claybuildup.ply", resolution=72, decimate=0.08)


if __name__ == "__main__":
    main()

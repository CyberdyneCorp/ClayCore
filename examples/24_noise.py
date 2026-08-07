"""A noise field — irregularity, which the engine had none of.

`add-magnify-pinch` carved Blob out of its row and recorded it as blocked on
this. Blob's character is an *irregular* surface response; the nearest thing
here was the `displace` deformer, whose sine is regular by construction and
gives an even corrugation — precisely what Blob is not. Noise is also what
weathering, erosion and organic break-up all want.

**The design question was which noise, and a number settled it.** Cross-backend
parity here is tolerance-based rather than bit-exact: 1e-6 relative on the CPU
backends, 1e-4 on the GPU ones. That is comfortable for ordinary arithmetic and
fatal for the usual float hash. `fract(sin(dot(p, k)) * 43758.5453)` takes
whatever `sin` does differently between libm, CUDA, Metal and OpenCL — a few
units in the last place — multiplies it by forty-three thousand, and then takes
a fractional part, which is chaotic by construction. A 1e-7 disagreement becomes
an O(1) disagreement. It would fail parity on the first case.

So the hash is **integer**: the same integer operations give the same bits
everywhere. That decided the noise too — gradient noise on an integer lattice —
and it is why the kernel shim now carries an unsigned integer type, which no
other kernel header had ever needed.

Three things are worth reading before looking.

**The seed is a parameter, not global state.** Two items with the same seed look
the same, and an item's appearance never depends on the order it was compiled
in. The second section shows the same shape under three seeds and asserts a
re-run reproduces one exactly.

**Octaves add detail, not size.** The fractal is normalized by its own weight, so
raising the octave count sharpens the surface without growing how far it moves.
Otherwise `octaves` and `amplitude` would be two controls for one thing and
neither would mean anything.

**Offsetting the distance costs the marcher.** The field stops being exact and
its slope grows with amplitude times frequency, summed over the octaves — each
octave has twice the frequency of the last. The tape carries it and the step
scale drops, which the last section prints.
"""

import numpy as np

import pyclay as clay

import _render as R


def rock(**noise):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    body = clay.Sphere(r=0.7)
    if noise:
        body.noise(**noise)
    layer.add(body, color="#b0784a")
    return doc


def roughness(doc, r=0.7, samples=3000):
    """How far the surface wanders from the sphere it started as."""
    rng = np.random.default_rng(3)
    d = rng.normal(size=(samples, 3))
    d /= np.linalg.norm(d, axis=1)[:, None]
    return float(np.std(doc.eval((d * r).astype(np.float32))))


def main():
    R.banner("24 noise — irregularity, and the hash that makes it reproducible")

    EYE, TARGET = (2.2, 1.5, 2.3), (0, 0, 0)

    # --- amplitude and octaves are different controls -------------------------
    print("  amplitude moves the surface; octaves sharpen it:")
    tiles = [R.render_array(rock(), eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["smooth"]
    for amp, octaves in ((0.05, 1), (0.05, 5), (0.11, 5)):
        doc = rock(amplitude=amp, frequency=5.0, octaves=octaves, seed=17)
        print(f"    amplitude {amp:<5} octaves {octaves} -> roughness "
              f"{roughness(doc):.4f}, step scale {doc.safe_step_scale():.3f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"amp {amp}, {octaves} oct")
    R.contact_sheet(tiles, "24_noise_octaves.png", columns=4, caption=", ".join(labels))

    # Octaves add detail without growing the excursion — the normalization.
    one = rock(amplitude=0.05, frequency=5.0, octaves=1, seed=17)
    five = rock(amplitude=0.05, frequency=5.0, octaves=5, seed=17)
    rng = np.random.default_rng(8)
    probes = rng.uniform(-1.1, 1.1, size=(5000, 3)).astype(np.float32)
    base = rock().eval(probes)
    widest = [float(np.abs(d.eval(probes) - base).max()) for d in (one, five)]
    print(f"  widest excursion: {widest[0]:.4f} at one octave, {widest[1]:.4f} at five "
          f"(amplitude is 0.05)")
    if max(widest) > 0.05 + 1e-3:
        raise SystemExit("the fractal is not normalized — octaves are growing the excursion")

    # --- the seed is a parameter, not state ----------------------------------
    tiles, labels = [], []
    for seed in (1, 2, 3):
        tiles.append(R.render_array(rock(amplitude=0.09, frequency=5.0, octaves=4, seed=seed),
                                    eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"seed {seed}")
    R.contact_sheet(tiles, "24_noise_seeds.png", columns=3, caption=", ".join(labels))

    a = rock(amplitude=0.09, frequency=5.0, octaves=4, seed=1)
    again = rock(amplitude=0.09, frequency=5.0, octaves=4, seed=1)
    b = rock(amplitude=0.09, frequency=5.0, octaves=4, seed=2)
    same = bool(np.array_equal(a.eval(probes), again.eval(probes)))
    differs = float(np.abs(a.eval(probes) - b.eval(probes)).max())
    print(f"  the same seed reproduces exactly: {same}; a different seed differs by "
          f"up to {differs:.4f}")
    if not same or differs < 1e-3:
        raise SystemExit("the seed is not behaving as a plain parameter")

    # --- it is irregular, which is the whole point ---------------------------
    # A sine repeats at its own period. Noise does not, and that is the
    # difference between a corrugation and weathering.
    freq = 4.0
    noisy = rock(amplitude=0.1, frequency=freq, octaves=1, seed=11)
    sine = clay.Document()
    sl = sine.add_sdf_layer("l")
    wave = clay.Sphere(r=0.7)
    wave.displace(0.1, freq)
    sl.add(wave, color="#8d6a4f")

    # Offset by the SINE's own period, 2*pi/f — not by 1/f, which is the noise
    # lattice spacing and a distance at which the sine is under no obligation to
    # agree with itself. Compared at its true period the sine repeats exactly,
    # and the noise does not repeat at all: that is the whole distinction.
    # ...and measured on the DEFORMER's contribution alone, by subtracting the
    # undeformed sphere. Both of these offset the distance after the primitive,
    # so the difference is exactly the displacement. Comparing the raw fields
    # instead measures the sphere, whose own distance changes far more over a
    # period than either deformer does — which made both read the same.
    xs = np.arange(-0.45, 0.45, 0.004, dtype=np.float32)
    period = 2.0 * np.pi / freq
    smooth = rock()
    def repeat_gap(doc):
        p0 = np.stack([xs, np.full_like(xs, 0.72), np.zeros_like(xs)], 1)
        p1 = p0 + np.array([period, 0, 0], np.float32)
        d0 = doc.eval(p0) - smooth.eval(p0)
        d1 = doc.eval(p1) - smooth.eval(p1)
        return float(np.abs(d0 - d1).max())
    print(f"  one SINE period apart, the field disagrees with itself by "
          f"{repeat_gap(sine):.4f} (the sine displace) vs {repeat_gap(noisy):.4f} (noise)")
    if repeat_gap(noisy) < repeat_gap(sine) * 5.0:
        raise SystemExit("the noise is repeating like a sine — it is not irregular")

    R.contact_sheet(
        [R.render_array(sine, eye=EYE, target=TARGET, width=310, height=290),
         R.render_array(rock(amplitude=0.1, frequency=freq, octaves=4, seed=11),
                        eye=EYE, target=TARGET, width=310, height=290)],
        "24_noise_vs_sine.png", columns=2,
        caption="the displace deformer's sine, and noise at the same amplitude and "
                "frequency — regular against irregular")

    # --- what irregularity costs the marcher ---------------------------------
    print("  offsetting the distance costs the raymarcher:")
    for amp, freq_ in ((0.0, 5.0), (0.04, 5.0), (0.1, 5.0), (0.1, 12.0)):
        doc = rock(amplitude=amp, frequency=freq_, octaves=4, seed=17) if amp else rock()
        print(f"    amplitude {amp:<5} frequency {freq_:<5} -> step scale "
              f"{doc.safe_step_scale():.3f}")
    scales = [rock().safe_step_scale()] + [
        rock(amplitude=a, frequency=f, octaves=4, seed=17).safe_step_scale()
        for a, f in ((0.04, 5.0), (0.1, 5.0), (0.1, 12.0))]
    if not all(x >= y for x, y in zip(scales, scales[1:])):
        raise SystemExit("a rougher field stopped costing more")

    # And the consequence: a ray still lands on the noisy surface.
    doc = rock(amplitude=0.09, frequency=5.0, octaves=4, seed=17)
    rays = np.array([[3.0, y, 0, -1, 0, 0] for y in np.linspace(-0.3, 0.3, 9)], np.float32)
    hits = doc.raycast_many(rays)["hit"]
    print(f"  {int(hits.sum())} of {len(rays)} rays land on the noisy surface")
    if not hits.all():
        raise SystemExit("the raymarcher is stepping through the noisy surface")

    # --- and it is an ordinary deformer --------------------------------------
    carved = clay.Document()
    layer = carved.add_sdf_layer("l")
    weathered = clay.Box(size=(0.55, 0.55, 0.55))
    weathered.noise(amplitude=0.05, frequency=7.0, octaves=5, seed=4)
    layer.add(weathered, color="#7f8a94", rounding=0.06)
    layer.add(clay.Sphere(r=0.42, position=(0.4, 0.4, 0.4)), op=clay.Op.SUBTRACT)
    R.render(carved, "24_noise_weathered.png", eye=(2.2, 1.7, 2.2), target=TARGET,
             caption="a weathered box with a bite taken out — noise composes like any "
                     "other deformer")

    R.export_model(rock(amplitude=0.09, frequency=5.0, octaves=4, seed=17),
                   "24_noise.ply", resolution=64, decimate=0.05)


if __name__ == "__main__":
    main()

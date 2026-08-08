"""hPolish — planing a facet without filling what is beside it.

ZBrush's hPolish, Planar and the Trim family are the hard-surface flatten: they
only ever plane DOWN. That is the whole brush. Cutting *without* filling is what
leaves a crisp facet against untouched surface, and filling the hollows beside a
facet is precisely what a polish must not do.

Here it is `field::flatten` in **cut-only** mode. The three modes differ by one
clamp on the blend term — `plane - here` two-sided, `max(plane - here, 0)` to cut
only, `min(...)` to fill only — which is why it is a mode rather than three
brushes.

Two things are worth reading before looking.

**The clamp is the brush.** The first section puts the same plane through a form
carrying a bump above it and a hollow below it. Two-sided lands both on the
plane; cut-only planes the bump and leaves the hollow exactly where it was. That
difference is the entire distinction between Flatten and hPolish.

**One pass is clean; chaining passes is not, and this example says so rather
than hiding it.** A flatten BAKES — it samples the field into a narrow-band
volume. Sampling the *document* gives an exact source and a 1-Lipschitz result.
Sampling a volume does not: outside the band a volume reports a lower bound
rather than a distance, and the blend then works from the wrong value. The third
section measures a chain and shows it: the declared Lipschitz goes 1.00 → 14.0
on the second pass whatever the falloff, and by the third the form is visibly
corrupt rather than merely expensive.

So today hPolish is a **single-pass** verb on an SDF layer. Polishing several
faces of a form wants either the cut tool (an Intersect against a prism is exact
and stays exact) or a consolidation step the engine does not yet have — the same
gap the Move brush hits from the other side.
"""

import numpy as np

import pyclay as clay

import _render as R


def bumpy():
    """A ball with a bump above the plane and a hollow below it, on one flank."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.62), color="#9aa3ad")
    layer.add(clay.Sphere(r=0.22, position=(0.52, 0.26, 0)), color="#9aa3ad")
    layer.add(clay.Sphere(r=0.20, position=(0.52, -0.24, 0)), op=clay.Op.SUBTRACT)
    return doc


def wrap(volume, colour="#9aa3ad"):
    doc = clay.Document()
    doc.add_sdf_layer("f").add(volume, color=colour)
    return doc


def polished(source, normal, distance, mode, falloff=0.12, radius=0.62, cell=0.010):
    n = np.array(normal, np.float32)
    n = n / np.linalg.norm(n)
    point = tuple(map(float, n * distance))
    return clay.Volume.flattened_from(
        source, plane_point=point, plane_normal=tuple(map(float, n)), strength=1.0,
        centre=point, region_radius=radius, falloff=falloff, cell=cell, band=0.09,
        mode=mode)


def along(doc, direction, hi=1.4):
    u = np.array(direction, np.float32)
    u = u / np.linalg.norm(u)
    ts = np.arange(hi, 0.0, -0.002, dtype=np.float32)
    pts = (ts[:, None] * u[None, :]).astype(np.float32)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(ts[inside[0]]) if len(inside) else float("nan")


def main():
    R.banner("28 hPolish — planing a facet without filling what is beside it")

    EYE, TARGET = (2.3, 1.3, 2.0), (0.15, 0, 0)
    PLANE_X = 0.46

    # --- the clamp is the brush ----------------------------------------------
    src = bumpy()
    print(f"  the flank: bump reaches {along(src, (1, 0.5, 0)):.3f}, "
          f"hollow sits at {along(src, (1, -0.45, 0)):.3f}; planing to x = {PLANE_X}")

    tiles = [R.render_array(src, eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["untouched"]
    results = {}
    for mode in ("two_sided", "cut", "fill"):
        doc = wrap(polished(src, (1, 0, 0), PLANE_X, mode))
        results[mode] = doc
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(mode)
    R.contact_sheet(tiles, "28_hpolish_modes.png", columns=4, caption=", ".join(labels) +
                    " — only cut-only leaves the hollow beside the facet alone")

    def probe(doc, y):
        return along(doc, (1, y, 0))

    print(f"    {'mode':<12}{'through the bump':>19}{'through the hollow':>21}")
    for name, doc in [("source", src)] + list(results.items()):
        print(f"    {name:<12}{probe(doc, 0.5):>19.3f}{probe(doc, -0.45):>21.3f}")

    # Cut-only must plane the bump and leave the hollow untouched.
    if abs(probe(results["cut"], -0.45) - probe(src, -0.45)) > 1e-3:
        raise SystemExit("cut-only moved the hollow — that is Flatten, not hPolish")
    if probe(results["cut"], 0.5) >= probe(src, 0.5) - 0.02:
        raise SystemExit("cut-only stopped planing the bump")

    # --- one pass is clean ----------------------------------------------------
    ball = clay.Document()
    ball.add_sdf_layer("l").add(clay.Sphere(r=0.62), color="#9aa3ad")
    one = polished(ball, (1, 0, 0), 0.44, "cut", falloff=0.30, radius=0.7)
    facet = wrap(one)
    print(f"  a single facet on a sphere: surface at {along(facet, (1, 0, 0)):.3f} "
          f"(plane 0.440), sample lipschitz {one.sample_lipschitz:.2f}, "
          f"step scale {facet.safe_step_scale():.3f}")
    if abs(along(facet, (1, 0, 0)) - 0.44) > 0.02:
        raise SystemExit("a single pass no longer lands the facet on the plane")

    # --- chaining passes is not, and the numbers say why ----------------------
    # A flatten BAKES. Sampling the DOCUMENT gives an exact source; sampling a
    # volume does not, because outside the band a volume reports a lower bound
    # rather than a distance — and the blend then works from the wrong value.
    print("  chaining passes samples a VOLUME rather than the document, and it shows:")
    print(f"    {'falloff':>8}" + "".join(f"{'pass ' + str(i):>9}" for i in (1, 2, 3)))
    chains = {}
    for falloff in (0.12, 0.30):
        cur, lips = ball, []
        for n in ((1, 0, 0), (0, 1, 0), (0, 0, 1)):
            v = polished(cur, n, 0.44, "cut", falloff=falloff, radius=0.7)
            lips.append(v.sample_lipschitz)
            cur = wrap(v)
        chains[falloff] = cur
        print(f"    {falloff:>8}" + "".join(f"{l:>9.2f}" for l in lips))
    print("  the jump is on the SECOND pass, whatever the falloff: that is the volume "
          "source, not the taper")

    # The claim this example exists to keep honest.
    first = polished(ball, (1, 0, 0), 0.44, "cut", falloff=0.30, radius=0.7)
    second = polished(wrap(first), (0, 1, 0), 0.44, "cut", falloff=0.30, radius=0.7)
    if not (second.sample_lipschitz > first.sample_lipschitz * 5.0):
        raise SystemExit("chaining stopped degrading — recheck whether it is now supported")

    R.contact_sheet(
        [R.render_array(facet, eye=(2.2, 1.5, 2.2), target=(0, 0, 0), width=205, height=195),
         R.render_array(wrap(second), eye=(2.2, 1.5, 2.2), target=(0, 0, 0),
                        width=205, height=195),
         R.render_array(chains[0.30], eye=(2.2, 1.5, 2.2), target=(0, 0, 0),
                        width=205, height=195)],
        "28_hpolish_chained.png", columns=3,
        caption="one pass is a crisp facet; two pulls the form concave; three breaks it "
                "— chaining bakes samples a volume rather than the document, so hPolish "
                "is a single-pass verb today")

    R.export_model(facet, "28_hpolish.ply", resolution=80, decimate=0.08)


if __name__ == "__main__":
    main()

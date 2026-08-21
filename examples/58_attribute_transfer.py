"""Getting the paint back after a round trip — and what does not come back.

A mesh layer's whole reason to exist is that it holds triangles somebody meant:
a retopology, a uv layout, painted colour. Sculpting one preserves all of that.
Anything that LEAVES it does not — sampling a model into a field and meshing it
back keeps the shape and discards the rest, because a distance field carries
neither colour nor uvs.

Most of that is refundable. The nearest point on the ORIGINAL surface knows
what belonged there, and the ray tree already returns the triangle and the
barycentrics needed to read it.

**What is not refundable is the topology**, and that is the headline rather
than a footnote. The mesh that comes back is the mesher's geometry: new
vertices, new edge loops, no relationship to the retopology that went in. This
script measures both halves — how well the colour comes back, and how
completely the topology does not — because a feature that refunds two thirds of
a loss should say which two thirds.
"""

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.3, 1.4, 2.3), (0.0, 0.0, 0.0)


def painted_source(resolution=44):
    """A form with colour that VARIES over the surface, so a transfer has
    something to get right or wrong rather than one flat value."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("src")
    layer.add(clay.Sphere(r=0.62), color="#b8452f")
    layer.add(clay.Sphere(r=0.34, position=(0.42, 0.30, 0.28)), color="#2f6fb8",
              blend=clay.Smooth(0.12))
    layer.add(clay.Capsule(a=(-0.55, -0.35, 0), b=(-0.15, 0.5, 0.2), r=0.14),
              color="#d8c24a", blend=clay.Smooth(0.10))
    return doc.mesh(resolution=resolution)


def through_the_field(mesh, cell=0.035, resolution=44):
    """The round trip: triangles -> sampled field -> triangles."""
    vol = clay.Volume.from_mesh(mesh, cell=cell)
    doc = clay.Document()
    doc.add_sdf_layer("resampled").add(vol, color="#b0b0b0")
    return doc.mesh(resolution=resolution)


def nearest_source_colour(target_p, src_p, src_c, sample=400):
    """The best answer available for a sample of target vertices: the colour of
    the nearest SOURCE vertex. Not the truth — the truth is the interpolated
    colour on the nearest triangle — but within a cell of it, and it needs no
    dependency."""
    idx = np.linspace(0, len(target_p) - 1, min(sample, len(target_p))).astype(int)
    picked = target_p[idx]
    # (sample, src) distances, in chunks so the matrix stays small
    out = np.empty((len(picked), 3), dtype=np.float64)
    for i in range(0, len(picked), 50):
        chunk = picked[i:i + 50]
        d = ((chunk[:, None, :] - src_p[None, :, :]) ** 2).sum(axis=2)
        out[i:i + 50] = src_c[d.argmin(axis=1)]
    return idx, out



def main():
    R.banner("58 attribute transfer — the paint comes back, the topology does not")

    src = painted_source()
    src_p, src_c = np.asarray(src.positions), np.asarray(src.colors)
    print(f"  source: {len(src_p)} vertices, "
          f"{len(np.asarray(src.indices))} triangles, coloured")

    back = through_the_field(src)
    back_p = np.asarray(back.positions)
    lost = np.asarray(back.colors).copy()
    print(f"  through the field: {len(back_p)} vertices — the shape survived and")
    print(f"  the colour did not: mean rgb {lost.mean(axis=0).round(3)} (one flat value)")

    report = back.transfer_attributes(src)
    got = np.asarray(back.colors)
    print(f"\n  transfer: {report['transferred']} vertices took a source colour, "
          f"{report['fell_back']} fell back")
    print(f"  the threshold was derived from the source's own size: "
          f"{report['max_distance']:.4f}")

    # --- half one: how well did the colour come back? ------------------------
    idx, expected = nearest_source_colour(back_p, src_p, src_c)
    err = np.abs(got[idx] - expected).max(axis=1)
    print(f"\n  per-vertex colour error against the nearest source vertex, over "
          f"{len(idx)} samples:")
    print(f"    median {np.median(err):.4f}   mean {err.mean():.4f}   "
          f"worst {err.max():.4f}")
    print("  The worst case is at a colour BORDER, where the nearest source vertex")
    print("  and the nearest source TRIANGLE can disagree — the measurement's own")
    print("  approximation, not the transfer's.")

    # --- half two: the topology did not come back ----------------------------
    # How many target vertices land exactly on a source vertex? On a genuine
    # retopology-preserving operation this would be all of them.
    shared = 0
    probe = np.linspace(0, len(back_p) - 1, 400).astype(int)
    for i in probe:
        d = ((src_p - back_p[i]) ** 2).sum(axis=1).min()
        if d < 1e-12:
            shared += 1
    print(f"\n  and the topology did NOT come back: of {len(probe)} sampled target")
    print(f"  vertices, {shared} coincide with a source vertex. The edge loops, the")
    print(f"  vertex count ({len(src_p)} -> {len(back_p)}) and any uv layout are the")
    print("  mesher's now. This refunds the paint, not the mesh.")

    # --- and a vertex the source never covered takes the fallback ------------
    far = clay.Document()
    far.add_sdf_layer("f").add(clay.Sphere(r=0.4, position=(9, 0, 0)))
    stray = far.mesh(resolution=16)
    stray_report = stray.transfer_attributes(src)
    print(f"\n  a mesh nowhere near the source: {stray_report['transferred']} transferred, "
          f"{stray_report['fell_back']} fell back —")
    print("  the report is how a host tells a good result from one that fell back")
    print("  everywhere, which is why it is returned rather than a bool.")

    # --- the pictures --------------------------------------------------------
    # render_mesh_array, not render_array: the ordinary preview traces a
    # DOCUMENT, and a mesh reaches one through Volume.from_mesh, which carries
    # a single colour for the whole item. A picture of what this feature does
    # has to come from the vertices.
    plain = through_the_field(src)
    tiles = [
        R.render_mesh_array(src, eye=EYE, target=TARGET, width=250, height=250),
        R.render_mesh_array(plain, eye=EYE, target=TARGET, width=250, height=250),
        R.render_mesh_array(back, eye=EYE, target=TARGET, width=250, height=250),
    ]
    R.contact_sheet(tiles, "58_attribute_transfer.png", columns=3,
                    caption="the painted source, the same shape through the field, "
                            "and the colours transferred back")


if __name__ == "__main__":
    main()

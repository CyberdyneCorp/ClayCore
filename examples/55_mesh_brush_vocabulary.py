"""The three mesh verbs the vocabulary was missing, and alphas on all of them.

`45`–`47` cover the eleven classical mesh verbs. Comparing that count against
ZBrush's ~36 overstates the gap — `flatten` carries three modes, so Blender's
Flatten/Fill/Scrape and ZBrush's Trim Dynamic/hPolish/Planar are already
covered, and many of the rest are alpha or falloff variants of one verb. Four
things were genuinely absent, and this page is about them.

**Relax** slides vertices *along* the surface to even their spacing. Smooth
moves toward the Laplacian average, which is inward on a convex region — that is
why smoothing shrinks. Relax takes the same target and keeps only its tangential
part.

It matters more here than in Blender or ZBrush, and for a reason particular to
this engine: **topology is fixed by contract**. No mesh brush adds a polygon, so
a large grab stretches the triangles it has, and `docs/sculpt_comparison.md`
names that as the point where the engine stops on purpose. Relax is what lets an
artist recover *without* a round trip through a retopo tool. Its value goes up
because polygons are never added.

It is not exactly shape-preserving, and this script measures that rather than
claiming otherwise: sliding along a *tangent plane* leaves a curved surface by a
second-order amount.

**Layer** deposits to a ceiling. Every other deposit verb accumulates, so a slow
stroke digs deeper than a fast one over the same path — the speed of your hand
becomes part of the result. Layer measures against the surface as the *stroke*
found it and stops at a fixed height.

**Nudge** pushes material along the surface. Grab carries the region rigidly, so
a drag with a component off the surface lifts material off it; nudge projects
into each vertex's own tangent plane.

**And alphas reach mesh brushes at all**, which they did not. Voxels have had
`sculpt_carve_alpha` since 0.24 and SDF items gained `Deformer::alpha`
recently — the mesh layer, the representation an artist reaches for *after* a
retopo pass when detail is exactly what they are adding, was the one that could
not stamp one. The alpha multiplies the brush's *weight*, so it composes with
every verb and every falloff without a line of per-verb code.
"""

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.2, 2.0, 2.4), (0.0, 0.35, 0.0)


def ball(resolution=40):
    """A CLOSED sphere, meshed from a field.

    Closed matters twice over. `Volume.from_mesh` — which the previews below go
    through — computes a signed distance from a winding number, and a winding
    number is undefined on an open sheet: the first version of this script used
    a height-field dome and rendered as confetti. And curvature matters, because
    on a FLAT surface the Laplacian displacement is already tangential, so relax
    and smooth are identical and comparing them proves nothing."""
    doc = clay.Document()
    doc.add_sdf_layer("src").add(clay.Sphere(r=1.0))
    return doc.mesh(resolution=resolution)


def copy_of(mesh):
    return clay.Mesh.from_triangles(np.array(mesh.positions, copy=True),
                                    np.array(mesh.indices, copy=True))


def surface_normals(p):
    """The sphere's analytic normal, so "how far did it move through the
    surface" is measured against the SHAPE rather than against the mesh's own
    stored normals, which the verbs are busy changing."""
    return p / np.linalg.norm(p, axis=1, keepdims=True)


def split_motion(before, after, centre, radius):
    near = np.linalg.norm(before - np.asarray(centre), axis=1) <= radius
    d = (after - before)[near]
    n = surface_normals(before[near])
    along = np.einsum("ij,ij->i", d, n)
    return float(np.abs(along).mean()), float(np.linalg.norm(d - n * along[:, None], axis=1).mean())


def edge_variation(mesh, centre, radius):
    """Coefficient of variation of edge length — how uneven the triangulation
    is, which is what relax exists to lower."""
    p = np.array(mesh.positions)
    idx = np.array(mesh.indices).reshape(-1, 3)
    a = p[idx[:, [0, 1, 2]]].reshape(-1, 3)
    b = p[idx[:, [1, 2, 0]]].reshape(-1, 3)
    mid = 0.5 * (a + b)
    near = np.linalg.norm(mid - np.asarray(centre), axis=1) <= radius
    length = np.linalg.norm((a - b)[near], axis=1)
    return float(length.std() / length.mean())


def preview(mesh, cell=0.012, colour="#8fa4c0"):
    """A DISPLAY-ONLY document: a mesh layer is never evaluated, so the renderer
    has nothing to trace against. Resampling here is exactly the approximation a
    mesh layer exists to avoid — fine for a picture, wrong for the export."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def main():
    R.banner("55 mesh brush vocabulary — relax, layer, nudge, and alphas")

    centre = (0.0, 1.0, 0.0)  # the sphere's north pole
    base = ball()
    print(f"  ball: {base.triangle_count} triangles, "
          f"{len(np.array(base.positions))} vertices")

    # --- relax: the verb fixed topology makes valuable ----------------------
    stretched = ball()
    clay.MeshSculptor(stretched).stamp("grab", center=centre, radius=0.7,
                                       direction=(0.25, 0, 0))
    start = np.array(stretched.positions, copy=True)
    before = edge_variation(stretched, centre, 0.45)
    print(f"\n  a big grab stretches the triangles it has — no brush here adds one.")
    print(f"  edge-length variation in the region: {before:.4f}")

    print(f"\n  {'verb':<8}{'edge variation':>16}{'moved THROUGH':>16}{'moved ACROSS':>15}")
    results = {}
    for verb in ("relax", "smooth"):
        m = copy_of(stretched)
        sc = clay.MeshSculptor(m)
        for _ in range(8):
            sc.stamp(verb, center=centre, radius=0.5, strength=1.0, smooth_iterations=2)
        through, across = split_motion(start, np.array(m.positions), centre, 0.45)
        results[verb] = (m, edge_variation(m, centre, 0.45), through, across)
        print(f"  {verb:<8}{results[verb][1]:>16.4f}{through:>16.5f}{across:>15.5f}")

    if results["relax"][2] >= results["smooth"][2] * 0.6:
        raise SystemExit("relax must move the surface far less than smooth")
    if results["relax"][1] >= before:
        raise SystemExit("relax must even the triangulation")
    print("    Both even the triangulation. Relax does it while moving the SURFACE")
    print("    a fraction as far — its motion is mostly ACROSS rather than through.")

    # ...and the size of that advantage is not a constant, which is worth
    # knowing rather than quoting one number. Smooth's normal component scales
    # with curvature times the SQUARE of the edge length, so it shrinks on a
    # fine mesh — the two verbs converge as triangles get small.
    print("\n  and the advantage depends on how coarse the mesh is:")
    print(f"    {'resolution':>11}{'triangles':>11}{'relax':>10}{'smooth':>10}{'ratio':>8}")
    for res in (28, 40, 56, 80):
        st = ball(res)
        clay.MeshSculptor(st).stamp("grab", center=centre, radius=0.7, direction=(0.25, 0, 0))
        s0 = np.array(st.positions, copy=True)
        got = {}
        for verb in ("relax", "smooth"):
            m = copy_of(st)
            sc = clay.MeshSculptor(m)
            for _ in range(8):
                sc.stamp(verb, center=centre, radius=0.5, strength=1.0, smooth_iterations=2)
            got[verb] = split_motion(s0, np.array(m.positions), centre, 0.45)[0]
        print(f"    {res:>11}{st.triangle_count:>11}{got['relax']:>10.5f}"
              f"{got['smooth']:>10.5f}{got['smooth'] / max(got['relax'], 1e-9):>7.1f}x")
    print("    Smooth's normal motion scales with curvature times the SQUARE of the")
    print("    edge length, so the two converge as triangles shrink. Relax earns")
    print("    the most exactly where it is needed most — on a coarse mesh, which")
    print("    is the one that stretches visibly when topology cannot change.")

    # --- layer: a ceiling, not an accumulation ------------------------------
    print("\n  layer stops; draw does not (same strength, same place):")
    print(f"    {'stamps':>8}{'layer':>10}{'draw':>10}")
    base_p = np.array(base.positions, copy=True)

    def deposit(verb, stamps):
        m = ball()
        sc = clay.MeshSculptor(m)
        record = clay.VertexDeltas()
        for _ in range(stamps):
            sc.stamp(verb, center=centre, radius=0.5, strength=0.15, layer_height=0.08,
                     deposit_normal=(0, 1, 0), deltas=record)
        return split_motion(base_p, np.array(m.positions), centre, 0.1)[0], m

    layer_mesh = draw_mesh = None
    for stamps in (1, 3, 6, 12):
        lay, layer_mesh = deposit("layer", stamps)
        drw, draw_mesh = deposit("draw", stamps)
        print(f"    {stamps:>8}{lay:>10.4f}{drw:>10.4f}")
        if lay > 0.08 + 1e-4:
            raise SystemExit("layer passed its ceiling")
    print("    Layer converges on its 0.08 ceiling; draw keeps climbing. That is")
    print("    what makes a slow stroke and a fast one over the same path agree.")

    # --- nudge stays on the surface -----------------------------------------
    print("\n  nudge vs grab, given a drag half of which points off the surface:")
    for verb in ("nudge", "grab"):
        m = ball()
        clay.MeshSculptor(m).stamp(verb, center=centre, radius=0.5,
                                   direction=(0.15, 0.15, 0))
        through, across = split_motion(base_p, np.array(m.positions), centre, 0.45)
        print(f"    {verb:<6} through the surface {through:.5f}   across it {across:.5f}")
        if verb == "nudge" and through > across * 0.35:
            raise SystemExit("nudge must move material along the surface")

    # --- alphas, on a verb that never had them ------------------------------
    # The stamp multiplies the WEIGHT, so it composes with every verb and every
    # falloff without per-verb code — and it is sampled by the same kernel
    # function the SDF alpha uses, so one stamp reads the same on both.
    def rivets(n=64, count=4):
        y, x = np.mgrid[0:n, 0:n].astype(np.float32)
        step = n / count
        u, v = (x % step) - step * 0.5, (y % step) - step * 0.5
        d = np.sqrt(u * u + v * v) / (step * 0.3)
        return np.clip(1.0 - d, 0.0, 1.0).astype(np.float32) ** 0.6

    stamps = {
        "none": None,
        "rivets": rivets(),
        "half": np.concatenate([np.zeros((64, 32)), np.ones((64, 32))], axis=1).astype(np.float32),
    }
    print("\n  the same draw, through three alphas:")
    tiles_alpha = {}
    for name, a in stamps.items():
        m = ball()
        moved = clay.MeshSculptor(m).stamp(
            "draw", center=centre, radius=0.65, strength=0.35, alpha=a,
            alpha_direction=(0, 1, 0), alpha_tangent=(1, 0, 0), deposit_normal=(0, 1, 0))
        tiles_alpha[name] = m
        print(f"    {name:<8} {moved:>6} classes moved")
    if tiles_alpha["half"] is not None:
        p0 = base_p
        p1 = np.array(tiles_alpha["half"].positions)
        d = np.linalg.norm(p1 - p0, axis=1)
        if d[p0[:, 0] < -0.15].max() != 0.0:
            raise SystemExit("the masked half of the alpha must not move")
        print("    The half stamp moved the +u side and left the -u side EXACTLY")
        print("    alone — the shape of the alpha reaches the mesh, not just its scale.")

    # --- the pictures --------------------------------------------------------
    # RELAX IS NOT IN THEM, and that is a fact about the preview rather than a
    # framing problem. These previews go through `Volume.from_mesh`, which
    # resamples the mesh into a field — so the triangulation, which is the only
    # thing relax changes, is thrown away before anything is drawn. Its evidence
    # is the table above; a render here would show two identical spheres and
    # imply the verb does nothing.
    #
    # What DOES render is the deposit verbs and the alphas, so those are the
    # tiles.
    tiles = [
        R.render_array(preview(base), eye=EYE, target=TARGET, width=250, height=250),
        R.render_array(preview(deposit("layer", 12)[1]), eye=EYE, target=TARGET,
                       width=250, height=250),
        R.render_array(preview(deposit("draw", 12)[1]), eye=EYE, target=TARGET,
                       width=250, height=250),
        R.render_array(preview(tiles_alpha["rivets"]), eye=EYE, target=TARGET,
                       width=250, height=250),
        R.render_array(preview(tiles_alpha["half"]), eye=EYE, target=TARGET,
                       width=250, height=250),
    ]
    R.contact_sheet(tiles, "55_mesh_brush_vocabulary.png", columns=5,
                    caption="untouched, then 12 stamps of layer and 12 of draw; "
                            "draw through a rivet alpha and a half alpha")
    print("\n  Relax is deliberately NOT in the contact sheet: the previews resample")
    print("  the mesh into a field, which throws away the triangulation — the only")
    print("  thing relax changes. The table above is its evidence; a render would")
    print("  show two identical spheres and imply the verb does nothing.")

    # --- the contract still holds -------------------------------------------
    for verb in ("relax", "layer", "nudge"):
        m = ball(64)
        before_p = np.array(m.positions, copy=True)
        before_i = np.array(m.indices, copy=True)
        sc = clay.MeshSculptor(m)
        record = clay.VertexDeltas()
        for _ in range(4):
            sc.stamp(verb, center=centre, radius=0.5, strength=0.6, direction=(0.1, 0.05, 0),
                     layer_height=0.05, deltas=record)
        if not np.array_equal(np.array(m.indices), before_i):
            raise SystemExit(f"{verb} changed the topology")
        record.revert(sc)
        if not np.array_equal(np.array(m.positions), before_p):
            raise SystemExit(f"{verb} did not revert bit-exactly")
    print("\n  topology never changed and every stroke reverted bit-exactly —")
    print("  the contract the other eleven verbs already live under.")


if __name__ == "__main__":
    main()

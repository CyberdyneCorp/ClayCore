"""The compositions — clay, crease, scrape, polish, snakehook.

`45_mesh_brushes` has the six primitives. These five are built out of them, and
the reason each one is a VERB rather than a recipe is the same reason
`VoxelGrid.sculpt_scrape` is: the halves are applied inside ONE stamp, against
ONE snapshot of the surface taken before anything moved. Run them in sequence
instead and the second half acts on what the first half already did, which is a
different operation and a worse one.

    clay       draw's deposit CLAMPED to a plane floating at the stamp height.
               The clamp is the brush: it fills up TO the plane and no further,
               so what it leaves is a flat-topped strip instead of a swell that
               carries the bumps underneath it along. On a flat surface it is
               draw; the difference is what it does to an uneven one.

    crease     a tight negative draw AND a pinch, summed inside one stamp. The
               cut and the squeeze together are what close the fold —
               sequenced, the pinch would gather vertices the draw had already
               pushed down, and you get a rounded ditch.

    scrape     flatten cut-only AND smooth, from one snapshot. Cut-only means
               material above the plane goes and the hollows beside it are left
               alone, which is what leaves a crisp facet.

    polish     smooth GATED by dihedral angle: full strength where the faces
               around a vertex agree, fading to nothing where they disagree. A
               hard edge disagrees, so it survives a pass that removes the
               noise beside it. That is the whole difference from smooth, and
               it is the difference between a polished bevel and a melted one.

    snakehook  grab re-anchored on every stamp, so the region walks with the
               pull instead of snapping back to where the drag started.
               On fixed topology it stretches triangles to the extreme — STATED
               BEHAVIOUR, not a bug. That stretch is how the mesh tells you it
               wants retopo, exactly as Blender behaves with Dyntopo off. The
               SDF `snakehook` resolver (`22_snakehook`) is still the verb for
               GROWING new volume.

Topology never changes under any of them, which the counts below check.
"""

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.2, 1.6, 2.6), (0.0, 0.0, 0.0)


def rough_ball(radius=0.7, cell=0.03, bumps=90, seed=46):
    """A noisy sphere: something for smooth and polish to have an opinion
    about, meshed at a resolution fine enough that a brush has vertices to
    move."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("ball")
    layer.add(clay.Sphere(r=radius))
    rng = np.random.default_rng(seed)
    for _ in range(bumps):
        d = rng.normal(size=3)
        d /= np.linalg.norm(d)
        p = d * radius
        layer.add(clay.Sphere(r=float(rng.uniform(0.05, 0.10)), position=tuple(p)),
                  blend=clay.Smooth(0.05))
    return doc.mesh(voxel_size=cell)


def dented_block(cell=0.025, amplitude=0.007, wavelength=0.26, seed=46):
    """A SHARP box with smooth dents on its faces: a hard edge beside a rough
    flat, which is the case polish exists for and a plain smooth ruins.

    Sharp rather than rounded on purpose — a rounded edge and a smoothed one
    differ by less than the display resampling can show, so the picture would
    say the two brushes agree when the numbers say they do not.

    The dents are a smooth product of sinusoids rather than per-vertex jitter,
    because per-vertex jitter is not what a scan looks like AND it defeats any
    curvature gate by construction: noise steeper than the feature makes every
    flat look like an edge. Real surface error is low-frequency, and that is
    what a polish can tell apart from a corner.

    Returns (dented, clean). Keeping the clean one is what lets the roughness
    be MEASURED afterwards rather than guessed at from a picture."""
    doc = clay.Document()
    doc.add_sdf_layer("block").add(clay.Box(size=(0.42, 0.42, 0.42)))
    mesh = doc.mesh(voxel_size=cell)
    clean = np.array(mesh.positions, copy=True)
    indices = np.array(mesh.indices, copy=True)
    rng = np.random.default_rng(seed)
    k = 2.0 * np.pi / wavelength
    phase = rng.uniform(0.0, 2.0 * np.pi, size=3)
    bump = (np.sin(k * clean[:, 0] + phase[0]) * np.sin(k * clean[:, 1] + phase[1]) *
            np.sin(k * clean[:, 2] + phase[2]))
    outward = clean / np.maximum(np.linalg.norm(clean, axis=1, keepdims=True), 1e-6)
    dented = clean + outward * (amplitude * bump)[:, None]
    return (clay.Mesh.from_triangles(dented.astype(np.float32), indices),
            clay.Mesh.from_triangles(clean.astype(np.float32), indices.copy()))


def preview(mesh, cell=0.012, colour="#9aa7b2"):
    """Display only: a mesh layer is never evaluated, so the renderer has
    nothing to trace until something resamples it. See 36_mesh_layers."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def copy_of(mesh):
    return clay.Mesh.from_triangles(np.array(mesh.positions, copy=True),
                                    np.array(mesh.indices, copy=True))


def stroke_across(mesh, verb, samples, radius, strength, **kwargs):
    """Draw a stroke and report it. Returns the edited mesh."""
    work = copy_of(mesh)
    sculptor = clay.MeshSculptor(work)
    preset = clay.StrokePreset(radius=radius, strength=strength, spacing=0.25)
    applied = sculptor.apply_stroke(np.asarray(samples, dtype=np.float32), preset, verb,
                                    **kwargs)
    same = np.array_equal(np.array(work.indices), np.array(mesh.indices))
    print(f"  {verb:<10} {applied:>3} stamps, topology identical: {same}")
    if not same:
        raise SystemExit(f"{verb} changed the topology")
    return work


def main():
    R.banner("46 mesh brush compositions — clay, crease, scrape, polish, snakehook")

    ball = rough_ball()
    print(f"  rough ball: {ball.triangle_count} triangles, {len(ball.positions)} vertices")

    # A stroke around the equator, on the side facing the camera.
    arc = np.linspace(-0.9, 0.9, 14)
    equator = [(float(np.sin(a) * 0.72), 0.0, float(np.cos(a) * 0.72)) for a in arc]

    # --- clay against draw ----------------------------------------------------
    # ONE stamp each, so the measurement is about the clamp and not about where
    # a stroke went. Draw carries the bumps under it up; clay fills to the
    # plane and levels them.
    patch = (0.0, 0.0, 0.72)
    clay_stamp, draw_stamp = copy_of(ball), copy_of(ball)
    clay.MeshSculptor(clay_stamp).stamp("clay", center=patch, radius=0.30, strength=0.5,
                                        falloff="constant")
    clay.MeshSculptor(draw_stamp).stamp("draw", center=patch, radius=0.30, strength=0.5,
                                        falloff="constant")
    print(f"  one stamp, constant falloff: the deposit is flat to "
          f"{plane_residual(clay_stamp, ball, patch):.5f} under clay and "
          f"{plane_residual(draw_stamp, ball, patch):.5f} under draw "
          f"(the surface it started from: {plane_residual(ball, ball, patch):.5f})")

    # And the same as strokes, for the picture.
    clayed = stroke_across(ball, "clay", equator, radius=0.24, strength=0.55)
    drawn = stroke_across(ball, "draw", equator, radius=0.24, strength=0.55)
    R.side_by_side(
        R.render_array(preview(clayed), eye=EYE, target=TARGET, width=240, height=240),
        R.render_array(preview(drawn), eye=EYE, target=TARGET, width=240, height=240),
        "46_clay_vs_draw.png",
        caption="clay's flat-topped strip (left) against draw's swell, same stroke")

    # --- crease ---------------------------------------------------------------
    creased = stroke_across(ball, "crease", equator, radius=0.18, strength=0.6)

    # --- scrape ---------------------------------------------------------------
    # A wide, firm pass: scrape's job is to shave a FACET, so it wants a
    # footprint several bumps across rather than a stroke that follows them.
    scraped = stroke_across(ball, "scrape", equator, radius=0.38, strength=1.0)

    # --- snakehook: the drag that stretches on purpose -------------------------
    # The drag starts ON the surface and walks away from it. Grab stays anchored
    # where it started and pulls a bulge; snakehook re-anchors on every stamp,
    # so its region travels with the tip and keeps pulling.
    pull = [(0.0, 0.70 + 0.06 * i, 0.0) for i in range(14)]
    hooked = stroke_across(ball, "snakehook", pull, radius=0.24, strength=1.0)
    grabbed = stroke_across(ball, "grab", pull, radius=0.24, strength=1.0)
    print(f"  the tip reached y={float(np.array(hooked.positions)[:, 1].max()):.3f} under "
          f"snakehook and y={float(np.array(grabbed.positions)[:, 1].max()):.3f} under grab")
    print(f"  snakehook's longest edge grew {edge_growth(ball, hooked):.1f}x; "
          f"grab's {edge_growth(ball, grabbed):.1f}x — that stretch is the signal "
          f"the mesh wants retopo")

    R.contact_sheet(
        [R.render_tile(preview(ball), eye=EYE, target=TARGET, size=190),
         R.render_tile(preview(clayed), eye=EYE, target=TARGET, size=190),
         R.render_tile(preview(creased), eye=EYE, target=TARGET, size=190),
         R.render_tile(preview(scraped), eye=EYE, target=TARGET, size=190),
         R.render_tile(preview(hooked), eye=EYE, target=TARGET, size=190),
         R.render_tile(preview(grabbed), eye=EYE, target=TARGET, size=190)],
        "46_mesh_brush_compositions.png", columns=3,
        caption="untouched, clay, crease, scrape, snakehook, grab")

    # --- polish against smooth ------------------------------------------------
    # The one that needs its own fixture: a hard edge beside a rough flat.
    block, clean = dented_block()
    print(f"\n  dented sharp block: {block.triangle_count} triangles")
    block_eye, block_target = (0.85, 0.62, 0.95), (0.0, 0.0, 0.0)

    # TWO passes over the block, because one set of settings cannot show both
    # halves of the claim. A gentle pass is where the NUMBERS live: polish and
    # smooth remove the same roughness, and polish leaves the edges alone. A
    # heavy pass is where the PICTURE lives: taken far enough that a plain
    # smooth melts the box, the gate is what is still holding the corners up.
    gentle = dict(center=(0.0, 0.0, 0.0), radius=1.5, strength=0.6,
                  falloff="constant", geodesic=False, smooth_iterations=6)
    heavy = dict(gentle, strength=1.0, smooth_iterations=24)

    def run(verb, settings, **kwargs):
        work = copy_of(block)
        clay.MeshSculptor(work).stamp(verb, **settings, **kwargs)
        return work

    smoothed = run("smooth", gentle)
    print(f"  {'':<16}{'flat roughness':>16}{'edges moved':>14}")
    print(f"  {'as built':<16}{flat_roughness(block, clean):>16.5f}{0.0:>14.5f}")
    for angle in (0.10, 0.20, 0.45):
        work = run("polish", gentle, polish_angle=angle)
        print(f"  {f'polish {angle:.2f}':<16}{flat_roughness(work, clean):>16.5f}"
              f"{edge_drift(block, work, clean):>14.5f}")
    print(f"  {'smooth':<16}{flat_roughness(smoothed, clean):>16.5f}"
          f"{edge_drift(block, smoothed, clean):>14.5f}")
    print("  At 0.10 polish removed slightly MORE of the roughness than a plain smooth\n"
          "  and moved the box's edges a seventh as far. By 0.45 the gate is open almost\n"
          "  everywhere and polish IS a plain smooth — the angle is the whole brush.")

    polished = run("polish", heavy, polish_angle=0.20)
    melted = run("smooth", heavy)

    R.contact_sheet(
        [R.render_tile(preview(block, cell=0.005), eye=block_eye, target=block_target, size=200),
         R.render_tile(preview(polished, cell=0.005), eye=block_eye, target=block_target, size=200),
         R.render_tile(preview(melted, cell=0.005), eye=block_eye, target=block_target, size=200)],
        "46_polish_vs_smooth.png", columns=3,
        caption="as built, then the same heavy pass through polish and through smooth — "
                "the gate is what is still holding the corners up")


def plane_residual(mesh, base, centre, radius=0.22):
    """RMS deviation from the least-squares plane over the vertices whose
    ORIGINAL position lies under the brush. "Is this patch flat" asked
    properly: the y spread alone would also count the plane's tilt."""
    p0 = np.array(base.positions)
    window = np.linalg.norm(p0 - np.asarray(centre, dtype=np.float32), axis=1) < radius
    if window.sum() < 8:
        return 0.0
    p = np.array(mesh.positions)[window]
    # Fit p . n = d by taking the smallest singular direction of the centred
    # points; the residual is the spread along it.
    centred = p - p.mean(axis=0)
    normal = np.linalg.svd(centred, full_matrices=False)[2][-1]
    return float(np.sqrt(np.mean((centred @ normal) ** 2)))


def edge_growth(base, edited):
    """How much the longest triangle edge under the brush grew. Fixed topology
    means a big pull has to come out of the edges, and this is that number."""
    idx = np.array(base.indices).reshape(-1, 3)
    def longest(mesh):
        p = np.array(mesh.positions)
        e = np.concatenate([np.linalg.norm(p[idx[:, 0]] - p[idx[:, 1]], axis=1),
                            np.linalg.norm(p[idx[:, 1]] - p[idx[:, 2]], axis=1),
                            np.linalg.norm(p[idx[:, 2]] - p[idx[:, 0]], axis=1)])
        return float(e.max())
    return longest(edited) / max(longest(base), 1e-9)


def _flat_mask(clean):
    """Vertices in the INTERIOR of one of the block's six faces: one coordinate
    at the extent, the other two well inside it. The rounded edges are the
    complement, which is what makes the two measurements below disjoint."""
    c = np.array(clean.positions)
    extent = float(np.abs(c).max())
    axes = np.sort(np.abs(c), axis=1)
    return (axes[:, 2] > 0.95 * extent) & (axes[:, 1] < 0.70 * extent), c, extent


def flat_roughness(mesh, clean):
    """How rough each face is: the RMS residual from a least-squares plane fit
    over that face's interior vertices, averaged over the six faces.

    Residual-from-a-plane rather than distance-from-clean, because a Laplacian
    smooth SHRINKS what it touches and a shrunken face is not a rough one. The
    plane fit absorbs both the shrinkage and the tilt and leaves the dents."""
    flat, c, extent = _flat_mask(clean)
    p = np.array(mesh.positions)
    residuals = []
    for axis in range(3):
        for sign in (1.0, -1.0):
            face = flat & (c[:, axis] * sign > 0.95 * extent)
            if face.sum() < 16:
                continue
            others = [a for a in range(3) if a != axis]
            design = np.column_stack([np.ones(face.sum()), p[face][:, others[0]],
                                      p[face][:, others[1]]])
            fit, *_ = np.linalg.lstsq(design, p[face][:, axis], rcond=None)
            residuals.append(float(np.sqrt(np.mean((design @ fit - p[face][:, axis]) ** 2))))
    return float(np.mean(residuals)) if residuals else float("nan")


def edge_drift(base, edited, clean):
    """RMS movement of the vertices on the EDGES and corners — the shape polish
    is supposed to leave alone and a plain smooth melts."""
    flat, _, _ = _flat_mask(clean)
    on_an_edge = ~flat
    d = np.array(edited.positions)[on_an_edge] - np.array(base.positions)[on_an_edge]
    return float(np.sqrt(np.mean(np.sum(d * d, axis=1))))


if __name__ == "__main__":
    main()

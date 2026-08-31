"""Wrinkles at a fine level, a new skull underneath them, and the wrinkles still there.

THE GAP THIS CLOSES, stated as an artist would: on a mesh layer, fine detail and
coarse form are the same edit. The detail IS the vertex positions, and there is
no record of which part of a position was form and which part was wrinkle — so a
pass that changes proportions destroys the pass before it. Every sculptor who has
ever put pores on a face and then been told the brow is wrong knows the cost.

A multiresolution surface is a base cage, a deterministic Catmull-Clark hierarchy
over it, and per-level detail stored as coefficients in a TRANSPORTED LOCAL FRAME
rather than as world-space offsets:

    P(0) = the cage             the production geometry, sculpted directly
    S(n) = Subdivide(P(n-1))    the pure subdivision, no detail
    P(n) = S(n) + F(n) * D(n)   the level as the artist sees it

WHAT THE PICTURES SHOW, and what the numbers underneath them assert:

  - A RIDGE sculpted at the finest level, and the coefficients it became.

  - THE FORM CHANGED AT A COARSE LEVEL while the fine one is displayed — the
    workflow the independence of the two levels exists for.

  - THE DETAIL STILL THERE afterwards, checked three ways: the coefficients are
    unchanged bit for bit, the high-pass energy of the surface is preserved, and
    the reconstructed offset is still along the surface normal. A world-space
    delta would pass the first and fail the last two.

  - A DAB THAT COSTS WHAT IT TOUCHES: an edit at a coarse level reconstructs the
    descendants of what it moved and leaves every unrelated base patch
    BYTE-IDENTICAL.

  - AND THE FIXED SCULPTOR UNTOUCHED, on the same model, because a mesh layer's
    contract is what makes it worth holding after a retopology pass.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (1.3, 1.0, 2.6), (0.0, 0.05, 0.15)


def cage(n=6, radius=1.0):
    """A cube-sphere, as triangles: the coarse production cage a retopology pass
    would hand over.

    DELIBERATELY COARSE. The point of a hierarchy is that the cage carries the
    form and the levels above it carry the detail, and starting from something
    already fine would hide which of the two every number below is about.
    """
    positions, indices = [], []
    axes = [(0, 1, 2), (0, 1, 2), (1, 2, 0), (1, 2, 0), (2, 0, 1), (2, 0, 1)]
    signs = [1.0, -1.0, 1.0, -1.0, 1.0, -1.0]
    for f in range(6):
        base = len(positions)
        for v in range(n + 1):
            for u in range(n + 1):
                c = [0.0, 0.0, 0.0]
                c[axes[f][0]] = -1.0 + 2.0 * u / n
                c[axes[f][1]] = -1.0 + 2.0 * v / n
                c[axes[f][2]] = signs[f]
                length = float(np.linalg.norm(c))
                positions.append([x / length * radius for x in c])
        stride = n + 1
        for v in range(n):
            for u in range(n):
                a = base + v * stride + u
                b, c2, d = a + 1, a + stride, a + stride + 1
                if signs[f] > 0.0:
                    indices += [a, c2, b, b, c2, d]
                else:
                    indices += [a, b, c2, b, d, c2]
    return clay.Mesh.from_triangles(np.array(positions, dtype=np.float32),
                                    np.array(indices, dtype=np.uint32))


def preview(mesh, cell=0.012, colour="#b0784a"):
    """A DISPLAY-ONLY document. A mesh layer is never evaluated, so the renderer
    — which raycasts a field — has nothing to trace against; resampling here is
    exactly the approximation a mesh layer exists to avoid, and it is fine for a
    picture and wrong for the export."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def detail_energy(surface, level):
    """How far, on average, the surface stands off the pure subdivision.

    THE MEASUREMENT THIS EXAMPLE IS BUILT AROUND. "The wrinkle survived" is a
    claim about this number before and after a coarse edit, and a screenshot
    cannot tell a preserved wrinkle from a flattened one at this scale.
    """
    p = np.asarray(surface.positions_at(level))
    s = np.asarray(surface.subdivided_at(level))
    return float(np.linalg.norm(p - s, axis=1).mean())


def stroke(sculptor, verb, centre, radius, strength, steps=1, along=(0.0, 0.0, 0.0)):
    moved = 0
    sculptor.begin_stroke()
    for i in range(steps):
        t = i / max(steps - 1, 1)
        c = tuple(float(a + b * t) for a, b in zip(centre, along))
        moved += sculptor.stamp(verb, center=c, radius=radius, strength=strength)
    return moved


def main():
    R.banner("68 mesh multires — sculpt fine, change the form, come back")

    base = cage(6, 1.0)
    surface = clay.MultiresSurface.from_mesh(base)
    print(f"  the cage is {base.triangle_count} triangles and {surface.base_vertex_count} "
          "geometric vertices.")

    # --- levels are PRICED before they are added ------------------------------
    #
    # Catmull-Clark multiplies faces by four, so the level nobody checked is the
    # allocation that kills an app on the device this library targets.
    for _ in range(3):
        p = surface.preflight_add_level()
        print(f"  level {p['level']}: {p['vertices']:>7} vertices, "
              f"{p['faces']:>7} faces, {p['persistent_bytes'] / 1024:.0f} KiB kept, "
              f"{p['peak_bytes'] / 1024:.0f} KiB peak")
        if not p["allowed"]:
            raise SystemExit(f"the level was refused: {p['error']}")
        surface.add_level()

    fine, coarse = surface.max_level, 1
    counts = surface.level_counts(fine)
    print(f"\n  the hierarchy is {surface.level_count} levels; the finest holds "
          f"{counts['vertices']} vertices.")

    tiles, labels = [], []
    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("cage, subdivided")

    # --- 1. fine detail -------------------------------------------------------
    surface.sculpt_level = fine
    surface.display_level = fine
    sculptor = clay.MultiresSculptor(surface)
    ridge = stroke(sculptor, "draw", (-0.45, 0.25, 0.88), radius=0.16, strength=0.5,
                   steps=9, along=(0.9, 0.0, 0.0))
    if ridge == 0:
        raise SystemExit("the fine stamp reached nothing")

    detail = np.asarray(surface.detail_at(fine))
    touched = int(np.count_nonzero(np.linalg.norm(detail, axis=1) > 1e-6))
    energy_before = detail_energy(surface, fine)
    coeffs_before = np.array(detail, copy=True)
    fine_before = np.array(surface.positions_at(fine), copy=True)
    sub_before = np.array(surface.subdivided_at(fine), copy=True)
    normals_before = np.array(surface.normals_at(fine), copy=True)

    print(f"\n  RIDGE     {ridge} vertices moved at level {fine}; {touched} of "
          f"{counts['vertices']} now carry coefficients.")
    print(f"            mean stand-off from the pure subdivision: {energy_before:.4f}")
    if surface.base_revision != 1:
        raise SystemExit("a fine stamp moved the cage; it must be stored as detail")
    print("            the CAGE is untouched: what the brush wrote became")
    print("            coefficients, which is the whole difference from a mesh layer.")
    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("ridge at L3")

    # --- 2. the form changes underneath it ------------------------------------
    #
    # SCULPT COARSE, DISPLAY FINE. The two levels are independent, which is what
    # lets an artist move the broad form and watch the detail move with it.
    surface.sculpt_level = coarse
    surface.display_level = fine
    surface.clear_dirty()
    surface.reset_eval_stats()
    form = stroke(sculptor, "draw", (0.0, 0.1, 1.0), radius=0.65, strength=0.6)
    if form == 0:
        raise SystemExit("the coarse stamp reached nothing")

    fine_after = np.asarray(surface.positions_at(fine))
    moved = float(np.abs(fine_after[:, 1] - fine_before[:, 1]).max())
    energy_after = detail_energy(surface, fine)
    print(f"\n  FORM      {form} vertices moved at level {coarse}; the fine surface "
          f"rose by up to {moved:.3f}")

    # --- 3. the three ways the detail is still there --------------------------
    # The FINE level's own coefficients, compared bit for bit. The whole-surface
    # checksum would be the wrong instrument here: an edit at level 1 is stored
    # as level 1's detail, so it moves on purpose — what must not move is the
    # level above it.
    if not np.array_equal(np.asarray(surface.detail_at(fine)), coeffs_before):
        raise SystemExit("the coarse edit rewrote the fine level's coefficients")
    print(f"  DETAIL    all {coeffs_before.shape[0]} coefficient triples at level "
          f"{fine} are unchanged, bit for bit")

    ratio = energy_after / max(energy_before, 1e-9)
    print(f"            high-pass energy {energy_before:.4f} -> {energy_after:.4f} "
          f"({ratio:.3f}x)")
    if not 0.9 < ratio < 1.1:
        raise SystemExit(
            f"the detail did not survive the form change: energy went to {ratio:.3f}x")

    # AND IT IS STILL ATTACHED, which is the half a magnitude check cannot see —
    # and the half that a world-space delta gets wrong. The measure is the
    # detail's LEAN against the surface it sits on: the cosine between the
    # offset and the local normal, before and after the form changed. The
    # control is what the same detail would have done stored as a world vector,
    # which is the implementation this design rejected.
    sub_after = np.asarray(surface.subdivided_at(fine))
    normals_after = np.asarray(surface.normals_at(fine))
    offsets_before = fine_before - sub_before
    offsets_after = fine_after - sub_after
    length = np.linalg.norm(offsets_before, axis=1)
    live = length > 1e-4

    def lean(offsets, normals):
        n = np.linalg.norm(offsets[live], axis=1)
        return (offsets[live] * normals[live]).sum(axis=1) / np.maximum(n, 1e-12)

    lean_before = lean(offsets_before, normals_before)
    lean_kept = lean(offsets_after, normals_after)
    # The world-space model: the SAME vector, still pointing where it pointed,
    # over a surface that has moved out from under it.
    lean_world = lean(offsets_before, normals_after)

    drift_kept = float(np.abs(lean_kept - lean_before).mean())
    drift_world = float(np.abs(lean_world - lean_before).mean())
    print(f"            and it is still attached. Over {int(live.sum())} detailed "
          "vertices, the")
    print(f"            detail's lean against the surface drifted {drift_kept:.4f} "
          "in a transported")
    print(f"            frame against {drift_world:.4f} for the same detail stored as a "
          "world vector")
    print(f"            — {drift_world / max(drift_kept, 1e-9):.1f}x worse, which is the "
          "artefact this")
    print("            representation exists to avoid.")
    if not drift_world > drift_kept * 3.0:
        raise SystemExit(
            f"the transported frame bought nothing: {drift_kept:.4f} against "
            f"{drift_world:.4f} for a world-space delta")

    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("form changed, ridge kept")

    # --- 4. the dab costs what it touched -------------------------------------
    stats = surface.eval_stats()
    dirty = surface.dirty_patches
    total_patches = surface.level_counts(0)["faces"]
    print(f"\n  LOCAL     the coarse dab reconstructed {stats['vertices_evaluated']} "
          f"vertices across {stats['partial_level_updates']} levels")
    print(f"            and reported {len(dirty)} of {total_patches} base patches dirty.")
    if stats["full_level_rebuilds"] != 0:
        raise SystemExit("a dab rebuilt a whole level; propagation is not local")
    if len(dirty) >= total_patches:
        raise SystemExit("a dab dirtied the whole cage")

    # BYTE-IDENTICAL, not merely unchanged to a tolerance: the only honest
    # account of "this dab did not reach there" is that nothing there was
    # written at all.
    far = fine_after[:, 2] < -0.6
    if not np.array_equal(fine_after[far], fine_before[far]):
        raise SystemExit("the far side moved; the propagation is not local")
    print(f"            the far side of the model is byte-identical: {int(far.sum())} "
          "vertices untouched.")

    # --- 5. and it survives a save ------------------------------------------
    blob = surface.serialize()
    reloaded = clay.MultiresSurface.deserialize(blob)
    if reloaded.detail_checksum != surface.detail_checksum:
        raise SystemExit("the hierarchy did not round-trip")
    if not np.array_equal(np.asarray(reloaded.positions_at(fine)), fine_after):
        raise SystemExit("the reloaded hierarchy reconstructs a different surface")
    print(f"\n  SAVE      {len(blob) / 1024:.1f} KiB carries the cage, the rule, the level")
    print("            count and each level's detail — the face lists and every evaluated")
    print("            position follow from those and are not written.")

    memory = surface.memory()
    print(f"            in memory: {memory['authoritative'] / 1024:.0f} KiB authoritative "
          f"({memory['detail'] / 1024:.0f} KiB of it detail), "
          f"{memory['rebuildable'] / 1024:.0f} KiB rebuildable.")

    R.contact_sheet(tiles, "68_mesh_multires.png", columns=3, caption=" | ".join(labels))

    # --- what is NOT affected -------------------------------------------------
    #
    # The fixed-topology contract is what makes a mesh layer worth holding after
    # a retopology pass, and none of the above changes it.
    fixed = clay.Mesh.from_triangles(np.array(base.positions, copy=True),
                                     np.array(base.indices, copy=True))
    indices_before = np.array(fixed.indices, copy=True)
    fixed_sculptor = clay.MeshSculptor(fixed)
    if fixed_sculptor.stamp("draw", center=(0, 0, 0), radius=1.0, strength=0.5) == 0:
        raise SystemExit("the fixed sculptor did nothing, so this proves nothing")
    if not np.array_equal(np.array(fixed.indices), indices_before):
        raise SystemExit("the FIXED sculptor changed the topology — the contract is broken")
    print("\n  the fixed sculptor is untouched: its index buffer is byte-identical.")
    print("  A hierarchy is a representation a caller converts into deliberately,")
    print("  never a mode the fixed one slips into.")

    obj = R.output_path("68_multires_level3.obj")
    surface.mesh_at_level(fine).save(obj)
    print(f"  wrote {os.path.basename(obj)} for inspection.")


if __name__ == "__main__":
    main()

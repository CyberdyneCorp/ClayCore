"""A wrinkle pass dialled 0 -> 50% -> 100% over a form that never changes.

THE GAP THIS CLOSES, stated as an artist states it: make a wrinkle pass, then
come back days later and dial it to half, then hide it, then delete it — without
redoing any of the work under it and without replaying a single stroke. Nothing
on a mesh could do that, and `68_mesh_multires` only got half way there: it made
a pass SURVIVE an edit beneath it. This makes a pass ADDRESSABLE afterwards.

    E(n) = B(n) + SUM over layers of  s_i * m_i(v) * L_i(n, v)

B is the level's own base detail — the FORM, unchanged in meaning. L_i is layer
i's coefficients, in the SAME field, in the SAME transported frame, at the SAME
block size: a layer contribution and a base detail coefficient are one quantity
under two owners, which is why there is no second displacement representation
here to keep in step with the first.

WHAT THE PICTURES SHOW, and what the numbers underneath them assert:

  - THE FORM ALONE. Every pass at strength 0, which must be BIT-IDENTICAL to the
    same hierarchy with no stack at all. A layer at zero is skipped rather than
    multiplied by zero, so this is an equality and not a tolerance.

  - THE SAME PASS AT 50% AND AT 100%. The offset at half is exactly half the
    offset at full, and the COEFFICIENTS do not move between the two — because
    strength is composition and not a scale on the pen. This is the behaviour
    most likely to be reported as a bug: a stroke made into a layer at 0.5
    records its FULL contribution and moves the surface half as far, so raising
    the slider afterwards doubles what is on screen and replays nothing.

  - ONE LAYER REMOVED, THE OTHERS UNTOUCHED. Byte-identical coefficients on the
    layers that stayed, and the base detail checksum unmoved throughout — the
    form under the passes is never rewritten by anything done to a pass.

  - AND WHAT A SLIDER COSTS. `blocks_recomposed` after a strength change is the
    layer's own coverage rather than the level, which is the difference between
    a stack that scales to a hundred passes and one that does not. There is no
    other way to see it: a correct implementation and a quadratic one produce
    the same surface.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (1.7, 1.3, 3.3), (0.0, 0.05, 0.12)


def cage(n=6, radius=1.0):
    """A cube-sphere, as triangles: the coarse production cage a retopology pass
    would hand over. The same cage `68_mesh_multires` uses, deliberately — this
    example is that one's second half and the models should be comparable."""
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
                # OUTWARD (#407): a closed cage wound inward has winding number
                # -1 in its interior, so the preview below renders as speckle.
                if signs[f] > 0.0:
                    indices += [a, b, c2, b, d, c2]
                else:
                    indices += [a, c2, b, b, c2, d]
    return clay.Mesh.from_triangles(np.array(positions, dtype=np.float32),
                                    np.array(indices, dtype=np.uint32))


def preview(mesh, cell=0.012, colour="#b0784a"):
    """A DISPLAY-ONLY document. A hierarchy is never evaluated as a field, so the
    renderer — which raycasts one — has nothing to trace against; resampling here
    is exactly the approximation this representation exists to avoid, and it is
    fine for a picture and wrong for the export."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def pass_stroke(surface, path, radius, strength):
    """One gesture into the surface's active layer, as a TRANSACTION.

    The context manager is the form to reach for and not sugar: entering begins
    the gesture, a clean exit commits it, and a raising block CANCELS. A stroke
    loop that raised halfway through a bare begin/commit pair would leave the
    surface holding its composition — every slider on the model refusing, with
    nothing in the traceback saying why.
    """
    moved = 0
    with surface.sculpt_layer_stroke() as stroke:
        for centre in path:
            moved += stroke.stamp("draw", center=centre, radius=radius, strength=strength)
        entries = stroke.record_size
        stamps = stroke.stamps
    return moved, stamps, entries


def arc(start, end, steps):
    a, b = np.array(start, np.float32), np.array(end, np.float32)
    return [tuple(float(x) for x in (a + (b - a) * (i / max(steps - 1, 1))))
            for i in range(steps)]


def main():
    R.banner("69 mesh sculpt layers — dial a pass, days later, replaying nothing")

    surface = clay.MultiresSurface.from_mesh(cage(6, 1.0))
    for _ in range(3):
        p = surface.preflight_add_level()
        if not p["allowed"]:
            raise SystemExit(f"the level was refused: {p['error']}")
        surface.add_level()
    fine = surface.max_level
    vertices = surface.level_counts(fine)["vertices"]
    surface.sculpt_level = fine
    surface.display_level = fine
    print(f"  the hierarchy is {surface.level_count} levels; the finest holds "
          f"{vertices} vertices.")

    # --- 1. THE FORM, which nothing below is allowed to change ----------------
    #
    # Written with write_domain='geometry': the base detail, the form UNDER the
    # passes. The two cases a host needs are opposite — "sculpt the pass I am
    # working on" and "fix the form under the passes without disturbing them" —
    # so neither is a default and the caller says which.
    with surface.sculpt_layer_stroke(write_domain="geometry") as form:
        if form.stamp("draw", center=(0.0, 0.35, 0.95), radius=0.55, strength=0.6) == 0:
            raise SystemExit("the form stamp reached nothing")
    form_checksum = surface.detail_checksum
    form_only = np.array(surface.positions_at(fine), copy=True)
    print(f"\n  FORM      written into the base detail; checksum {form_checksum:#x}")

    tiles, labels = [], []
    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("form only")

    # --- 2. three passes, each in its own channel -----------------------------
    passes = {}
    for name, path, radius, strength in (
            ("wrinkles", arc((-0.45, 0.42, 0.86), (0.45, 0.42, 0.86), 9), 0.13, 0.55),
            ("pores", arc((-0.2, 0.05, 1.0), (0.25, 0.05, 0.98), 7), 0.10, 0.35),
            ("scar", arc((0.1, -0.35, 0.92), (0.1, 0.15, 0.98), 6), 0.09, 0.45)):
        layer = surface.add_sculpt_layer(name)
        surface.active_sculpt_layer = layer
        moved, stamps, entries = pass_stroke(surface, path, radius, strength)
        if moved == 0:
            raise SystemExit(f"the '{name}' pass reached nothing")
        info = surface.sculpt_layer_info(layer)
        passes[name] = layer
        print(f"  PASS      {name:<9} {stamps} stamps into {entries} undo entries; "
              f"{info['coverage_vertices']} vertices covered, "
              f"{info['bytes'] / 1024:.1f} KiB")

    # THE FORM IS UNTOUCHED. Three passes went into three channels and the base
    # detail is byte-identical — which is the whole claim, and a render cannot
    # tell it from three passes baked in.
    if surface.detail_checksum != form_checksum:
        raise SystemExit("a layered stroke rewrote the base detail")
    print(f"\n  the base detail is unchanged after all three passes: "
          f"{surface.detail_checksum:#x}")

    full = np.array(surface.positions_at(fine), copy=True)
    stack_checksum = surface.sculpt_layer_checksum

    # --- 2b. THE RECORD FOLLOWS THE VERTICES, NOT THE STAMPS ------------------
    #
    # A hundred stamps over one place is ONE entry per vertex, keeping the first
    # `before` and the last `after` — so an undo step's size follows what the
    # stroke reached rather than how long the artist dwelt on it. Both gestures
    # here are CANCELLED, which is also the exactness claim: a layered write is
    # `L += dE`, so the only exact restore is the recorded `before` values.
    surface.active_sculpt_layer = passes["wrinkles"]
    # Taken FROM the surface rather than assumed: the form stamp and the three
    # passes have moved it, so a point that was on the cage is no longer on the
    # model and a dab aimed there would reach nothing and prove nothing.
    live_positions = np.array(surface.positions_at(fine), copy=True)
    dwell = tuple(float(x) for x in live_positions[
        int(np.argmin(np.linalg.norm(live_positions - np.array([0.0, 0.42, 0.9]), axis=1)))])
    with surface.sculpt_layer_stroke() as one:
        one.stamp("draw", center=dwell, radius=0.13, strength=0.4)
        one_entries = one.record_size
        one.cancel()
    with surface.sculpt_layer_stroke() as many:
        for _ in range(40):
            many.stamp("draw", center=dwell, radius=0.13, strength=0.4)
        many_entries, many_stamps = many.record_size, many.stamps
        many.cancel()
    print(f"\n  COALESCE  {many_stamps} stamps over one place record {many_entries} "
          f"entries — the same {one_entries}")
    print("            one stamp records. The record follows the VERTICES the stroke")
    print("            reached, not the stamps it took.")
    if one_entries == 0:
        raise SystemExit("the dwell dab reached nothing, so the comparison proves nothing")
    if many_entries != one_entries:
        raise SystemExit(f"{many_stamps} stamps recorded {many_entries} entries "
                         f"against {one_entries} for one; the gesture did not coalesce")
    if surface.sculpt_layer_checksum != stack_checksum:
        raise SystemExit("a cancelled gesture did not restore the channel exactly")

    # --- 3. THE DIAL: 0 -> 50% -> 100%, replaying nothing ---------------------
    for layer in passes.values():
        surface.set_sculpt_layer_strength(layer, 0.0)
    at_zero = np.array(surface.positions_at(fine), copy=True)
    # BIT-IDENTICAL, not close: a layer at zero effective strength is SKIPPED
    # rather than multiplied by zero, so the composed field is the base field
    # bit for bit and an empty stack costs nothing.
    if not np.array_equal(at_zero, form_only):
        raise SystemExit("at strength 0 the passes still moved the surface")
    print("\n  DIAL 0    every pass at 0: the surface is BYTE-IDENTICAL to the form.")
    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("passes at 0%")

    for layer in passes.values():
        surface.set_sculpt_layer_strength(layer, 0.5)
    at_half = np.array(surface.positions_at(fine), copy=True)
    offset_half = np.linalg.norm(at_half - form_only, axis=1)
    offset_full = np.linalg.norm(full - form_only, axis=1)
    live = offset_full > 1e-5
    ratio = float((offset_half[live] / offset_full[live]).mean())
    print(f"  DIAL 50   mean offset is {ratio:.4f} of the offset at full over "
          f"{int(live.sum())} moved vertices.")
    if not abs(ratio - 0.5) < 1e-3:
        raise SystemExit(f"half strength moved {ratio:.4f} of the pass, not half")
    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("passes at 50%")

    # AND NOTHING WAS REPLAYED. The coefficients are the same bytes they were at
    # full strength: strength is composition, so raising the slider back
    # restores the whole contribution rather than a scaled-down remnant.
    if surface.sculpt_layer_checksum != stack_checksum:
        raise SystemExit("the slider rewrote the coefficients; it is not composition")
    for layer in passes.values():
        surface.set_sculpt_layer_strength(layer, 1.0)
    if not np.array_equal(np.array(surface.positions_at(fine)), full):
        raise SystemExit("dialling back to 1.0 did not reproduce the surface exactly")
    print("  DIAL 100  back to full, BYTE-IDENTICAL to before the slider moved,")
    print("            and the stored coefficients never changed at all.")
    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("passes at 100%")

    # --- 4. WHAT A SLIDER COSTS ----------------------------------------------
    #
    # A correct implementation and a quadratic one produce the same surface, so
    # the counter is the only instrument. A strength change must cost the
    # LAYER'S coverage and not the level's size.
    block = 1024
    level_blocks = (vertices + block - 1) // block
    coverage_blocks = surface.sculpt_layer_info(passes["scar"])["coverage_vertices"] // block
    surface.reset_sculpt_layer_stats()
    surface.set_sculpt_layer_strength(passes["scar"], 0.4)
    surface.positions_at(fine)
    stats = surface.sculpt_layer_stats()
    print(f"\n  LOCAL     a slider on 'scar' recomposed {stats['blocks_recomposed']} blocks "
          f"of the {level_blocks} at level {fine},")
    print(f"            which is exactly the {coverage_blocks} that layer has ALLOCATED — "
          "its coverage,")
    print(f"            not the surface — summing {stats['layer_blocks_visited']} "
          "(block, layer) pairs in doing it.")
    # THE GATE, and it is a data-structure property rather than an optimisation:
    # the blocks the layer has allocated, keyed on its own storage. A subdivided
    # cube-sphere numbers a spatially local footprint across many blocks, which
    # is why this figure is a fair fraction of the level here and a single block
    # on the plane `test_mesh_sculpt_layers` measures — the claim is the
    # equality below either way.
    if stats["blocks_recomposed"] != coverage_blocks:
        raise SystemExit(f"a strength change recomposed {stats['blocks_recomposed']} blocks "
                         f"against a coverage of {coverage_blocks}")
    if stats["blocks_recomposed"] >= level_blocks:
        raise SystemExit("a strength change recomposed the whole level")
    surface.set_sculpt_layer_strength(passes["scar"], 1.0)

    # --- 5. ONE LAYER REMOVED, THE OTHERS UNTOUCHED ---------------------------
    kept = {name: np.array([surface.sculpt_layer_detail(layer, fine, v)
                            for v in range(vertices)], np.float32)
            for name, layer in passes.items() if name != "pores"}
    surface.remove_sculpt_layer(passes["pores"])
    if surface.sculpt_layer_count != 2:
        raise SystemExit("removing one layer did not leave two")
    for name, before in kept.items():
        after = np.array([surface.sculpt_layer_detail(passes[name], fine, v)
                          for v in range(vertices)], np.float32)
        # BYTE-IDENTICAL: no stroke is replayed and no other layer's
        # coefficients, strength or relative order change.
        if not np.array_equal(before, after):
            raise SystemExit(f"removing 'pores' rewrote '{name}'")
    if surface.detail_checksum != form_checksum:
        raise SystemExit("removing a layer rewrote the form beneath it")
    print("\n  REMOVE    'pores' discarded; 'wrinkles' and 'scar' are byte-identical")
    print("            and the form beneath all three never moved.")
    tiles.append(R.render_tile(preview(surface.mesh_at_level(fine)),
                               eye=EYE, target=TARGET, size=190))
    labels.append("'pores' removed")

    # --- 6. a gesture that raises leaves nothing behind -----------------------
    #
    # The half of the context manager that is not sugar. A partial gesture
    # committed on the way out of an exception is an undo step for work nobody
    # asked for; one left open holds the composition forever.
    before_accident = surface.sculpt_layer_checksum
    surface.active_sculpt_layer = passes["wrinkles"]
    try:
        with surface.sculpt_layer_stroke() as stroke:
            stroke.stamp("draw", center=(0.0, 0.42, 0.9), radius=0.3, strength=0.9)
            raise RuntimeError("the tablet fell off the desk")
    except RuntimeError:
        pass
    if surface.sculpt_layer_checksum != before_accident:
        raise SystemExit("a raising stroke left its half-finished work on the layer")
    surface.set_sculpt_layer_strength(passes["wrinkles"], 0.9)  # refuses if still held
    surface.set_sculpt_layer_strength(passes["wrinkles"], 1.0)
    print("\n  CANCEL    a stroke loop that raised cancelled exactly, and the")
    print("            composition is not left held: the sliders still move.")

    # --- 7. and the stack survives a save ------------------------------------
    blob = surface.serialize()
    reloaded = clay.MultiresSurface.deserialize(blob)
    if reloaded.sculpt_layer_ids != surface.sculpt_layer_ids:
        raise SystemExit("the layer ids did not survive the round trip")
    if reloaded.sculpt_layer_checksum != surface.sculpt_layer_checksum:
        raise SystemExit("the stack did not round-trip")
    if not np.array_equal(np.array(reloaded.positions_at(fine)),
                          np.array(surface.positions_at(fine))):
        raise SystemExit("the reloaded stack reconstructs a different surface")
    memory = surface.memory()
    print(f"\n  SAVE      {len(blob) / 1024:.1f} KiB carries the cage, the rule, each "
          "level's detail")
    print("            AND the stack: id, name, kind, sliders, per-level blocks and masks.")
    print(f"            in memory: {memory['sculpt_layers'] / 1024:.0f} KiB of passes "
          f"(authoritative) beside")
    print(f"            {memory['detail'] / 1024:.0f} KiB of form, and "
          f"{memory['composed'] / 1024:.0f} KiB of composed field, which is derived.")

    R.contact_sheet(tiles, "69_mesh_sculpt_layers.png", columns=3,
                    caption=" | ".join(labels))
    obj = R.output_path("69_sculpt_layers_level3.obj")
    surface.mesh_at_level(fine).save(obj)
    print(f"\n  wrote {os.path.basename(obj)} for inspection.")
    print("  A sculpt layer is an artist CHANNEL and MeshBrush::Layer is a brush")
    print("  ALGORITHM; nothing here is ever spelled 'layer' unqualified, and")
    print("  tools/check_c_abi.py gates the discipline rather than remembering it.")


if __name__ == "__main__":
    main()

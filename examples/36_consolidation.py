"""Consolidation — the policy that lets the region verbs be used twice.

None of the SDF sculpting verbs were missing. `relaxed` smooths under a region,
`flattened_from` planes a facet, `snakehook` pulls a tendril, `move_surface`
drags a surface, and the mask brush is complete. What was missing was using any
of them **more than once in a row** — and an artist does not polish once, they
polish, look, and polish again.

Three things are worth reading before looking.

**Two mechanisms, not one.** A polish samples the document and hands back a
volume, so the second pass samples a VOLUME rather than a document: outside its
band a volume reports a lower bound rather than a distance, and the blend then
works from the wrong value. A Move stroke never touches a volume at all — each
drag appends a `grab` to the deformer chain, and those multiply. The report
names both separately (`steepest_volume`, `longest_deformer_chain`), because an
aggregate step scale says something is wrong and those two say which thing.

**Baking alone does NOT fix it, and this example measures that rather than
asserting it.** The obvious guess is that collapsing the chain into one volume
collapses its cost with it. It does not: steepness is a property of the FIELD,
and resampling it onto a lattice reproduces the steepness — a finer cell makes
it *worse*, because there are then more cells across the same steep shell. What
removes it is **redistancing**: replacing the baked samples with the distance to
their own zero set. `redistance=False` is kept reachable so the second table
below can show the difference honestly.

**The trigger is advisory, and the threshold is yours.** Nothing here bakes on
its own. Consolidating discards the parameters of everything it absorbs, and an
engine that decided on an artist's behalf that a sphere's radius is no longer
editable would be making the wrong person pay. `field_report` measures and
`advise_below_step_scale` is the tolerance YOU hold, because a tolerance for
marching cost belongs to a viewport and a frame budget rather than to the
artwork.

What survives a consolidation is the surface at the resolution you chose. What
does not is every parameter of every item absorbed — so it is one undo step,
and its inverse hands them all back.
"""

import numpy as np

import pyclay as clay

import _render as R

# The bound this example pins. sqrt(3) is the interpolation factor a volume pays
# whatever its samples do; 1.10 is the slack a redistanced bake is allowed over
# a perfect 1.
SAMPLE_BOUND = 1.10
CELL, BAND = 0.03, 0.12
FACES = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]


def ball(radius=0.62, colour="#9aa3ad"):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=radius), color=colour)
    return doc, layer


def polished(source, normal, distance=0.44):
    """One hPolish pass: plane the form back along a normal, cutting only."""
    u = np.array(normal, np.float32)
    u = u / np.linalg.norm(u)
    point = tuple(map(float, u * distance))
    return clay.Volume.flattened_from(
        source, plane_point=point, plane_normal=tuple(map(float, u)), strength=1.0,
        centre=point, region_radius=0.7, falloff=0.3, cell=CELL, band=BAND, mode="cut")


def wrap(volume, colour="#9aa3ad"):
    doc = clay.Document()
    layer = doc.add_sdf_layer("f")
    layer.add(volume, color=colour)
    return doc, layer


def surface_along(doc, direction, hi=1.4):
    """How far out along a direction the surface sits."""
    u = np.array(direction, np.float32)
    u = u / np.linalg.norm(u)
    ts = np.arange(hi, 0.0, -0.002, dtype=np.float32)
    inside = np.nonzero(doc.eval((ts[:, None] * u[None, :]).astype(np.float32)) <= 0)[0]
    return float(ts[inside[0]]) if len(inside) else float("nan")


def dragged(drags=9):
    """A Move stroke: many drags, each one another grab on the chain."""
    doc, layer = ball(1.0, "#b0463f")
    scales = [doc.safe_step_scale()]
    for i in range(drags):
        layer.move_surface((1.0 + 0.25 * i, 0, 0), (0.25, 0, 0), radius=0.5)
        scales.append(doc.safe_step_scale())
    return doc, layer, scales


def main():
    R.banner("36 consolidation — the policy that lets the region verbs be used twice")

    EYE, TARGET = (2.2, 1.5, 2.2), (0, 0, 0)

    # --- two mechanisms, and the report tells them apart -----------------------
    # An aggregate step scale says something is wrong. These two say which
    # thing, and therefore which cure an app should offer.
    chain, chain_layer = ball()
    for n in FACES[:2]:
        chain, chain_layer = wrap(polished(chain, n))
    _stroke_doc, stroke_layer, stroke_scales = dragged()

    print("  the same aggregate cost, two different causes:")
    print(f"    {'chain':<14}{'step scale':>12}{'steepest volume':>18}"
          f"{'deformer chain':>17}{'advises':>9}")
    for name, layer in (("two polishes", chain_layer), ("nine drags", stroke_layer)):
        r = layer.field_report(advise_below_step_scale=0.25)
        print(f"    {name:<14}{r['safe_step_scale']:>12.4f}{r['steepest_volume']:>18.2f}"
              f"{r['longest_deformer_chain']:>17}{str(r['advises_consolidation']):>9}")
    print("  a policy keyed on only one of them would miss the other")

    polish_report = chain_layer.field_report(advise_below_step_scale=0.25)
    drag_report = stroke_layer.field_report(advise_below_step_scale=0.25)
    if polish_report["longest_deformer_chain"] != 0 or polish_report["steepest_volume"] <= 5.0:
        raise SystemExit("a polish chain stopped degrading through its volume")
    if drag_report["steepest_volume"] > 1.001 or drag_report["longest_deformer_chain"] != 9:
        raise SystemExit("a move stroke stopped degrading through its deformer chain")

    # --- what it costs, before it is paid --------------------------------------
    # The quote IS the bill: it is the real bake with the result thrown away, so
    # it cannot drift from what the real one produces. Asking is a read.
    quote = chain_layer.consolidation_cost(cell=CELL, band=BAND)
    print(f"  consolidating that chain would cost {quote['megabytes']:.2f} MB across "
          f"{quote['brick_count']} bricks ({quote['sample_count']:,} samples) at cell "
          f"{quote['cell_size']:.3f}")
    parametric, parametric_layer = ball()
    before_asking = parametric.safe_step_scale()
    parametric_layer.consolidation_cost(cell=CELL, band=BAND)
    if (parametric_layer.consolidation_state is not None
            or parametric.safe_step_scale() != before_asking):
        raise SystemExit("asking what a consolidation costs consolidated something")

    # --- baking is not enough, and this is the measurement ---------------------
    # The steepness is a property of the FIELD; resampling it onto a lattice
    # reproduces it. Redistancing is what removes it.
    print("  a bake alone does not bound the Lipschitz — redistancing is what does:")
    print(f"    {'bake':<28}{'sample lipschitz':>18}{'step scale':>13}")
    for label, redistance in (("as sampled", False), ("redistanced", True)):
        c = chain_layer.consolidation_cost(cell=CELL, band=BAND, redistance=redistance)
        print(f"    {label:<28}{c['sample_lipschitz']:>18.2f}{c['safe_step_scale']:>13.4f}")
    if chain_layer.consolidation_cost(cell=CELL, band=BAND,
                                      redistance=False)["sample_lipschitz"] <= 5.0:
        raise SystemExit("a plain bake stopped reproducing the chain's steepness — recheck "
                         "whether redistancing is still doing the work")

    # --- the chain, with and without ------------------------------------------
    print("  six polish passes, unconsolidated and consolidated after each:")
    print(f"    {'pass':>5}{'plain lipschitz':>18}{'plain step':>12}"
          f"{'held lipschitz':>17}{'held step':>11}{'held MB':>9}")

    plain, _ = ball()
    held, held_layer = ball()
    plain_declared, held_declared = [], []
    plain_tiles, held_tiles = [], []
    for i in range(6):
        normal = FACES[i % 3]
        plain, _ = wrap(polished(plain, normal))
        held, held_layer = wrap(polished(held, normal))
        cost = held_layer.consolidate(cell=CELL, band=BAND)
        plain_declared.append(1.0 / plain.safe_step_scale())
        held_declared.append(1.0 / held.safe_step_scale())
        print(f"    {i + 1:>5}{plain_declared[-1]:>18.2f}{plain.safe_step_scale():>12.4f}"
              f"{held_declared[-1]:>17.2f}{held.safe_step_scale():>11.4f}"
              f"{cost['megabytes']:>9.2f}")
        if i in (0, 2, 5):
            plain_tiles.append(R.render_array(plain, eye=EYE, target=TARGET,
                                              width=205, height=195))
            held_tiles.append(R.render_array(held, eye=EYE, target=TARGET,
                                             width=205, height=195))
        # The bound, checked at every step rather than only at the end.
        if cost["sample_lipschitz"] > SAMPLE_BOUND:
            raise SystemExit(f"pass {i + 1} consolidated to sample lipschitz "
                             f"{cost['sample_lipschitz']:.3f}, past the stated bound "
                             f"{SAMPLE_BOUND}")

    print(f"  plain: {plain_declared[0]:.2f} -> {plain_declared[-1]:.2f} declared, "
          f"{1.0 / plain.safe_step_scale():.0f}x the marching cost. "
          f"held: {max(held_declared):.2f} at its worst, flat in the chain length")

    if plain_declared[-1] < plain_declared[0] * 5.0:
        raise SystemExit("chaining stopped degrading — recheck whether it is now supported "
                         "without consolidating")
    if max(held_declared) > SAMPLE_BOUND * np.sqrt(3.0):
        raise SystemExit(f"a consolidated chain declared {max(held_declared):.2f}, past the "
                         f"stated bound")
    if abs(surface_along(held, FACES[0]) - 0.44) > 0.04:
        raise SystemExit("six consolidated passes moved the facet off its plane")

    R.contact_sheet(plain_tiles + held_tiles, "36_consolidation_chain.png", columns=3,
                    caption="passes 1, 3 and 6 without consolidating (top) and with it "
                            "(bottom) — chaining bakes samples a volume rather than a "
                            "distance field, so the third pass is corrupt and by the sixth "
                            "there is nothing left to render; consolidated, the sixth pass "
                            "costs what the first did")

    # --- the other mechanism: a stroke of drags --------------------------------
    print("  a Move stroke of nine drags, and the same stroke consolidated:")
    for n in (1, 3, 6, 9):
        print(f"    {n} drag(s): step scale {stroke_scales[n]:.4f}  "
              f"({1.0 / stroke_scales[n]:.0f}x the marching cost)")
    stroke_cost = stroke_layer.consolidate(cell=0.03, band=0.12)
    after = stroke_layer.field_report(advise_below_step_scale=0.25)
    print(f"  consolidated: step scale {after['safe_step_scale']:.4f} "
          f"({stroke_cost['megabytes']:.2f} MB), and the deformer chain is gone")

    if after["longest_deformer_chain"] != 0:
        raise SystemExit("consolidating a move stroke left the deformer chain in place")
    if after["safe_step_scale"] < stroke_scales[9] * 10.0:
        raise SystemExit("consolidating a nine-drag stroke stopped paying for itself")
    if after["advises_consolidation"]:
        raise SystemExit("a consolidated layer still advises consolidation")

    # --- what survives, and what the undo record still holds -------------------
    doc = clay.Document()
    doc.enable_undo()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.7), color="#9aa3ad")
    layer.add(clay.Sphere(r=0.3).at((0.55, 0, 0)), op=clay.Op.SUBTRACT)
    bite = surface_along(doc, (1, 0, 0))

    before_depth = doc.undo_depth
    layer.consolidate(cell=CELL, band=BAND)
    state = layer.consolidation_state
    print(f"  a two-item layer consolidates to samples at cell {state['cell_size']:.3f} — "
          f"one undo step, not two")

    if doc.undo_depth != before_depth + 1:
        raise SystemExit("consolidation was not a single undo step")
    doc.undo()
    if layer.consolidation_state is not None:
        raise SystemExit("undoing a consolidation left the layer baked")
    # Editable BY ITS PARAMETERS again, which is the whole claim: the bite came
    # back as a sphere whose radius can still be changed.
    layer.set_prim(2, clay.Sphere(r=0.45))
    if not surface_along(doc, (1, 0, 0)) < bite:
        raise SystemExit("the absorbed items came back unparametric")

    R.render(held, "36_consolidation.png", eye=(2.3, 1.6, 2.3), target=TARGET,
             caption="six hPolish passes on a sphere, consolidated after each — the chain "
                     "holds its declared Lipschitz at sqrt(3) instead of multiplying, so "
                     "the seventh pass costs what the first did")

    R.export_model(held, "36_consolidation.ply", resolution=80, decimate=0.08)


if __name__ == "__main__":
    main()

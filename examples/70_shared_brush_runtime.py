"""One brush, three representations, and the parts that are allowed to differ.

THE GAP THIS CLOSES, stated as an artist would: a sculpting session moves
between representations. You block the form on a mesh layer, switch to dynamic
topology to pull a horn out of nothing, subdivide for pores. A brush that means
one thing on the fixed mesh and something else on the adaptive surface is not a
brush — it is three brushes wearing one name, and you find out which one you
were using when the result surprises you.

Before this change that was literally true of ONE setting. `automask` reached
`DynamicSculptor::gather` on every stamp and was never read: an artist who
enabled "do not cross onto a face pointing away" got it on the fixed mesh, got
it on the hierarchy, and silently did not get it on the adaptive surface. No
error, a plausible-looking result, and nothing in the library that would say so.

WHAT THE PICTURES SHOW, and what the numbers underneath them assert:

  - ONE PRESET GESTURE, RESOLVED ONCE and replayed on a fixed mesh, an adaptive
    surface and a multiresolution cage built from the SAME source model. The
    three surfaces come out BYTE-IDENTICAL — not close, identical — because a
    Grab reads no vertex normal and therefore has nothing to disagree about.

  - THE AUTOMASK REACHING ALL THREE through one argument, masking the same
    vertices on each. That is the divergence, closed, in the only form that
    proves it: a topological factor is set-valued, so "the same" has to mean
    exactly the same set.

  - WHAT A STROKE'S SCRATCH COSTS, read back through `arena_stats`. A warm dab
    on a stable surface allocates nothing, and the number that says so is
    `growths` — a count that has stopped climbing. A current usage could not:
    it reads zero between stamps whatever the arena is doing.

  - AND WHAT LEGITIMATELY DIFFERS, asserted as a difference rather than hidden.
    Draw reads a vertex normal, the three representations estimate normals
    differently, and neither estimator is wrong. The divergence is real, it is
    about 3e-10 on this model, and an example that claimed otherwise would be
    claiming something false about the library.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (1.5, 1.9, 2.6), (0.0, 0.25, 0.0)

# The automask factors this example drives. Both are TOPOLOGICAL and set-valued
# — a vertex is on the border or it is not, is reachable from the seed or is not
# — which is what makes "the three representations agree" an exact claim rather
# than a tolerance.
BOUNDARY = int(clay.AutomaskFactor.BOUNDARY.value)
TOPOLOGY_CONNECTED = int(clay.AutomaskFactor.TOPOLOGY_CONNECTED.value)


def dome(n=24, half=1.0):
    """A quad-gridded dome: ONE source model that all three representations can
    take without losing anything.

    NO DUPLICATED VERTICES, and that is the fixture rather than a convenience.
    The fixed sculptor addresses work by WELD CLASS, the adaptive surface by a
    generational vertex handle and the hierarchy by (level, vertex); on a mesh
    with seams those three numberings diverge and any comparison between them
    becomes a pairing heuristic. On this one they are the same numbering, so a
    byte comparison is a byte comparison.

    Curved rather than flat, because Draw on a plane would agree everywhere and
    the difference this example exists to name would not appear.
    """
    xs = np.linspace(-half, half, n + 1, dtype=np.float32)
    positions = []
    for z in range(n + 1):
        for x in range(n + 1):
            u, v = float(xs[x]), float(xs[z])
            positions.append((u, 0.5 * (1.0 - 0.5 * (u * u + v * v)), v))
    stride = n + 1
    indices = []
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            b, c, d = a + 1, a + stride, a + stride + 1
            indices += [a, c, b, b, c, d]
    return np.array(positions, np.float32), np.array(indices, np.uint32)


def automask_of(factors, boundary_rings=2):
    a = clay.AutomaskSettings()
    a.factors = int(factors)
    a.boundary_rings = boundary_rings
    return a


def three_sculptors(positions, indices):
    """The same model, converted three ways.

    Each conversion is deliberate and none of them is a mode the others slip
    into — that is the contract the mesh layer's byte-identical index buffer
    rests on.
    """
    fixed_mesh = clay.Mesh.from_triangles(positions, indices)
    fixed = clay.MeshSculptor(fixed_mesh, 0.0)

    surface = clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    adaptive = clay.DynamicSculptor(surface)

    hierarchy_surface = clay.MultiresSurface.from_mesh(
        clay.Mesh.from_triangles(positions, indices))
    hierarchy = clay.MultiresSculptor(hierarchy_surface)

    return (fixed_mesh, fixed), (surface, adaptive), (hierarchy_surface, hierarchy)


def adaptive_permutation(positions, indices):
    """Source vertex index for each row of `DynamicSurface.to_mesh()`.

    THE ADAPTIVE SURFACE HAS ITS OWN VERTEX ORDER, and pretending otherwise
    would be the quiet way to make this example's comparisons meaningless. Its
    vertices live in a slot pool that retires and reuses slots, and `to_mesh`
    emits them in pool order. So the pairing is built ONCE, from the untouched
    surface, against coordinates that are still unique — after a stamp they are
    not, because a stamp is what moves them.
    """
    surface = clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    start = np.asarray(surface.to_mesh().positions)
    key = {tuple(p): i for i, p in enumerate(positions.tolist())}
    if len(key) != len(positions):
        raise SystemExit("the fixture has duplicated vertices; the pairing would be ambiguous")
    order = np.array([key[tuple(p)] for p in start.tolist()], np.int64)
    inverse = np.empty_like(order)
    inverse[order] = np.arange(len(order))
    return inverse


def main():
    R.banner("70 shared brush runtime — one brush, three representations")

    positions, indices = dome()
    inverse = adaptive_permutation(positions, indices)
    tiles, labels = [], []

    print(f"  the source model: {len(positions)} vertices, {len(indices) // 3} triangles,")
    print("  converted three ways — a fixed mesh layer, an adaptive surface with")
    print("  topology changes disabled, and a multiresolution cage at level 0.")

    topology = clay.TopologySettings()
    topology.enabled = False

    # --- 1. one preset gesture, resolved once --------------------------------
    #
    # The preset decides the verb, the spacing, the falloff and the pressure
    # response. Resolving it ONCE and replaying the resolved stamps is what
    # makes this a comparison of the three RUNTIMES rather than of three stroke
    # resolvers that happen to agree.
    preset = clay.BrushPreset.by_name("Move")
    gesture = np.array(
        [[-0.5, 0.6, 0.0, 1.0, 0.0],
         [-0.25, 0.62, 0.0, 1.0, 0.0],
         [0.0, 0.625, 0.0, 1.0, 0.0],
         [0.25, 0.62, 0.0, 1.0, 0.0],
         [0.5, 0.6, 0.0, 1.0, 0.0]],
        np.float32,
    )
    resolved = preset.stroke.resolve(gesture)
    stamps = np.asarray(resolved["positions"])
    radii = np.asarray(resolved["radii"])
    strengths = np.asarray(resolved["strengths"])
    verb = preset.model.verb
    print(f"\n  GESTURE   '{preset.name}' resolves to {len(stamps)} stamps of '{verb}' "
          f"at spacing {preset.stroke.spacing:.3f}.")

    (fixed_mesh, fixed), (surface, adaptive), (cage, hierarchy) = three_sculptors(
        positions, indices)

    drag = (0.03125, 0.0625, 0.0)
    moved = [0, 0, 0]
    for centre, radius, strength in zip(stamps, radii, strengths):
        centre = tuple(float(c) for c in centre)
        radius, strength = float(radius), float(strength)
        common = dict(direction=drag, geodesic=False)
        moved[0] += fixed.stamp(verb, centre, radius, strength, **common)
        moved[1] += adaptive.stamp(verb, centre, radius, strength,
                                   topology=topology, **common)["moved"]
        moved[2] += hierarchy.stamp(verb, centre, radius, strength, **common)

    fixed_after = np.asarray(fixed_mesh.positions)
    adaptive_after = np.asarray(surface.to_mesh().positions)[inverse]
    cage_after = np.asarray(cage.positions_at(0))

    if moved[0] == 0:
        raise SystemExit("the gesture moved nothing, so this example proves nothing")
    if not (moved[0] == moved[1] == moved[2]):
        raise SystemExit(f"the three representations reached different vertex counts: {moved}")

    # BYTE-IDENTICAL, not equal to a tolerance. Every tolerance in this library
    # would admit a re-associated weight, which is exactly the mistake a shared
    # runtime invites — so the claim is about bytes or it is not a claim.
    if fixed_after.tobytes() != adaptive_after.tobytes():
        raise SystemExit("the adaptive surface diverged from the fixed mesh on a normal-free verb")
    if fixed_after.tobytes() != cage_after.tobytes():
        raise SystemExit("the multiresolution cage diverged from the fixed mesh")

    print(f"            all three moved {moved[0]} vertices and wrote BYTE-IDENTICAL")
    print("            positions. A Grab reads no vertex normal, so the three")
    print("            representations have nothing left to disagree about — the")
    print("            walk, the falloff, the weight's factor order and the drop")
    print("            rule are one implementation, not three that agree.")

    for label, mesh in (("fixed mesh", fixed_mesh),
                        ("adaptive surface", surface.to_mesh()),
                        ("multires cage", cage.mesh_at_level(0))):
        tiles.append(R.render_mesh_array(mesh, eye=EYE, target=TARGET, width=260, height=260,
                                         colors=np.tile(np.float32([0.62, 0.68, 0.78]),
                                                        (len(mesh.positions), 1))))
        labels.append(label)

    # --- 2. the automask reaches all three -----------------------------------
    #
    # THE DIVERGENCE THIS CHANGE EXISTS TO CLOSE. One argument, three
    # representations, the same answer.
    print("\n  AUTOMASK  the same two factors, set through the same argument on each:")
    counts = {}
    for name, factors in (("off", 0),
                          ("boundary + connected", BOUNDARY | TOPOLOGY_CONNECTED)):
        (m2, f2), (s2, a2), (c2, h2) = three_sculptors(positions, indices)
        args = dict(direction=drag, geodesic=False, automask=automask_of(factors))
        counts[name] = (
            f2.stamp("grab", (0.0, 0.5, 0.0), 1.4, 0.5, **args),
            a2.stamp("grab", (0.0, 0.5, 0.0), 1.4, 0.5, topology=topology, **args)["moved"],
            h2.stamp("grab", (0.0, 0.5, 0.0), 1.4, 0.5, **args),
        )
        print(f"            {name:<22} fixed {counts[name][0]:>4}   "
              f"adaptive {counts[name][1]:>4}   hierarchy {counts[name][2]:>4}")

    for name, row in counts.items():
        if len(set(row)) != 1:
            raise SystemExit(
                f"the automask '{name}' reached different vertex counts on the three "
                f"representations: {row} — this is the divergence, back")

    # AND IT ACTUALLY MASKED SOMETHING. Without this the equality above is
    # satisfied by three representations that all did nothing, which is
    # precisely the state the adaptive path was in.
    if counts["boundary + connected"][0] >= counts["off"][0]:
        raise SystemExit("the automask masked nothing, so the agreement above proves nothing")
    print(f"            the border fade removed "
          f"{counts['off'][0] - counts['boundary + connected'][0]} vertices, on each.")
    print("            Before this change the adaptive column read the SAME number")
    print("            in both rows: the descriptor carried the factors and the")
    print("            adaptive gather never read them.")

    # --- 3. what a stroke's scratch costs ------------------------------------
    #
    # A TRAP WORTH NAMING RATHER THAN WORKING AROUND SILENTLY: the fixed
    # sculptor's arena reads all zeroes for a stamp whose automask needs no
    # flood. NormalAngle reads the workset's own normals and TopologyConnected
    # returns early on a region that is already one component, so only Boundary
    # reaches the arena there. Every adaptive stamp uses it, which is why the
    # convergence claim below is made on the adaptive path.
    print("\n  SCRATCH   what one stroke's scratch costs, read back per sculptor:")
    (m3, f3), (s3, a3), (c3, h3) = three_sculptors(positions, indices)

    def dab(i, sculptor, adaptive_path=False):
        centre = (-0.0625 + 0.03125 * (i % 6), 0.5, 0.0)
        kw = dict(direction=drag, geodesic=False, automask=automask_of(BOUNDARY))
        if adaptive_path:
            return sculptor.stamp("grab", centre, 1.4, 0.02, topology=topology, **kw)
        return sculptor.stamp("grab", centre, 1.4, 0.02, **kw)

    for i in range(8):
        dab(i, f3)
        dab(i, a3, adaptive_path=True)
        dab(i, h3)
    warm = (f3.arena_stats, a3.arena_stats, h3.arena_stats)
    for i in range(40):
        dab(i, f3)
        dab(i, a3, adaptive_path=True)
        dab(i, h3)
    settled = (f3.arena_stats, a3.arena_stats, h3.arena_stats)

    for label, w, s in zip(("fixed mesh", "adaptive surface", "hierarchy"), warm, settled):
        print(f"            {label:<18} {s['capacity_bytes']:>6} B owned, "
              f"{s['high_water_bytes']:>6} B peak, {s['growths']} growths "
              f"(unchanged over 40 more dabs: {s['growths'] == w['growths']})")

    if a3.arena_stats["capacity_bytes"] == 0:
        raise SystemExit("the adaptive arena spent nothing, so the claim below is untested")
    for label, w, s in zip(("fixed mesh", "adaptive surface", "hierarchy"), warm, settled):
        if s["growths"] != w["growths"]:
            raise SystemExit(
                f"the {label} arena was still growing after warm-up: "
                f"{w['growths']} -> {s['growths']} — scratch that grows a little per dab "
                "allocates nothing after warm-up and consumes memory without bound")
        if s["high_water_bytes"] > s["capacity_bytes"]:
            raise SystemExit(f"the {label} arena's peak exceeds what it owns")
    print("            forty more dabs of the same footprint took no more storage.")
    print("            That is the number to watch: an allocation count cannot see")
    print("            scratch that grows a little every stamp and is never reset.")

    # --- 4. and what legitimately differs ------------------------------------
    #
    # A gate that only checked agreement would be satisfied by two
    # representations that had become equally wrong, and a divergence that
    # suddenly vanished would mean an estimator had been silently unified —
    # which is a real change to what a brush does.
    print("\n  DIFFERS   Draw reads a vertex normal, and the estimators are not the same:")
    (m4, f4), (s4, a4), (c4, h4) = three_sculptors(positions, indices)
    args = dict(geodesic=False)
    n_fixed = f4.stamp("draw", (0.0, 0.5, 0.0), 0.4, 0.5, **args)
    n_adaptive = a4.stamp("draw", (0.0, 0.5, 0.0), 0.4, 0.5, topology=topology, **args)["moved"]
    n_cage = h4.stamp("draw", (0.0, 0.5, 0.0), 0.4, 0.5, **args)

    draw_fixed = np.asarray(m4.positions)
    draw_adaptive = np.asarray(s4.to_mesh().positions)[inverse]
    draw_cage = np.asarray(c4.positions_at(0))

    if not (n_fixed == n_adaptive == n_cage):
        raise SystemExit(f"Draw reached different vertices: {n_fixed}, {n_adaptive}, {n_cage}")

    divergence = float(np.abs(draw_fixed - draw_adaptive).max())
    displacement = float(np.abs(draw_fixed - positions).max())
    print(f"            the fixed mesh weights each face normal by its corner angle,")
    print("            which reaches `acos`; the adaptive surface averages the face")
    print("            normals it already caches. Both are correct and they differ.")
    print(f"            worst disagreement {divergence:.3e} against a displacement of "
          f"{displacement:.3e}")

    if draw_fixed.tobytes() == draw_adaptive.tobytes():
        raise SystemExit(
            "Draw is now byte-identical between the fixed and adaptive paths. Either an "
            "estimator was silently unified — a real change to what the brush does — or this "
            "example's fixture stopped being curved")
    if divergence > displacement * 1e-3:
        raise SystemExit(
            f"the two representations have drifted apart: {divergence:.3e} is not a last-bit "
            f"disagreement against a displacement of {displacement:.3e}")
    # The HIERARCHY, at its cage, is the fixed sculptor — it binds a MeshSculptor
    # to level 0's own mesh and calls it — so it must not differ at all.
    if draw_fixed.tobytes() != draw_cage.tobytes():
        raise SystemExit("the hierarchy's cage differs from the fixed mesh, which it IS")
    print("            the hierarchy's cage is byte-identical, because at level 0 it")
    print("            IS the fixed sculptor rather than a second implementation of it.")

    R.contact_sheet(tiles, "70_shared_brush_runtime.png", columns=3,
                    caption=" | ".join(labels))

    obj = R.output_path("70_shared_brush_adaptive.obj")
    surface.to_mesh().save(obj)
    print(f"\n  wrote {os.path.basename(obj)} for inspection.")
    print("  one brush model, one weight, one factor order — and the three")
    print("  differences that remain are named rather than discovered.")


if __name__ == "__main__":
    main()

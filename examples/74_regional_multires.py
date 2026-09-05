"""Level 5 on the nose, and the boots left alone.

THE GAP THIS CLOSES, stated as an artist would: a hierarchy refines uniformly,
so asking for the resolution a face needs means paying for it on the shins as
well. Catmull-Clark multiplies faces by four a level, so three levels past what
the boots need is sixty-four times the geometry nobody will ever sculpt — and on
a device with a memory ceiling that is the difference between a session and a
refusal.

WHAT WAS ACTUALLY MEASURED FIRST, because the intuitive answer is wrong. A level
of a 1,600-patch cage costs 121 MB at level 4, and NONE of it is coefficients:
`DetailField` is already sparse and an unsculpted level costs 0.0 MB of detail.
What a level costs is TOPOLOGY, the evaluated buffers and the chunk index, all
of which follow the face count. So the sparsity here is in the topology and what
derives from it, and a design that had made the coefficients sparser would have
delivered nothing.

THE RULE, and it is the one `VoxelGrid.add_level_region` already states for a
lattice: outside the refined patches the level has no storage, and the patch is
read at its own depth. Only what is STORED changes.

WHY IT IS WATERTIGHT, and it is by construction rather than by repair. Every
vertex a regional level stores is computed by the same stencil, against the same
parent neighbourhood, as the uniformly refined hierarchy would have used — so it
holds the same bits. A fine patch's boundary IS the exact subdivision of the
coarse edge it meets, because it is literally the same arithmetic. That is why
`refine_patches_to_level` grades the levels beneath: a patch whose neighbour is
missing one level down would see that shared edge as an open BORDER, and
Catmull-Clark's border rule is a different rule.

WHAT THE NUMBERS ASSERT: the refined patches carry the uniform hierarchy's own
positions bit for bit, the memory and the evaluation work follow the refined
area, and depth is a property of a patch rather than of the surface.

WHAT THIS DOES NOT YET DO, said plainly rather than hidden behind a picture.
`mesh_at_level(4)` on a regional hierarchy exports the faces that level HAS,
which is the region and not the model — the pictures below frame it as the patch
it is. Assembling a mixed-depth hierarchy into ONE mesh needs transition
polygons on the coarse side of every boundary, because a fine patch's corner
vertex has taken one more subdivision step than the coarse neighbour's has, and
the two are a step apart rather than a hairline apart. Those polygons are
derived display data and are the next piece of this change; the storage and the
evaluation underneath them are what is finished here.

Run: python examples/74_regional_multires.py
"""

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (1.9, 1.2, 3.1), (0.0, 0.0, 0.0)


def cage(n=6, radius=1.0):
    """A cube-sphere as triangles: patch `i` is triangle `i` of this list."""
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
                # Outward, for the reason 68_mesh_multires records.
                if signs[f] > 0.0:
                    indices += [a, b, c2, b, d, c2]
                else:
                    indices += [a, c2, b, b, c2, d]
    p = np.array(positions, dtype=np.float32)
    i = np.array(indices, dtype=np.uint32)
    return clay.Mesh.from_triangles(p, i), p, i


def patches_near(positions, indices, centre, radius):
    """The base patches whose face centroid falls inside a ball.

    SELECTION IS SEPARATE FROM REFINEMENT, and that separation is the API's:
    the core call takes a patch LIST, and a region helper like this one sits on
    top. A test that names patches is deterministic; a test that names a world
    ball is a test of the ball.
    """
    tris = indices.reshape(-1, 3)
    centroids = positions[tris].mean(axis=1)
    inside = np.linalg.norm(centroids - np.asarray(centre, dtype=np.float32), axis=1) <= radius
    return [int(i) for i in np.flatnonzero(inside)]


def preview(mesh, cell=0.012, colour="#b0784a"):
    """A display-only document; a mesh layer is never evaluated, so the
    renderer has nothing to trace until the mesh is resampled."""
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell), color=colour)
    return doc


def main():
    R.banner("74 regional multires — depth where it is needed")

    base, positions, indices = cage(6, 1.0)
    patches = base.triangle_count
    region = patches_near(positions, indices, (0.0, 0.0, 1.0), 0.55)
    print(f"  the cage is {patches} patches; {len(region)} of them are the region.")

    # --- the same cage, refined two ways --------------------------------------
    uniform = clay.MultiresSurface.from_mesh(base)
    partial = clay.MultiresSurface.from_mesh(base)
    for _ in range(4):
        uniform.add_level()
    partial.refine_patches_to_level(region, 4)

    print(f"\n  uniform : {uniform.level_counts(4)['faces']:>8} faces at level 4")
    print(f"  regional: {partial.level_counts(4)['faces']:>8} faces at level 4")
    print(f"  depth is per patch now: uniform_depth is {partial.uniform_depth}")

    # --- 0. the cold evaluation, before anything has asked for a position -----
    #
    # Measured HERE and not later, because an evaluation happens once: every
    # comparison below reads the cached result, and stats taken after them would
    # be counting nothing. COUNTED, not timed — a wall clock would measure the
    # machine as much as the change.
    for s in (uniform, partial):
        s.reset_eval_stats()
        s.positions_at(4)
    uw = uniform.eval_stats()["vertices_evaluated"]
    rw = partial.eval_stats()["vertices_evaluated"]

    # --- 1. the refined patches hold the uniform hierarchy's own numbers -------
    #
    # BIT-IDENTICAL, not close. Compared by walking each patch's faces in order:
    # a regional level numbers its vertices compactly, so the same point on the
    # surface has a different id in the two hierarchies, but a patch's faces are
    # a contiguous run emitted parent-face by parent-face in both.
    def patch_corners(surface, level, patch):
        topo = surface.topology_at(level)
        pos = np.asarray(surface.positions_at(level))
        faces = topo["face_patch"]
        corners = topo["corners"].reshape(-1, 4)
        return pos[corners[faces == patch].reshape(-1)]

    worst, checked = 0.0, 0
    for patch in region:
        a = patch_corners(uniform, 4, patch)
        b = patch_corners(partial, 4, patch)
        assert a.shape == b.shape, (patch, a.shape, b.shape)
        worst = max(worst, float(np.abs(a - b).max()))
        checked += 1
    print(f"\n  EXACT     {checked} refined patches compared against the uniform")
    print(f"            hierarchy, worst |difference| {worst:.1e} — the same stencils")
    print("            against the same parent, so the same bits.")
    assert worst == 0.0, worst

    # --- 2. depth is a property of a patch ------------------------------------
    depths = [partial.patch_max_level(p) for p in range(patches)]
    histogram = {d: depths.count(d) for d in sorted(set(depths))}
    print(f"\n  GRADED    patches by depth: {histogram}")
    print("            the named region reaches 4; each ring below it one less,")
    print("            which is exactly what every level above needs in order to")
    print("            evaluate against a complete parent.")
    assert all(partial.patch_max_level(p) == 4 for p in region)
    assert min(depths) < 4

    # --- 3. memory and work follow the refined area ---------------------------
    u, r = uniform.memory(), partial.memory()
    print("\n  COST      topology   evaluated  total")
    print(f"    uniform {u['topology'] / 1e6:>8.2f}M {u['evaluated'] / 1e6:>10.2f}M "
          f"{u['total'] / 1e6:>6.2f}M")
    print(f"    region  {r['topology'] / 1e6:>8.2f}M {r['evaluated'] / 1e6:>10.2f}M "
          f"{r['total'] / 1e6:>6.2f}M")
    print(f"            total is {u['total'] / max(r['total'], 1):.1f}x smaller.")
    assert r["total"] * 2 < u["total"]

    print(f"\n  WORK      a cold evaluation of level 4 touched {uw} vertices")
    print(f"            uniformly and {rw} regionally — counted, not timed,")
    print(f"            which is {uw / max(rw, 1):.1f}x less arithmetic for the same nose.")
    assert rw * 2 < uw

    # --- 4. the pictures ------------------------------------------------------
    #
    # The region is framed ON ITSELF rather than at the model's framing. What a
    # regional level holds IS a patch, and drawn beside the whole sphere it
    # would read as a broken model rather than as the thing that was asked for.
    NEAR, NEAR_TARGET = (0.35, 0.30, 2.25), (0.0, 0.0, 0.95)
    tiles, labels = [], []
    tiles.append(R.render_tile(preview(uniform.mesh_at_level(2)), eye=EYE, target=TARGET,
                               size=190))
    labels.append("the model, at the depth it needs")
    tiles.append(R.render_tile(preview(partial.mesh_at_level(partial.max_level)),
                               eye=NEAR, target=NEAR_TARGET, size=190))
    labels.append("what level 4 holds: the region")

    # A dab at the fine level, in the region that has one.
    partial.sculpt_level = partial.max_level
    partial.display_level = partial.max_level
    sculptor = clay.MultiresSculptor(partial)
    sculptor.begin_stroke()
    moved = sculptor.stamp("draw", center=(0.0, 0.0, 1.0), radius=0.22, strength=0.45)
    print(f"\n  SCULPT    a dab at level {partial.max_level} moved {moved} vertices;")
    print("            it is an ordinary level, and the brush does not know it is regional.")
    assert moved > 0
    tiles.append(R.render_tile(preview(partial.mesh_at_level(partial.max_level)),
                               eye=NEAR, target=NEAR_TARGET, size=190))
    labels.append("a dab at the fine level")

    R.contact_sheet(tiles, "74_regional_multires.png", columns=3,
                    caption=" / ".join(labels))


if __name__ == "__main__":
    main()

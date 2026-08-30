"""Voxel remesh — throwing the topology away on purpose.

Everything else in this gallery preserves something. Sculpting a mesh layer
keeps its triangles byte for byte; dynamic topology adapts them locally;
decimation removes some and keeps the rest. This does none of that. It samples
the whole surface into a signed volumetric field at a spatial resolution you
choose and reconstructs a completely new surface from it — the operation
sculpting applications call DynaMesh or Voxel Remesh.

That is worth doing for exactly two situations, and both are here:

**Stretched topology.** Pull a tendril out of a coarse ball and the triangles
along it grow until the surface is useless. Nothing local repairs that; the
density has to be rebuilt from scratch.

**Intersecting parts.** Two shells that overlap are two shells, however solid
they look. A volumetric rebuild fuses them into one body because the field only
knows where the material is, not which mesh claimed it.

And it is worth being honest about the price, which is why this script prints
it rather than describing it:

- vertex and polygon identity are gone — nothing maps old indices to new;
- **UVs are dropped**, not reprojected, because a spatially resampled UV across
  a seam is a stretched layout that looks like a preserved one;
- **details finer than the voxel size disappear**, which is the whole reason
  the resolution is a control rather than a constant.

Vertex colour DOES survive, because colour can be resampled from the source's
geometry by closest point, and so can a mask.
"""

import numpy as np

import pyclay as clay

import _render as R

def frame(mesh, margin=1.25):
    """A camera that frames a MESH. The gallery's cameras are usually derived
    from a layer's bounds, and there is no layer here — a remesh result is
    triangles and nothing else."""
    p = np.asarray(mesh.positions, dtype=np.float64)
    return R.orbit_camera((p.min(axis=0), p.max(axis=0)), margin=margin)


def coloured_ball(radius=0.62, colour="#c8623a", position=(0.0, 0.0, 0.0), resolution=40,
                  banded=False):
    """A meshed sphere carrying vertex colour, so a render shows the surface
    rather than a silhouette. `banded` blends a second colour into the top,
    which is what makes a colour transfer visible rather than merely reported."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("ball")
    layer.add(clay.Sphere(r=radius, position=position), color=colour)
    if banded:
        cap = (position[0], position[1] + radius * 0.55, position[2])
        layer.add(clay.Sphere(r=radius * 0.72, position=cap), color="#e8c247",
                  blend=clay.Smooth(radius * 0.25))
    return doc.mesh(resolution=resolution)


# The spikes are the point of the two-resolution comparison, and their size is
# chosen rather than picked. Each is 0.07 thick. At COARSE that is barely one
# voxel across, which is the scale at which a marched isosurface has no sample
# to put inside the feature and loses it entirely; at FINE it is eight, which is
# plenty. That makes "details finer than the voxel size disappear" a picture
# rather than a sentence, and the estimate's thin-feature warning fires on the
# coarse one, which is how a host would have said so first.
SPIKE_RADIUS = 0.035
SPIKE_LENGTH = 0.30
SPIKE_COUNT = 10
COARSE, FINE = 32, 192


def spiked_ball(radius=0.62, resolution=112):
    """A banded ball wearing a ring of thin spikes — detail at a known scale."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("ball")
    layer.add(clay.Sphere(r=radius), color="#c8623a")
    layer.add(clay.Sphere(r=radius * 0.72, position=(0.0, radius * 0.55, 0.0)),
              color="#e8c247", blend=clay.Smooth(radius * 0.25))
    for i in range(SPIKE_COUNT):
        a = 2.0 * np.pi * i / SPIKE_COUNT
        direction = np.array([np.cos(a), 0.0, np.sin(a)])
        inner = direction * (radius - 0.06)
        outer = direction * (radius + SPIKE_LENGTH)
        layer.add(clay.Capsule(a=tuple(inner), b=tuple(outer), r=SPIKE_RADIUS),
                  color="#7fb04a")
    return doc.mesh(resolution=resolution)


def stretched(mesh, pull=1.5):
    """Pull the top of a mesh into a spike by moving its vertices — the shape a
    snakehook leaves, with the triangles along the pull stretched far past
    anything else on the surface.

    The vertices move and the INDICES do not, which is the point: this is what
    a fixed-topology brush can do, and the stretch is what it cannot undo."""
    p = np.array(mesh.positions, dtype=np.float32)
    idx = np.array(mesh.indices, dtype=np.uint32)
    t = np.clip(p[:, 1] / 0.62, 0.0, None)
    p[:, 1] += pull * (t ** 3) * 0.62
    out = clay.Mesh.from_triangles(p, idx)
    # from_triangles takes geometry only, so the paint is put back the same way
    # a remesh puts it back — spatially, from the shape it came off. Generous
    # threshold: the tendril's tip is a long way from where it started.
    out.transfer_attributes(mesh, uvs=False, max_distance=3.0)
    return out


def two_shells():
    """Two spheres whose interiors overlap, handed over as ONE mesh with no
    shared vertices — two components, one input.

    Concatenated in numpy rather than unioned in a document, and the difference
    is the whole fixture: a union is one surface before anything remeshes it,
    while this really is two shells passing through each other, which is what a
    kitbash hands an artist."""
    a = coloured_ball(0.52, "#c8623a", (-0.34, 0.0, 0.0))
    b = coloured_ball(0.52, "#3f7fc0", (0.34, 0.0, 0.0))
    pa, pb = np.array(a.positions), np.array(b.positions)
    ia, ib = np.array(a.indices, dtype=np.uint32), np.array(b.indices, dtype=np.uint32)
    positions = np.concatenate([pa, pb]).astype(np.float32)
    indices = np.concatenate([ia, ib + len(pa)]).astype(np.uint32)
    pair = clay.Mesh.from_triangles(positions, indices)

    # Paint from the union of the same two spheres, so each shell keeps its own
    # colour and the remesh has something to carry across.
    doc = clay.Document()
    layer = doc.add_sdf_layer("pair")
    layer.add(clay.Sphere(r=0.52, position=(-0.34, 0.0, 0.0)), color="#c8623a")
    layer.add(clay.Sphere(r=0.52, position=(0.34, 0.0, 0.0)), color="#3f7fc0")
    pair.transfer_attributes(doc.mesh(resolution=48), uvs=False, max_distance=0.3)
    return pair


def longest_edge(mesh):
    """How far the topology has been stretched, in world units — one number a
    reader can compare before and after."""
    p = np.asarray(mesh.positions, dtype=np.float64)
    idx = np.asarray(mesh.indices, dtype=np.int64).reshape(-1, 3)
    worst = 0.0
    for a, b in ((0, 1), (1, 2), (2, 0)):
        d = np.linalg.norm(p[idx[:, a]] - p[idx[:, b]], axis=1)
        worst = max(worst, float(d.max()))
    return worst


def edge_scale(mesh):
    """Mean incident edge length per vertex — how densely the topology is
    tessellated THERE, which is the quantity a stretch ruins and a remesh
    rebuilds."""
    p = np.asarray(mesh.positions, dtype=np.float64)
    idx = np.asarray(mesh.indices, dtype=np.int64).reshape(-1, 3)
    total = np.zeros(len(p))
    count = np.zeros(len(p))
    for a, b in ((0, 1), (1, 2), (2, 0)):
        ia, ib = idx[:, a], idx[:, b]
        d = np.linalg.norm(p[ia] - p[ib], axis=1)
        np.add.at(total, ia, d)
        np.add.at(total, ib, d)
        np.add.at(count, ia, 1)
        np.add.at(count, ib, 1)
    return np.divide(total, count, out=np.zeros_like(total), where=count > 0)


def density_colours(mesh, lo, hi):
    """The edge scale as a colour ramp, cool where the tessellation is fine and
    hot where it is stretched. A SHARED lo/hi across every tile, or the two
    pictures would each normalise to themselves and show nothing."""
    t = np.clip((edge_scale(mesh) - lo) / max(hi - lo, 1e-9), 0.0, 1.0)[:, None]
    cool = np.array([0.16, 0.42, 0.78])
    hot = np.array([0.93, 0.72, 0.20])
    return cool * (1.0 - t) + hot * t


def cutaway(mesh):
    """The near half removed and the remaining triangles turned inside out, so
    the render shows the INTERIOR.

    Necessary rather than decorative: from the outside a pair of intersecting
    shells and the single body they fuse into look identical, because the
    difference between them is entirely inside. Cut the front off and it is
    obvious — the pair still has the second shell's wall running through it,
    and the fused body has one continuous inner surface and nothing else.

    The winding is reversed because the mesh rasteriser culls back faces, and
    after a cut the surface facing the camera IS the back face."""
    p = np.asarray(mesh.positions, dtype=np.float32)
    idx = np.asarray(mesh.indices, dtype=np.int64).reshape(-1, 3)
    keep = idx[p[idx].mean(axis=1)[:, 2] <= 0.0]
    flipped = keep[:, [0, 2, 1]].astype(np.uint32).ravel()
    # from_triangles keeps every position, so the source's colour array still
    # lines up index for index.
    return clay.Mesh.from_triangles(p, flipped), np.asarray(mesh.colors)


def show_estimate(mesh, resolution):
    """What a host would put in front of an artist BEFORE committing."""
    e = mesh.voxel_remesh_estimate(resolution=resolution)
    print(f"  resolution {resolution:>4}  voxel size {e['voxel_size']:.4f}  "
          f"grid {e['grid_dimensions'][0]}x{e['grid_dimensions'][1]}x"
          f"{e['grid_dimensions'][2]}")
    print(f"                  ~{e['active_samples'] / 1e6:.2f} M band samples, "
          f"~{e['memory_bytes'] / (1024 * 1024):.0f} MB, "
          f"{e['triangle_min'] / 1000:.0f}-{e['triangle_max'] / 1000:.0f} K triangles")
    warnings = []
    if e["open_boundaries"]:
        warnings.append(f"{e['boundary_edges']} open boundary edges will be closed")
    if e["thin_feature_warning"]:
        warnings.append("thin details may disappear at this resolution")
    for w in warnings:
        print(f"                  warning: {w}")
    return e


def main():
    R.banner("67 voxel remesh — the topology is replaced, and that is the feature")

    # --- stretched topology --------------------------------------------------
    print("\nA spiked ball with a tendril pulled out of it. The vertices moved;")
    print("the triangles did not, so the ones along the pull are stretched — and")
    print("the spikes are detail at a known scale, to show what a voxel size costs.\n")
    base = spiked_ball()
    pulled = stretched(base)

    show_estimate(pulled, 64)
    show_estimate(pulled, 160)

    coarse, coarse_report = pulled.voxel_remesh(resolution=64)
    fine, fine_report = pulled.voxel_remesh(resolution=160)

    print(f"\n  source:            {pulled.triangle_count:>7} triangles, "
          f"longest edge {longest_edge(pulled):.4f}")
    for name, mesh, report in (("remesh at 64", coarse, coarse_report),
                               ("remesh at 160", fine, fine_report)):
        print(f"  {name:<18} {mesh.triangle_count:>7} triangles, "
              f"longest edge {longest_edge(mesh):.4f}  "
              f"(voxel {report['voxel_size']:.4f})")
    print("\n  The longest edge lands on the voxel size, not on the source's")
    print("  stretch: an isosurface vertex sits on a lattice edge, so no")
    print("  triangle can span more than one cell. THAT is uniform density.")

    assert longest_edge(coarse) < longest_edge(pulled)
    assert fine.triangle_count > coarse.triangle_count
    assert coarse_report["result_watertight"] and coarse_report["result_manifold"]

    # --- what it costs -------------------------------------------------------
    print("\n  what did not survive:")
    print(f"    vertex identity   {len(pulled.positions)} in, {len(coarse.positions)} out — "
          "no index maps one to the other")
    print(f"    uvs               dropped: {coarse_report['uvs_dropped']}")
    print(f"    colour            transferred: {coarse_report['colors_transferred']} "
          "(resampled by closest point, not by index)")
    print(f"    volume            {coarse_report['source_volume']:.4f} -> "
          f"{coarse_report['result_volume']:.4f}  "
          f"({coarse_report['relative_volume_error'] * 100:.2f}% off)")
    assert coarse_report["colors_transferred"]

    # --- intersecting shells -------------------------------------------------
    print("\nTwo overlapping spheres handed over as one mesh. Two shells in,")
    print("one body out — the field only knows where the material is.\n")
    pair = two_shells()
    fused, fused_report = pair.voxel_remesh(resolution=56)
    print(f"  components   {fused_report['source_components']} before, "
          f"{fused_report['result_components']} after")
    print(f"  triangles    {fused_report['source_triangles']} before, "
          f"{fused_report['result_triangles']} after")
    print(f"  watertight   {fused_report['result_watertight']}, "
          f"2-manifold {fused_report['result_manifold']}, "
          f"oriented {fused_report['result_oriented']}")
    assert fused_report["source_components"] == 2
    assert fused_report["result_components"] == 1
    assert fused_report["result_watertight"]

    # --- a mask survives it --------------------------------------------------
    # A mask is a per-vertex gate a host owns, and the remesh destroys the
    # vertices it was indexed by. It comes across the same way colour does:
    # spatially, from the source's geometry.
    src_p = np.asarray(pulled.positions)
    mask = (src_p[:, 0] > 0.0).astype(np.float32)
    moved = fine.transfer_vertex_scalar(pulled, mask,
                                        max_distance=fine_report["voxel_size"] * 2.0)
    out_p = np.asarray(fine.positions)
    clear = np.abs(out_p[:, 0]) > 4.0 * fine_report["voxel_size"]
    agree = ((out_p[clear, 0] > 0.0) == (moved[clear] > 0.5)).all()
    print(f"\n  a mask over the source's +x half, resampled onto {len(moved)} new")
    print(f"  vertices: every vertex clear of the boundary agrees — {agree}")
    assert agree

    # --- the pictures --------------------------------------------------------
    # render_mesh_array, not render_array: the ordinary preview traces a
    # DOCUMENT, and what this feature does is visible in the vertices.
    #
    # WHAT A COARSER VOXEL COSTS, on the unpulled ball so the studs are large
    # in frame. The studs stick out by less than the 64-resolution voxel and by
    # far more than the 160 one, so the middle tile has lost them and the right
    # one has not.
    plain_coarse, coarse_plain_report = base.voxel_remesh(resolution=COARSE)
    plain_fine, fine_plain_report = base.voxel_remesh(resolution=FINE)
    thick = SPIKE_RADIUS * 2.0
    print(f"\n  a spike is {thick:.2f} thick. At resolution {COARSE} a voxel is "
          f"{coarse_plain_report['voxel_size']:.3f} "
          f"({thick / coarse_plain_report['voxel_size']:.1f} voxels across a spike);")
    print(f"  at {FINE} it is {fine_plain_report['voxel_size']:.3f} "
          f"({thick / fine_plain_report['voxel_size']:.1f} across). The estimate says so "
          "first:")
    for r in (COARSE, FINE):
        e = base.voxel_remesh_estimate(resolution=r)
        print(f"    resolution {r:>4}: thin-detail warning {e['thin_feature_warning']}")

    # A sub-voxel feature does not fade out politely; it ALIASES, breaking into
    # disconnected crumbs where the lattice happened to sample inside it. That
    # is what the component policy is for — and why it is opt-in with an
    # explicit threshold rather than automatic, because nothing here can tell a
    # crumb from a tooth.
    print(f"\n  at {COARSE} the spikes did not fade, they broke up: "
          f"{coarse_plain_report['result_components']} components")
    cleaned, cleaned_report = base.voxel_remesh(resolution=COARSE,
                                               minimum_component_volume=0.02)
    print(f"  removing components under 0.02 cubic units drops "
          f"{cleaned_report['removed_components']} of them, leaving "
          f"{cleaned_report['result_components']}")
    assert coarse_plain_report["result_components"] > 1
    assert cleaned_report["result_components"] == 1
    del cleaned
    eye, target = frame(base, margin=1.05)
    R.contact_sheet(
        [R.render_mesh_array(m, eye=eye, target=target, width=300, height=300)
         for m in (base, plain_coarse, plain_fine)],
        "67_voxel_remesh_resolution.png", columns=3,
        caption=f"the spiked source, rebuilt at {COARSE}, and rebuilt at {FINE} — a spike is "
                f"about one voxel thick at the coarse size, so the coarse rebuild has "
                f"broken them into crumbs")

    # WHAT THE TOPOLOGY LOOKS LIKE, which the silhouettes cannot show: the same
    # two surfaces coloured by their own edge lengths on one shared scale. The
    # source's stretch is a hot streak up the tendril; the rebuild is one
    # colour, because that is what uniform density means.
    scales = np.concatenate([edge_scale(pulled), edge_scale(fine)])
    lo, hi = float(np.percentile(scales, 2)), float(np.percentile(scales, 98))
    pulled_eye, pulled_target = frame(pulled)
    R.contact_sheet(
        [R.render_mesh_array(m, eye=pulled_eye, target=pulled_target, width=260,
                             height=260, colors=density_colours(m, lo, hi))
         for m in (pulled, fine)],
        "67_voxel_remesh_density.png", columns=2,
        caption="edge length per vertex on one shared scale — the pulled source's "
                "stretched streak, and the rebuild that has no streak to show")

    # WHAT FUSION ACTUALLY CHANGES, which is invisible from outside: cut the
    # front off both. The source still has the blue shell's wall running
    # through the orange one; the rebuild has one inner surface and no
    # partition, because a field knows where material is and not which mesh
    # claimed it.
    pair_eye, pair_target = frame(pair, margin=1.1)
    cut_tiles = []
    for m in (pair, fused):
        cut, cut_colours = cutaway(m)
        cut_tiles.append(R.render_mesh_array(cut, eye=pair_eye, target=pair_target,
                                             width=320, height=320, colors=cut_colours))
    R.contact_sheet(cut_tiles, "67_voxel_remesh_fusion.png", columns=2,
                    caption="both cut open: two shells crossing keep an interior wall, "
                            "and the body they fuse into has none")

    print("\n  Use it after stretching, kitbashing or a long adaptive session.")
    print("  Do NOT use it to preserve a retopology or a uv layout: it destroys")
    print("  both by design, and no setting changes that.")


if __name__ == "__main__":
    main()

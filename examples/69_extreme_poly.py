"""A dab costs what it TOUCHES, not what the model HOLDS.

THE GAP THIS CLOSES, stated as an artist would: on a model an artist actually
wants to finish — twenty million triangles, pores and cloth folds and skin —
every dab gets slower as the model gets bigger, even though the brush is the
same size and is touching the same amount of surface. Nothing about the dab
changed; the model did. That is the whole complaint, and it is what makes a
detailing pass on a heavy model feel like working through treacle.

The cause is never the deformation. It is everything AROUND it that was written
as a sweep: which vertices are under the brush, which normals need recomputing,
what the viewport has to be handed afterwards. Each of those is easy to write as
a pass over the model and each of them is invisible until the model is large.

So the runtime is built on ONE UNIT — a chunk of a few hundred faces that is at
once the spatial-index leaf, the brush's candidate set, the parallel work unit,
the normal-recompute unit, the dirty set and the host's upload unit. A dab
touches chunks. Everything downstream is a walk over those and nothing else.

WHAT THE PICTURES SHOW, and what the numbers underneath them assert:

  - THE SAME DAB ON TWO MODELS, one sixteen times the other at the SAME detail
    — more of the same geometry, never a more finely subdivided version of it,
    because that would grow the brush's footprint along with the model and the
    comparison would be measuring the fixture. The touched patch is lit; it is
    the same patch on both, and on the big sheet it is a speck.

  - THE STREAM IS THE SURFACE. What a host is handed per dab is reconstructed
    here and compared, triangle by triangle, against the whole-surface path the
    library still ships. An incremental preview that disagrees with the engine
    draws something the engine does not think it made, and nothing in the
    pixels says so.

  - WHAT IT COSTS, split three ways — the work, the caches, the undo — because
    a host answering a memory warning does not need to know how big the document
    is. It needs to know WHICH PART, since that decides what it is allowed to
    release. A trim at the hardest pressure there is releases the caches and
    leaves the authoritative checksum untouched.

  - AND WHAT AN OPERATION WILL COST BEFORE IT IS PAID, because the peak — not
    the steady state — is what terminates an application on a memory-constrained
    device, and an engine that discovers this by being killed cannot say what
    happened.
"""

import math
import os

import numpy as np

import pyclay as clay

import _render as R

# Small enough to render, and the ratio is what the claim is about.
SMALL, LARGE = 48, 192          # quads a side
SPACING = 0.02                  # world units between vertices, THE SAME on both
BRUSH_RADIUS = 0.20             # world units, THE SAME on both
TARGET_FACES = 128              # the measured chunk size; see bench_surface_chunks


def plane(n, spacing=SPACING):
    """A patch at FIXED SPACING whose extent grows with `n`.

    THE FIXTURE IS THE EXPERIMENT. "A bigger model" here means more of the same
    geometry at the same detail; a more finely subdivided patch would put more
    vertices under a brush of the same radius, and then a dab costing more on
    the bigger model would prove nothing at all.

    Measured outward from the CENTRE rather than from a corner, because
    `-half + spacing * x` rounds differently at two sizes: the same world point
    comes out a few ulps apart on a 48-quad grid and a 192-quad one, the ball
    admits a slightly different set of vertices, and the counts stop being
    comparable for a reason that has nothing to do with the runtime.
    """
    centre = n // 2
    xs = (np.arange(n + 1) - centre) * spacing
    grid_x, grid_z = np.meshgrid(xs, xs, indexing="xy")
    # A gentle relief, so the shading has something to show and the normals
    # have something to be.
    y = 0.012 * np.sin(grid_x * 9.0) * np.cos(grid_z * 9.0)
    positions = np.stack([grid_x, y, grid_z], axis=-1).reshape(-1, 3).astype(np.float32)

    stride = n + 1
    a = (np.arange(n)[:, None] * stride + np.arange(n)[None, :]).reshape(-1)
    b, c, d = a + 1, a + stride, a + stride + 1
    faces = np.stack([np.stack([a, c, b], axis=1), np.stack([b, c, d], axis=1)],
                     axis=1).reshape(-1, 3).astype(np.uint32)
    return clay.Mesh.from_triangles(positions, faces)


def chunks_reached(view, centre, radius):
    """The chunks a dab of this radius reaches, by their bounds.

    A FIXED MESH'S VIEW CARRIES A PARTITION AND NO DIRTY SET, deliberately: the
    fixed sculptor still tracks dirty weld classes and the chunk table has not
    taken that over. So this asks the partition directly, which is exactly what
    the candidate-set half of the query does before the exact footprint test.
    """
    lo = np.array(centre) - radius
    hi = np.array(centre) + radius
    hit = []
    for i in range(view.chunk_count):
        info = view.chunk_info(i)
        if not info["live"]:
            continue
        cmin, cmax = np.array(info["bounds_min"]), np.array(info["bounds_max"])
        if np.all(cmax >= lo) and np.all(cmin <= hi):
            hit.append(i)
    return hit


def upload_bytes(view, chunks):
    """What a host is handed for these chunks: positions, in bytes.

    POSITIONS AND NOT INDICES, and that is the point of carrying four revisions
    rather than one. A dab with stable topology advances the GEOMETRY revision
    and leaves the TOPOLOGY revision alone, so a host re-uploads the vertex
    buffer and keeps the index buffer it already has.
    """
    return sum(view.chunk_info(i)["vertex_count"] * 3 * 4 for i in chunks)


def canonical_triangles(positions, indices):
    """Every triangle as the nine floats a host would upload, in a form two
    paths that number their vertices differently can still be compared by.

    Rotated so the smallest corner is first — a ROTATION and not a sort, which
    would lose the winding and call a surface whose triangles are all inside out
    equal to one that is not.
    """
    tris = np.asarray(positions, dtype=np.float32)[
        np.asarray(indices, dtype=np.int64).reshape(-1, 3)]
    keys = tris.view(np.uint32).reshape(len(tris), 3, 3)
    # Which corner is lexicographically smallest, per triangle.
    order = np.lexsort((keys[:, :, 2], keys[:, :, 1], keys[:, :, 0]), axis=1)[:, 0]
    rolled = np.stack([tris[np.arange(len(tris)), (order + k) % 3] for k in range(3)], axis=1)
    flat = rolled.reshape(len(tris), 9)
    return flat[np.lexsort(flat.T[::-1])]


def reconstruct(view):
    """The whole surface, assembled the way a host assembles it: chunk by chunk,
    each with its own local indices, none of them knowing about the others."""
    tris = []
    for i in range(view.chunk_count):
        if not view.chunk_info(i)["live"]:
            continue
        got = view.copy_chunk(i, normals=False)
        tris.append(canonical_triangles(got["positions"], got["indices"]))
    stacked = np.concatenate(tris, axis=0)
    return stacked[np.lexsort(stacked.T[::-1])]


def main():
    print(__doc__)

    small, large = plane(SMALL), plane(LARGE)
    small_view = clay.SurfaceView.over_mesh(small, target_faces=TARGET_FACES)
    large_view = clay.SurfaceView.over_mesh(large, target_faces=TARGET_FACES)
    # `Mesh.indices` comes back shaped (triangles, 3), so its LENGTH is the
    # triangle count. Dividing it by three again is the mistake this line was
    # written to avoid, and it under-reported both models by 3x.
    small_tris = np.asarray(small.indices).reshape(-1, 3).shape[0]
    large_tris = np.asarray(large.indices).reshape(-1, 3).shape[0]
    model_ratio = large_tris / small_tris

    print(f"\n  TWO MODELS AT THE SAME DETAIL, {SPACING} apart on both:")
    print(f"    small  {len(np.asarray(small.positions)):8,d} vertices  "
          f"{small_tris:8,d} triangles  {small_view.chunk_count:5d} chunks")
    print(f"    large  {len(np.asarray(large.positions)):8,d} vertices  "
          f"{large_tris:8,d} triangles  {large_view.chunk_count:5d} chunks")
    print(f"    the large model is {model_ratio:.0f}x the small one.")
    if model_ratio < 8.0:
        raise SystemExit("the two models are too close in size to say anything")

    # --- 1. the same dab, on both ---------------------------------------------
    centre = (0.0, 0.0, 0.0)
    small_hit = chunks_reached(small_view, centre, BRUSH_RADIUS)
    large_hit = chunks_reached(large_view, centre, BRUSH_RADIUS)
    small_bytes = upload_bytes(small_view, small_hit)
    large_bytes = upload_bytes(large_view, large_hit)

    small_sculptor = clay.MeshSculptor(small)
    large_sculptor = clay.MeshSculptor(large)
    small_moved = small_sculptor.stamp("draw", center=centre, radius=BRUSH_RADIUS, strength=0.5)
    large_moved = large_sculptor.stamp("draw", center=centre, radius=BRUSH_RADIUS, strength=0.5)

    full_small = len(np.asarray(small.positions)) * 3 * 4
    full_large = len(np.asarray(large.positions)) * 3 * 4
    print("\n  THE SAME DAB, ON BOTH:")
    print(f"    small   {small_moved:6d} vertices moved  {len(small_hit):4d} chunks  "
          f"{small_bytes / 1024:8.1f} KiB to the host   (whole surface {full_small / 1024:9.1f} KiB)")
    print(f"    large   {large_moved:6d} vertices moved  {len(large_hit):4d} chunks  "
          f"{large_bytes / 1024:8.1f} KiB to the host   (whole surface {full_large / 1024:9.1f} KiB)")

    if large_moved != small_moved:
        raise SystemExit(
            f"the same brush moved {small_moved} vertices on one model and {large_moved} "
            "on the other: the footprint is not being held constant")
    # THE CLAIM. A chunk is a fixed face count and the partition is a median
    # split over the whole mesh, so the same ball straddles a chunk boundary
    # differently at the two sizes — by one or two chunks, never by the model
    # ratio.
    if large_bytes > 2 * small_bytes:
        raise SystemExit(
            f"a dab hands the host {large_bytes} bytes on the large model against "
            f"{small_bytes} on the small one: the preview is following the MODEL")
    saving = full_large / large_bytes
    print(f"    the dab on the large model costs {large_bytes / small_bytes:.2f}x what it costs")
    print(f"    on the small one, against a model {model_ratio:.0f}x the size — and it is")
    print(f"    {saving:.0f}x cheaper than handing the host the whole surface.")
    if saving < 10.0:
        raise SystemExit("the per-chunk transport is not saving anything worth having")

    # --- 2. the stream IS the surface ------------------------------------------
    whole = canonical_triangles(np.asarray(large.positions), np.asarray(large.indices))
    streamed = reconstruct(large_view)
    print("\n  THE STREAM IS THE SURFACE:")
    print(f"    {large_view.chunk_count} chunks reassembled into {len(streamed):,d} triangles,")
    print(f"    against {len(whole):,d} from the whole-surface path.")
    if streamed.shape != whole.shape or not np.array_equal(streamed, whole):
        raise SystemExit(
            "the surface reassembled from the chunks is not the surface the "
            "whole-surface path produces — a host drawing incrementally would be "
            "drawing something the engine does not think it made")
    print("    every triangle agrees, by the bits of its nine floats.")

    # --- 3. what it costs, and what a host may take back ------------------------
    hierarchy = clay.MultiresSurface.from_mesh(plane(24, spacing=0.08))
    hierarchy.add_level()
    hierarchy.add_level()
    hierarchy.mesh_at_level(2)
    ledger = hierarchy.memory_ledger()
    print("\n  WHAT IT COSTS, in the three figures a memory warning actually asks for:")
    print(f"    essential    {ledger['essential'] / 1024:9.1f} KiB   the work: cage, topology, "
          "the coefficients")
    print(f"    rebuildable  {ledger['rebuildable'] / 1024:9.1f} KiB   caches, chunk indices, "
          "evaluated levels")
    print(f"    undoable     {ledger['undoable'] / 1024:9.1f} KiB   history, to the host's own "
          "policy")
    if ledger["total"] != ledger["essential"] + ledger["rebuildable"] + ledger["undoable"]:
        raise SystemExit("the three roll-ups do not add up to the total")

    checksum = hierarchy.detail_checksum
    with clay.MemoryPin() as pin:
        # A SAVE IS RUNNING. Whatever the operating system says next, the
        # document under the writer does not move — and the host still gets an
        # answer rather than a zero it cannot interpret.
        would = hierarchy.trim(clay.Pressure.critical, pin)
        if not would["pinned"] or would["total_released"] == 0:
            raise SystemExit("a pinned trim must report what it WOULD have released")
        if hierarchy.memory_ledger()["rebuildable"] != ledger["rebuildable"]:
            raise SystemExit("a pinned trim released something")
    released = hierarchy.trim(clay.Pressure.critical)
    print(f"\n    under a pin, a critical trim reports {would['total_released'] / 1024:.1f} KiB "
          "and releases nothing.")
    print(f"    once the save is done it releases {released['total_released'] / 1024:.1f} KiB "
          "for real —")
    if released["multires_detail"] or released["base_geometry"] or released["topology"]:
        raise SystemExit("a trim released authoritative content")
    if hierarchy.detail_checksum != checksum:
        raise SystemExit("a trim changed the authoritative checksum: it took the WORK")
    rebuilt = np.asarray(hierarchy.mesh_at_level(2).positions)
    if len(rebuilt) == 0:
        raise SystemExit("a dropped cache did not come back")
    print("    and the authoritative checksum is unchanged, which is what says it")
    print("    released caches and not work. Every one of them reconstructs.")

    # --- 4. priced before it is paid --------------------------------------------
    priced = large.preflight_to_dynamic()
    refused = large.preflight_to_dynamic(budget=priced["persistent_bytes"])
    print("\n  PRICED BEFORE IT IS PAID:")
    print(f"    converting the large model to an adaptive surface KEEPS "
          f"{priced['persistent_bytes'] / 1048576:.1f} MiB")
    print(f"    and PEAKS at {priced['peak_bytes'] / 1048576:.1f} MiB, because the source mesh, "
          "the half-edge")
    print("    structure and the weld map are all live at once.")
    if priced["peak_bytes"] <= priced["persistent_bytes"]:
        raise SystemExit("a preflight whose peak equals its result has nothing to say")
    if refused["allowed"]:
        raise SystemExit("a budget under the PEAK was allowed: the estimate is reading the "
                         "steady state, which is not what kills an application")
    print(f"    a budget of {refused['peak_bytes'] / 1048576:.1f} MiB minus the transient is "
          "refused BEFORE anything")
    print(f"    is allocated: \"{refused['error']}\".")
    absurd = large.preflight_global_remesh(2 ** 64 - 1)
    if absurd["allowed"]:
        raise SystemExit("an estimate that overflowed 64 bits was ALLOWED — which is what a "
                         "wrapped multiply produces, because a wrapped estimate is a small one")
    print(f"    and an absurd target refuses rather than wrapping: \"{absurd['error']}\".")

    # --- the pictures -------------------------------------------------------------
    tiles, labels = [], []
    for name, mesh, view, hit in (("small", small, small_view, small_hit),
                                  ("large", large, large_view, large_hit)):
        eye, target = R.orbit_camera(
            (np.asarray(mesh.positions).min(axis=0), np.asarray(mesh.positions).max(axis=0)),
            elevation=52.0)
        lit = np.full((len(np.asarray(mesh.positions)), 3), 0.34, dtype=np.float64)
        lit[:, 0] *= 1.06
        # The touched region, by the same ball the query used, so the picture is
        # of what the runtime actually reached rather than of a drawn circle.
        d = np.linalg.norm(np.asarray(mesh.positions) - np.array(centre), axis=1)
        lit[d <= BRUSH_RADIUS] = (1.0, 0.60, 0.20)
        tiles.append(R.render_mesh_array(mesh, eye=eye, target=target, colors=lit))
        labels.append(f"{name}: {np.asarray(mesh.indices).reshape(-1, 3).shape[0]:,d} "
                      f"triangles, {len(hit)} chunks touched")

    R.contact_sheet(tiles, "69_extreme_poly.png", columns=2,
                    caption=" | ".join(labels))

    path = R.output_path("69_extreme_poly_chunk.obj")
    got = large_view.copy_chunk(large_hit[0], normals=False)
    clay.Mesh.from_triangles(got["positions"],
                             got["indices"].reshape(-1, 3)).save(path)
    print(f"\n  wrote {os.path.basename(path)}: ONE chunk, as its own standalone draw with")
    print("  indices local to itself. That file is what a host uploads per dab; the")
    print("  model it came from is on disk beside it and is not.")


if __name__ == "__main__":
    main()

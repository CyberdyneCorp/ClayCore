# The surface tier through pyclay (c-abi and sculpt-runtime specs,
# add-extreme-poly-runtime).
#
# The principle the whole change serves is that a dab costs approximately what
# it TOUCHES rather than what the model HOLDS, and these are the three things a
# Python host needs for that to be true across the boundary: what changed, those
# bytes and no others, and what the whole thing costs.
#
# TWO CASES HERE ARE THE ONES THAT MATTER, and both are about a host that falls
# behind. A chunk acknowledged against the revisions it was actually copied at
# retires; a chunk that changed AGAIN in between does NOT, and stays waiting. A
# green suite where every acknowledgement succeeds would prove nothing about the
# case the acknowledgement exists for.

import math

import numpy as np
import pytest

import pyclay as clay


def sphere(n, radius=1.0):
    """A cube-sphere as flat arrays, so the fixture depends on no file."""
    axes = [(0, 1, 2), (0, 1, 2), (1, 2, 0), (1, 2, 0), (2, 0, 1), (2, 0, 1)]
    signs = [1.0, -1.0, 1.0, -1.0, 1.0, -1.0]
    positions, indices = [], []
    for f in range(6):
        base = len(positions) // 3
        for v in range(n + 1):
            for u in range(n + 1):
                c = [0.0, 0.0, 0.0]
                c[axes[f][0]] = -1.0 + 2.0 * u / n
                c[axes[f][1]] = -1.0 + 2.0 * v / n
                c[axes[f][2]] = signs[f]
                length = math.sqrt(sum(x * x for x in c))
                positions += [x / length * radius for x in c]
        stride = n + 1
        for v in range(n):
            for u in range(n):
                a = base + v * stride + u
                b, c2, d = a + 1, a + stride, a + stride + 1
                indices += [a, c2, b, b, c2, d] if signs[f] > 0 else [a, b, c2, b, d, c2]
    return (np.array(positions, dtype=np.float32).reshape(-1, 3),
            np.array(indices, dtype=np.uint32).reshape(-1, 3))


def grid(n=4, half=1.0):
    step = 2.0 * half / n
    positions = np.array(
        [[-half + step * x, 0.0, -half + step * z] for z in range(n + 1) for x in range(n + 1)],
        dtype=np.float32)
    stride = n + 1
    faces = []
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            faces += [[a, a + 1, a + stride + 1], [a, a + stride + 1, a + stride]]
    return positions, np.array(faces, dtype=np.uint32)


def test_fixed_mesh_partitions_every_triangle_exactly_once():
    positions, indices = sphere(20)
    mesh = clay.Mesh.from_triangles(positions, indices)
    view = clay.SurfaceView.over_mesh(mesh, target_faces=64)

    assert view.kind == clay.SurfaceKind.fixed
    assert view.chunk_count > 1

    # A partition that DROPPED a triangle and one that SHARED it between two
    # chunks both draw plausibly; only the count says which happened.
    triangles = sum(view.chunk_info(i)["index_count"] // 3
                    for i in range(view.chunk_count)
                    if view.chunk_info(i)["live"])
    assert triangles == len(indices)

    got = view.copy_chunk(0)
    assert got["positions"].shape[1] == 3
    assert got["normals"].shape == got["positions"].shape
    # WELDED: the chunk's own vertices with indices local to it, so it uploads
    # as a standalone draw and never indexes past its own array.
    assert got["indices"].max() < got["positions"].shape[0]
    assert got["positions"].shape[0] < got["indices"].shape[0]
    assert got["stale"] is False

    # A fixed mesh's view carries a partition and no dirty set: the fixed
    # sculptor still tracks dirty WELD CLASSES, which the chunk table has not
    # taken over.
    assert view.dirty_chunks == []

    without_normals = view.copy_chunk(0, normals=False)
    assert "normals" not in without_normals


def test_a_dab_dirties_a_fraction_of_the_surface_and_drains_losslessly():
    positions, indices = sphere(24)
    surface = clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    sculptor = clay.DynamicSculptor(surface)
    view = clay.SurfaceView.over_dynamic(sculptor)
    assert view.kind == clay.SurfaceKind.adaptive
    view.clear_dirty()

    sculptor.stamp("draw", (0, 0, 1), 0.15, 0.3)
    dirty = view.dirty_chunks
    # THE PREVIEW PROPERTY, at the smallest size a unit test can afford: the dab
    # reached a fraction of the surface, so the set it reports is a fraction of
    # the chunks. A transport that reported everything would pass every other
    # assertion in this file.
    assert 0 < len(dirty) < view.chunk_count

    got = view.copy_chunk(dirty[0])
    # UNWELDED, because an adaptive surface's topology changes under the very
    # stamp being uploaded: three vertices per triangle.
    assert got["positions"].shape[0] == got["indices"].shape[0]

    assert view.acknowledge(dirty[0], got["revisions"]) is True
    assert len(view.dirty_chunks) == len(dirty) - 1

    # AND THE CASE THE MECHANISM EXISTS FOR. Stamp again over the same region,
    # then acknowledge with the revisions from BEFORE that stamp: the chunk
    # changed after the host read it, so it stays dirty and the change the host
    # never saw is not lost.
    sculptor.stamp("draw", (0, 0, 1), 0.15, 0.3)
    again = view.dirty_chunks
    current = view.copy_chunk(again[0])["revisions"]
    behind = dict(current)
    behind["geometry"] = max(0, behind["geometry"] - 1)

    assert view.acknowledge(again[0], behind) is False
    assert len(view.dirty_chunks) == len(again)

    # ...and the readback says so, rather than leaving a host to draw something
    # the engine does not think it made.
    stale = view.copy_chunk(again[0], expected=behind)
    assert stale["stale"] is True
    assert stale["requested"]["geometry"] == behind["geometry"]
    assert stale["revisions"]["geometry"] != behind["geometry"]
    assert view.copy_chunk(again[0], expected=stale["revisions"])["stale"] is False

    view.clear_dirty()
    assert view.dirty_chunks == []


def test_a_hierarchy_level_reports_the_same_shape():
    positions, faces = grid(4)
    hierarchy = clay.MultiresSurface.from_mesh(clay.Mesh.from_triangles(positions, faces))
    hierarchy.add_level()
    hierarchy.add_level()

    view = clay.SurfaceView.over_level(hierarchy, 2)
    assert view.kind == clay.SurfaceKind.multires
    assert view.chunk_count > 0

    triangles = sum(view.chunk_info(i)["index_count"] // 3 for i in range(view.chunk_count))
    level_faces = hierarchy.level_counts(2)["faces"]
    # Quads, triangulated into two: the chunks cover the level's faces exactly
    # once, the same claim the fixed case makes about its triangles.
    assert triangles == level_faces * 2

    view.clear_dirty()
    sculptor = clay.MultiresSculptor(hierarchy)
    sculptor.stamp("draw", (0.0, 0.0, 0.0), 0.5, 0.2)
    dirty = view.dirty_chunks
    assert len(dirty) > 0
    got = view.copy_chunk(dirty[0])
    assert got["indices"].max() < got["positions"].shape[0]
    assert view.acknowledge(dirty[0], got["revisions"]) is True
    assert len(view.dirty_chunks) == len(dirty) - 1

    with pytest.raises(ValueError):
        clay.SurfaceView.over_level(hierarchy, 9)


def test_the_ledger_partitions_the_total_and_a_trim_keeps_the_work():
    positions, faces = grid(6)
    hierarchy = clay.MultiresSurface.from_mesh(clay.Mesh.from_triangles(positions, faces))
    hierarchy.add_level()
    hierarchy.add_level()
    hierarchy.mesh_at_level(2)  # force the caches to exist before pricing them

    profile = clay.SculptMemoryProfile()
    assert profile.memory_class == clay.MemoryClass.full
    assert profile.cache_budget == 0
    assert profile.allow_index_rebuild is True
    profile.memory_class = clay.MemoryClass.constrained
    profile.max_resident_levels = 2
    hierarchy.memory_profile = profile
    assert hierarchy.memory_profile.memory_class == clay.MemoryClass.constrained
    assert hierarchy.memory_profile.max_resident_levels == 2

    ledger = hierarchy.memory_ledger()
    assert ledger["total"] == ledger["essential"] + ledger["rebuildable"] + ledger["undoable"]
    assert ledger["essential"] > 0 and ledger["rebuildable"] > 0
    # Authoritative detail is NEVER counted as rebuildable: a host acting on
    # that distinction would delete the user's work.
    assert ledger["multires_detail"] <= ledger["essential"]

    before = hierarchy.detail_checksum

    # THE PIN: a memory warning arriving mid-save gets an honest answer instead
    # of a document mutating under the writer.
    with clay.MemoryPin() as pin:
        assert pin.held is True
        pinned = hierarchy.trim(clay.Pressure.critical, pin)
        assert pinned["pinned"] is True
        assert hierarchy.memory_ledger()["rebuildable"] == ledger["rebuildable"]
    assert pin.held is False

    report = hierarchy.trim(clay.Pressure.critical)
    assert report["pinned"] is False
    assert report["total_released"] > 0
    assert report["pressure"] == "critical"
    # Nothing essential went, at the hardest pressure there is...
    assert report["multires_detail"] == 0
    assert report["base_geometry"] == 0
    assert report["topology"] == 0
    # ...which the checksum is what proves. A trim that released the user's work
    # would still report a large and plausible number of bytes freed.
    assert hierarchy.detail_checksum == before
    # ...and the dropped caches reconstruct.
    assert len(hierarchy.mesh_at_level(2).positions) > 0


def test_a_document_report_folds_in_the_surfaces_the_host_holds_beside_it():
    doc = clay.Document()
    bare = doc.memory
    # A hierarchy is opaque and OWNING and lives beside a document, so a
    # document that holds none reports zero for them rather than guessing.
    assert bare["surface_content"] == 0
    assert bare["multires_detail"] == 0
    assert bare["total"] == bare["essential"] + bare["rebuildable"] + bare["undoable"]

    positions, faces = grid(4)
    hierarchy = clay.MultiresSurface.from_mesh(clay.Mesh.from_triangles(positions, faces))
    hierarchy.add_level()
    ledger = hierarchy.memory_ledger()

    with_surface = doc.memory_with_surfaces(ledger)
    assert with_surface["total"] > bare["total"]
    assert with_surface["multires_detail"] == ledger["multires_detail"]
    assert (with_surface["total"] ==
            with_surface["essential"] + with_surface["rebuildable"] + with_surface["undoable"])
    # A list of ledgers is the shape a host holding several surfaces has.
    assert doc.memory_with_surfaces([ledger, ledger])["total"] > with_surface["total"]
    assert doc.memory_with_surfaces(None)["total"] == bare["total"]


def test_an_operation_is_priced_before_it_is_paid_and_refuses_whole():
    positions, indices = sphere(8)
    mesh = clay.Mesh.from_triangles(positions, indices)

    priced = mesh.preflight_to_dynamic()
    assert priced["allowed"] is True
    assert priced["persistent_bytes"] == (priced["authoritative_bytes"] +
                                          priced["runtime_bytes"])
    # THE PEAK IS THE NUMBER THAT MATTERS: the conversion holds the source mesh,
    # the half-edge structure and the weld map at once, and an operation priced
    # by its result is the one that terminates the process half way through.
    assert priced["peak_bytes"] >= priced["persistent_bytes"]

    refused = mesh.preflight_to_dynamic(budget=1024)
    assert refused["allowed"] is False
    assert refused["error"] == "predicted peak exceeds the declared budget"
    # A refusal still reports the figures, so a host can say how far over it is.
    assert refused["peak_bytes"] == priced["peak_bytes"]

    # An estimate that overflows 64 bits refuses at ANY budget, including none:
    # a wrapped estimate is a SMALL one, and a small one is allowed.
    overflowed = mesh.preflight_global_remesh(2 ** 64 - 1)
    assert overflowed["allowed"] is False
    assert overflowed["error"] == "the capacity estimate overflowed 64 bits"

    surface = clay.DynamicSurface.from_mesh(mesh)
    assert surface.preflight_to_mesh()["allowed"] is True
    blob = surface.preflight_encode()
    assert blob["peak_bytes"] > blob["persistent_bytes"]

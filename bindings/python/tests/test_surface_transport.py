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


def _plane(n, spacing=0.02):
    """A patch at FIXED SPACING whose extent grows with `n`, measured outward
    from the centre.

    THE FIXTURE IS THE EXPERIMENT for the two scaling cases below: a bigger
    model here is more of the same geometry at the same detail, never a more
    finely subdivided version of it — that would put more vertices under a brush
    of the same radius and the comparison would be measuring the fixture.
    Centred rather than cornered because `-half + spacing * x` rounds
    differently at two sizes, so the same world point comes out a few ulps apart
    and the ball admits a different set of vertices for a reason that has
    nothing to do with the runtime.
    """
    centre = n // 2
    xs = (np.arange(n + 1) - centre) * spacing
    gx, gz = np.meshgrid(xs, xs, indexing="xy")
    y = np.where((np.add.outer(np.arange(n + 1), np.arange(n + 1)) % 2) == 0, 0.0, spacing * 0.5)
    positions = np.stack([gx, y, gz], axis=-1).reshape(-1, 3).astype(np.float32)
    stride = n + 1
    a = (np.arange(n)[:, None] * stride + np.arange(n)[None, :]).reshape(-1)
    b, c, d = a + 1, a + stride, a + stride + 1
    faces = np.stack([np.stack([a, c, b], axis=1), np.stack([b, c, d], axis=1)],
                     axis=1).reshape(-1, 3).astype(np.uint32)
    return clay.Mesh.from_triangles(positions, faces)


def _canonical(positions, indices):
    """Triangles as the nine floats a host uploads, rotated so the smallest
    corner is first — a rotation and not a sort, which would lose the winding
    and call an inside-out surface equal to one that is not."""
    tris = np.asarray(positions, dtype=np.float32)[
        np.asarray(indices, dtype=np.int64).reshape(-1, 3)]
    keys = tris.view(np.uint32).reshape(len(tris), 3, 3)
    order = np.lexsort((keys[:, :, 2], keys[:, :, 1], keys[:, :, 0]), axis=1)[:, 0]
    rolled = np.stack([tris[np.arange(len(tris)), (order + k) % 3] for k in range(3)], axis=1)
    flat = rolled.reshape(len(tris), 9)
    return flat[np.lexsort(flat.T[::-1])]


def test_the_stream_reassembles_into_the_whole_surface_path():
    # The whole-surface path stays in the library for correctness and the
    # incremental one is what a host at twenty million vertices uses. Nothing in
    # the pixels says when they stop agreeing: a host drawing from the stream
    # draws something the engine does not think it made, and it looks like
    # geometry.
    mesh = _plane(24)
    view = clay.SurfaceView.over_mesh(mesh, target_faces=64)

    streamed = []
    for i in range(view.chunk_count):
        if not view.chunk_info(i)["live"]:
            continue
        got = view.copy_chunk(i, normals=False)
        # Local indices, so a chunk uploads as its own standalone draw and can
        # never index past its own array.
        assert got["indices"].max() < got["positions"].shape[0]
        streamed.append(_canonical(got["positions"], got["indices"]))
    streamed = np.concatenate(streamed, axis=0)
    streamed = streamed[np.lexsort(streamed.T[::-1])]

    whole = _canonical(np.asarray(mesh.positions), np.asarray(mesh.indices))
    assert streamed.shape == whole.shape
    assert np.array_equal(streamed, whole)


def test_a_dab_hands_the_host_the_same_bytes_at_sixteen_times_the_model():
    # THE PREVIEW GATE. Bytes handed to a host per stamp follow the dirty
    # chunks, not the model size.
    radius, centre = 0.20, np.zeros(3)

    def dab(n):
        mesh = _plane(n)
        view = clay.SurfaceView.over_mesh(mesh, target_faces=128)
        lo, hi = centre - radius, centre + radius
        reached = [i for i in range(view.chunk_count)
                   if view.chunk_info(i)["live"]
                   and np.all(np.array(view.chunk_info(i)["bounds_max"]) >= lo)
                   and np.all(np.array(view.chunk_info(i)["bounds_min"]) <= hi)]
        upload = sum(view.chunk_info(i)["vertex_count"] * 3 * 4 for i in reached)
        sculptor = clay.MeshSculptor(mesh)
        moved = sculptor.stamp("draw", center=tuple(centre), radius=radius, strength=0.4)
        whole = len(np.asarray(mesh.positions)) * 3 * 4
        return moved, len(reached), upload, whole

    small_moved, small_chunks, small_bytes, small_whole = dab(48)
    large_moved, large_chunks, large_bytes, large_whole = dab(192)

    assert small_moved > 0
    # The footprint is held constant BY CONSTRUCTION, and this is the assertion
    # that says the fixture did its job. Without it every line below could pass
    # on a model that simply had less surface under the brush.
    assert large_moved == small_moved
    assert large_whole > 14 * small_whole

    # A chunk is a fixed face count and the partition is a median split over the
    # whole mesh, so the same ball straddles a chunk boundary differently at the
    # two sizes — by one or two chunks, never by the model ratio.
    assert large_chunks <= 2 * small_chunks
    assert 0 < large_bytes <= 2 * small_bytes
    # And the point of it: the dab is cheaper than the surface by the model.
    assert large_bytes * 20 < large_whole


def test_a_constrained_profile_cannot_change_what_is_committed():
    # THE 1.3 GATE, and the reason every field of the profile is a HINT. Each
    # one names something recomputable exactly from what was committed; there is
    # deliberately no field that reaches the result, because a deferred SPLIT
    # would make the mesh a function of machine speed. This is the assertion
    # that would fail if such a field were ever added.
    positions, faces = grid(6)

    def stroke_under(profile):
        hierarchy = clay.MultiresSurface.from_mesh(clay.Mesh.from_triangles(positions, faces))
        for _ in range(3):
            hierarchy.add_level()
        if profile is not None:
            hierarchy.memory_profile = profile
        hierarchy.sculpt_level = hierarchy.max_level
        hierarchy.display_level = hierarchy.max_level
        sculptor = clay.MultiresSculptor(hierarchy)
        sculptor.begin_stroke()
        for i in range(10):
            sculptor.stamp("draw", (-0.3 + 0.06 * i, 0.0, 0.1 * (i % 3)), 0.3, 0.3)
        return (hierarchy.detail_checksum,
                np.array(hierarchy.positions_at(hierarchy.max_level), copy=True))

    constrained = clay.SculptMemoryProfile()
    constrained.memory_class = clay.MemoryClass.constrained
    constrained.max_resident_levels = 1
    constrained.cache_budget = 64 * 1024
    constrained.scratch_budget = 32 * 1024
    constrained.preview_chunks_per_frame = 2
    constrained.defer_normals_in_stroke = True
    constrained.allow_index_rebuild = False

    minimal = clay.SculptMemoryProfile()
    minimal.memory_class = clay.MemoryClass.minimal
    minimal.max_resident_levels = 1

    full_sum, full_positions = stroke_under(None)
    for profile in (constrained, minimal):
        checksum, got = stroke_under(profile)
        # The coefficients, which are what the document holds...
        assert checksum == full_sum
        # ...and the surface they reconstruct, bit for bit. The constrained run
        # defers its normals, so this is also the assertion that a deferred
        # flush ends exactly where a per-stamp one does.
        assert np.array_equal(got, full_positions)


def test_a_trim_between_two_dabs_does_not_eat_the_next_dab():
    """A memory warning arriving mid-stroke used to cost the next dab.

    THE DEFECT, and the reason it is worth a Python case of its own: a script
    that reacts to a memory warning by calling `trim` between two dabs of a
    stroke is the ordinary way to use this API from Python, and the failure was
    silent from up here. `stamp` returned the number of weld classes it had
    moved and the surface did not change, because the sculptor was still bound
    to a level mesh the trim had released — so the displacement went into freed
    storage and the next evaluation rebuilt the level from the authoritative
    detail, which had never been written.

    The assertion is therefore `detail_checksum` after EVERY dab, which is what
    "the dab landed" means for a hierarchy. Checking the return of `stamp` was
    how the defect stayed hidden, and checking the surface once at the end
    would have passed: with the defect present the odd dabs land and the even
    ones do not.
    """
    positions, faces = grid(6)
    hierarchy = clay.MultiresSurface.from_mesh(clay.Mesh.from_triangles(positions, faces))
    hierarchy.add_level()
    hierarchy.add_level()

    sculptor = clay.MultiresSculptor(hierarchy)
    sculptor.begin_stroke()
    previous = hierarchy.detail_checksum
    for i in range(4):
        moved = sculptor.stamp("draw", (-0.3 + 0.2 * i, 0.0, 0.0), 0.35, 0.5)
        assert moved > 0, f"dab {i} reached nothing"
        now = hierarchy.detail_checksum
        assert now != previous, f"dab {i} reported {moved} moved and changed nothing"
        previous = now

        report = hierarchy.trim(clay.Pressure.critical)
        assert report["pinned"] is False
        # ...and the release itself still changes nothing, which is the older
        # claim this one sits beside rather than replaces.
        assert hierarchy.detail_checksum == previous


def test_a_stroke_taken_under_memory_pressure_commits_the_same_surface():
    """Determinism, which is the form the same defect takes for a reproduction.

    Two machines running the same session must not diverge because one of them
    was low on memory. Compared as the authoritative checksum and then position
    by position, because a stroke that landed a DIFFERENT surface under
    pressure is a bug report nobody can reproduce.
    """
    def stroke(trim_between):
        positions, faces = grid(6)
        hierarchy = clay.MultiresSurface.from_mesh(
            clay.Mesh.from_triangles(positions, faces))
        hierarchy.add_level()
        hierarchy.add_level()
        sculptor = clay.MultiresSculptor(hierarchy)
        sculptor.begin_stroke()
        moved = []
        for i in range(4):
            moved.append(sculptor.stamp("draw", (-0.3 + 0.2 * i, 0.0, 0.0), 0.35, 0.5))
            if trim_between:
                hierarchy.trim(clay.Pressure.critical)
        return hierarchy, moved

    calm, calm_moved = stroke(False)
    pressed, pressed_moved = stroke(True)

    assert pressed_moved == calm_moved
    assert pressed.detail_checksum == calm.detail_checksum
    # Bit equality rather than a tolerance: "the same surface" is the claim, and
    # a tolerance would pass a stroke that had quietly landed somewhere else.
    for level in range(3):
        want = calm.mesh_at_level(level).positions
        got = pressed.mesh_at_level(level).positions
        assert np.array_equal(got, want), f"level {level} diverged under pressure"

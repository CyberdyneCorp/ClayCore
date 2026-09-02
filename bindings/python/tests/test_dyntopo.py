# Dynamic topology through pyclay (dynamic-topology spec, add-dynamic-topology).
#
# THE BINDING WAS THERE AND THE PARITY TEST WAS NOT. `DynamicSurface`,
# `DynamicSculptor`, `TopologySettings` and `DetailMode` have all been bound
# since the feature landed, and Python touched exactly one half of them: every
# adaptive case in the suite passes `topology_off()`, because it is there to
# compare the shared brush kernels against the fixed representation and a
# remesh mid-comparison would be measuring the remesher. So the deformation
# path was covered from Python and the ADAPTATION path — the thing the
# representation exists for — was covered only from C++.
#
# What that leaves untested across the boundary, and what is here:
#
#   * a stamp with topology ENABLED, which is the only call that reaches
#     split/collapse/flip at all;
#   * determinism of an adaptive stroke, which for this representation includes
#     the connectivity and not merely the positions;
#   * `DynamicSurface.serialize`/`deserialize`, which no Python test called;
#   * `validate()`, likewise;
#   * the dirty-chunk transport over a surface whose TOPOLOGY moved under it —
#     test_surface_transport.py drains an adaptive surface, but only one a
#     deformation-only dab dirtied, so nothing there has ever asked whether the
#     stream still reassembles after the chunk table was resized by a split.
#
# PARITY, here, is the stamp's own report against the surface it changed: the
# result dict is what a host schedules its next frame from, and a host that
# believed a `topology_revision` the surface does not agree with would skip an
# index-buffer upload it needed. Same standard as test_c_abi_parity.py — do the
# work, then check the two readings of it agree.
#
# UNDO/REDO IS DELIBERATELY ABSENT. `revert`/`apply` over a TopologyDelta wedge
# the next stamp on a known open defect; parity for them belongs with that fix
# and is deferred to it, not forgotten.

import math

import numpy as np
import pytest

import pyclay as clay


def sphere(n, radius=1.0):
    """A cube-sphere as flat arrays, so the fixture depends on no file.

    The same construction test_surface_transport.py uses, and for the same
    reason: it welds into a closed manifold with no boundary edges, which is
    what `DynamicSurface.from_mesh` will accept.
    """
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


def surface(n=6, radius=1.0):
    positions, indices = sphere(n, radius)
    return clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))


def topology_on(detail_resolution=6.0):
    """Brush-relative detail, the mode the C++ cases use: the target edge
    length follows the brush, so one set of numbers refines at any radius."""
    t = clay.TopologySettings()
    t.enabled = True
    t.detail_mode = clay.DetailMode.BRUSH_RELATIVE
    t.detail_resolution = detail_resolution
    return t


def topology_off():
    t = clay.TopologySettings()
    t.enabled = False
    return t


def canonical(mesh):
    """Triangles as the nine floats a host uploads, each rotated so its
    smallest corner leads.

    A rotation and NOT a sort: sorting the corners would lose the winding and
    call an inside-out surface equal to one that is not. Rows are then ordered
    so two descriptions of the same surface compare equal whatever order they
    arrived in — which is the whole point when one of them came off a chunk
    stream and the other off `to_mesh`.
    """
    positions = np.asarray(mesh.positions, dtype=np.float32)
    tris = positions[np.asarray(mesh.indices, dtype=np.int64).reshape(-1, 3)]
    return canonical_triangles(tris)


def canonical_triangles(tris):
    tris = np.asarray(tris, dtype=np.float32).reshape(-1, 3, 3)
    keys = tris.view(np.uint32).reshape(len(tris), 3, 3)
    order = np.lexsort((keys[:, :, 2], keys[:, :, 1], keys[:, :, 0]), axis=1)[:, 0]
    rolled = np.stack([tris[np.arange(len(tris)), (order + k) % 3] for k in range(3)], axis=1)
    flat = rolled.reshape(len(tris), 9)
    return flat[np.lexsort(flat.T[::-1])]


def stroke(sculptor, topology, steps=6, radius=0.4, strength=0.35):
    """The C++ determinism stroke, driven from Python: a short drag across the
    +Z pole, every stamp remeshing under itself."""
    result = []
    for i in range(steps):
        centre = (-0.3 + 0.12 * i, 0.0, 0.95)
        result.append(sculptor.stamp("draw", centre, radius, strength, topology=topology))
    return result


# -- the surface itself ------------------------------------------------------

def test_a_mesh_becomes_a_surface_that_reports_itself():
    positions, indices = sphere(6)
    mesh = clay.Mesh.from_triangles(positions, indices)
    s = clay.DynamicSurface.from_mesh(mesh)

    # The half-edge structure over a closed manifold: every triangle kept,
    # every edge shared by two faces, so nothing is on a boundary. Euler holds
    # for a sphere, V - E + F == 2, which is a stronger statement than any
    # count on its own — a build that dropped a face would still report three
    # self-consistent numbers.
    assert s.face_count == mesh.triangle_count
    # The cube-sphere's six patches share their seams, so the build WELDS: the
    # surface holds fewer vertices than the flat mesh listed.
    assert 0 < s.vertex_count < len(positions)
    assert s.boundary_edge_count == 0
    assert s.dead_slots == 0
    assert s.vertex_count - s.edge_count + s.face_count == 2
    assert s.bytes > 0

    report = s.validate()
    assert report["ok"] is True, report["summary"]

    # A fresh surface exports the surface it was built from, triangle for
    # triangle — not merely the same count.
    assert np.array_equal(canonical(s.to_mesh()), canonical(mesh))


def test_from_mesh_refuses_rather_than_repairs():
    # Two faces is fine; a third on the same edge is not expressible as a
    # half-edge pair, and the binding says so rather than dropping it.
    positions = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1], [0, -1, 0]],
                         dtype=np.float32)
    indices = np.array([[0, 1, 2], [0, 1, 3], [0, 1, 4]], dtype=np.uint32)
    with pytest.raises(ValueError, match="three or more faces on one edge"):
        clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))


# -- the stamp that adapts ---------------------------------------------------

def test_a_topology_stamp_creates_geometry_and_says_that_it_did():
    s = surface(6)
    sculptor = clay.DynamicSculptor(s)
    before_v, before_f = s.vertex_count, s.face_count

    r = sculptor.stamp("draw", (0, 0, 1), 0.35, 0.5, topology=topology_on())

    # The result object reports what it did, and the surface agrees that it was
    # done. THIS IS THE PARITY CLAIM: a host schedules its next frame off the
    # returned dict and never re-reads the surface, so the two readings must be
    # the same reading.
    assert r["moved"] > 0
    assert r["split"] > 0
    assert r["hit_budget"] is False
    assert r["topology_revision"] == s.topology_revision
    assert r["geometry_revision"] == s.geometry_revision
    assert r["topology_revision"] > 1 and r["geometry_revision"] > 1

    assert s.vertex_count > before_v
    assert s.face_count > before_f
    # A split makes one triangle into two and one edge into two-plus-the-new
    # spoke, so faces grow at least as fast as vertices. A remesher that
    # reported splits it did not perform would not move these together.
    assert s.face_count - before_f >= s.vertex_count - before_v

    report = s.validate()
    assert report["ok"] is True, report["summary"]
    assert s.vertex_count - s.edge_count + s.face_count == 2


def test_the_new_geometry_is_where_the_brush_is():
    # A remesher that refined globally would be a different, much more
    # expensive operation wearing this one's name, and every count-based
    # assertion above would still pass.
    s = surface(6)
    centre = np.array([0.0, 0.0, 1.0], dtype=np.float32)
    radius = 0.35

    def shell_counts(mesh):
        p = np.asarray(mesh.positions, dtype=np.float32)
        d = np.linalg.norm(p - centre, axis=1)
        return np.array([(d < radius * k).sum() for k in (1.5, 2.0)] + [len(p)], dtype=np.int64)

    before = shell_counts(s.to_mesh())
    clay.DynamicSculptor(s).stamp("draw", tuple(centre), radius, 0.5, topology=topology_on())
    grew = shell_counts(s.to_mesh()) - before

    assert grew[-1] > 0, "the stamp added no geometry at all"
    # BEYOND TWICE THE RADIUS, NOTHING IS NEW. The bound is 2r rather than the
    # brush's own radius because splitting an edge that straddles the falloff
    # puts its midpoint just outside — a one-ring skirt is the remesher being
    # local, not global.
    assert grew[1] == grew[-1]
    # And the bulk of it lands under the brush rather than in that skirt.
    assert grew[0] > 0.8 * grew[-1]


def test_topology_off_is_the_control_and_changes_no_connectivity():
    # The contrast that makes the case above mean something: the same verb, the
    # same place, the same strength, with adaptation the only difference.
    s = surface(6)
    before_v, before_f, before_topology = s.vertex_count, s.face_count, s.topology_revision

    r = clay.DynamicSculptor(s).stamp("draw", (0, 0, 1), 0.35, 0.5, topology=topology_off())

    assert r["moved"] > 0
    assert r["split"] == 0 and r["collapsed"] == 0 and r["flipped"] == 0
    assert s.vertex_count == before_v and s.face_count == before_f
    assert s.topology_revision == before_topology
    assert r["geometry_revision"] == s.geometry_revision


def test_an_adaptive_stroke_is_deterministic_bit_for_bit():
    # For this representation determinism includes the CONNECTIVITY, and every
    # later claim about undo, parity and reproducibility rests on it.
    a, b = surface(5), surface(5)
    ra = stroke(clay.DynamicSculptor(a), topology_on(5.0))
    rb = stroke(clay.DynamicSculptor(b), topology_on(5.0))

    assert sum(x["split"] for x in ra) > 0, "the stroke never remeshed; the fixture is wrong"
    assert ra == rb

    assert (a.vertex_count, a.edge_count, a.face_count) == \
           (b.vertex_count, b.edge_count, b.face_count)

    ma, mb = a.to_mesh(), b.to_mesh()
    # Not the canonical form here: the same run twice must give the same
    # ARRAYS, in the same order, to the bit — a canonicalisation would forgive
    # a renumbering that a host's index buffer would not.
    assert np.array_equal(np.asarray(ma.positions), np.asarray(mb.positions))
    assert np.array_equal(np.asarray(ma.indices), np.asarray(mb.indices))
    assert a.serialize() == b.serialize()


# -- the transport, over a surface whose topology moved ----------------------

def test_the_dirty_set_names_a_fraction_and_the_stream_rebuilds_the_surface():
    s = surface(12)
    sculptor = clay.DynamicSculptor(s)
    view = clay.SurfaceView.over_dynamic(sculptor)
    view.clear_dirty()
    assert view.dirty_chunks == []

    r = sculptor.stamp("draw", (0, 0, 1), 0.3, 0.4, topology=topology_on())
    assert r["split"] > 0, "a stamp that did not remesh does not exercise this path"

    dirty = view.dirty_chunks
    # A stamp reached a fraction of the surface, so it names a fraction of the
    # chunks. A transport that marked everything would pass every other
    # assertion here.
    assert 0 < len(dirty) < view.chunk_count
    assert len(set(dirty)) == len(dirty)
    assert sculptor.chunk_count == view.chunk_count
    assert sorted(sculptor.dirty_chunks) == sorted(dirty)

    # THE STREAM IS THE SURFACE. Every live chunk copied and concatenated
    # reassembles, triangle for triangle, into what `to_mesh` reports — after a
    # stamp that resized the chunk table under it.
    streamed = []
    for chunk in range(view.chunk_count):
        info = view.chunk_info(chunk)
        if not info["live"]:
            continue
        got = view.copy_chunk(chunk)
        positions, indices = got["positions"], got["indices"]
        assert got["stale"] is False
        assert positions.shape[0] == info["vertex_count"]
        assert indices.shape[0] == info["index_count"]
        # UNWELDED: an adaptive surface's topology changes under the very stamp
        # being uploaded, so a chunk copies as loose triangles.
        assert positions.shape[0] == indices.shape[0]
        if indices.shape[0]:
            streamed.append(positions[np.asarray(indices, dtype=np.int64)])

    assert streamed, "no chunk carried a triangle"
    assert np.array_equal(canonical_triangles(np.concatenate(streamed)), canonical(s.to_mesh()))

    # `clear_dirty` is the all-or-nothing drain, and it drains.
    view.clear_dirty()
    assert view.dirty_chunks == []
    assert sculptor.dirty_chunks == []

    # ...and the next stamp fills it again, so the clear reset the set rather
    # than disabling the tracking.
    sculptor.stamp("draw", (0.2, 0, 0.95), 0.3, 0.4, topology=topology_on())
    assert 0 < len(view.dirty_chunks) < view.chunk_count


def test_the_sculptors_own_dirty_list_clears_from_either_seam():
    # `DynamicSculptor.clear_dirty` and `SurfaceView.clear_dirty` reach the same
    # chunk table by two different routes; a host holding one must not see a set
    # the other already drained.
    s = surface(8)
    sculptor = clay.DynamicSculptor(s)
    view = clay.SurfaceView.over_dynamic(sculptor)

    sculptor.stamp("draw", (0, 0, 1), 0.3, 0.4, topology=topology_on())
    assert sculptor.dirty_chunks
    sculptor.clear_dirty()
    assert sculptor.dirty_chunks == []
    assert view.dirty_chunks == []


def test_the_peak_telemetry_bounds_what_the_stamps_actually_did():
    # The high-water marks are what sizes a host's staging buffer and slot
    # pools, so they must bound the run that produced them rather than track it
    # loosely.
    s = surface(8)
    sculptor = clay.DynamicSculptor(s)
    view = clay.SurfaceView.over_dynamic(sculptor)
    sculptor.reset_peak_telemetry()

    results = stroke(sculptor, topology_on(), steps=4, radius=0.35)
    ops = max(r["split"] + r["collapsed"] + r["flipped"] for r in results)

    peak = sculptor.peak_telemetry
    assert peak["topology_ops"] >= ops
    assert peak["dirty_chunks"] >= len(view.dirty_chunks)
    assert peak["workset_vertices"] >= max(r["moved"] for r in results)


# -- maintenance and persistence ---------------------------------------------

def test_rebuild_index_leaves_the_surface_and_the_index_usable():
    s = surface(10)
    sculptor = clay.DynamicSculptor(s)
    stroke(sculptor, topology_on(), steps=4, radius=0.35)

    before = canonical(s.to_mesh())
    revisions = (s.topology_revision, s.geometry_revision)

    sculptor.rebuild_index()

    # A rebuild is a repartition of the same surface: it changes how the
    # triangles are grouped and not which triangles there are, so nothing the
    # revisions describe has changed.
    assert (s.topology_revision, s.geometry_revision) == revisions
    assert np.array_equal(canonical(s.to_mesh()), before)
    assert s.validate()["ok"] is True

    quality = sculptor.index_quality
    assert quality["leaf_count"] > 0
    assert quality["quality"] > 0.0  # a sphere has volume, so this is a real figure
    assert sculptor.chunk_count == quality["leaf_count"]

    # And the index still serves a stamp afterwards, which is the only reason
    # to rebuild it.
    r = sculptor.stamp("draw", (0, 0, 1), 0.3, 0.4, topology=topology_on())
    assert r["moved"] > 0
    assert s.validate()["ok"] is True


def test_a_remeshed_surface_round_trips_through_serialize():
    s = surface(6)
    stroke(clay.DynamicSculptor(s), topology_on(), steps=3, radius=0.35)

    blob = s.serialize()
    assert isinstance(blob, bytes) and len(blob) > 0

    back = clay.DynamicSurface.deserialize(blob)
    assert (back.vertex_count, back.edge_count, back.face_count) == \
           (s.vertex_count, s.edge_count, s.face_count)
    assert back.validate()["ok"] is True

    ms, mb = s.to_mesh(), back.to_mesh()
    assert np.array_equal(np.asarray(ms.positions), np.asarray(mb.positions))
    assert np.array_equal(np.asarray(ms.indices), np.asarray(mb.indices))
    # Round-tripping again is a fixed point. THIS IS THE STRONG CLAIM, and the
    # reason it is worth making after a REMESHED stroke rather than on a fresh
    # surface: the blob carries every live element as (slot, generation), so a
    # decode that renumbered the slots a split allocated, or reset the
    # generation counters the free list uses, would come back as an equal
    # surface and an unequal blob.
    assert back.serialize() == blob

    # THE REVISIONS DO NOT TRAVEL, and a host must not expect them to. They are
    # the live instance's generation counters, not part of the format — the
    # blob preserves per-slot generations and nothing about how many stamps
    # produced them — so a reloaded surface counts from the start and
    # everything in it is new to whoever holds it.
    assert s.topology_revision > 1 and s.geometry_revision > 1
    assert back.topology_revision == 1
    assert back.geometry_revision == 1

    # ...and a decoded surface is a live one, not a read-only snapshot.
    r = clay.DynamicSculptor(back).stamp("draw", (0, 0, 1), 0.3, 0.4, topology=topology_on())
    assert r["moved"] > 0
    assert back.validate()["ok"] is True


def test_deserialize_refuses_a_truncated_blob():
    s = surface(5)
    blob = s.serialize()
    with pytest.raises(ValueError, match="not a dynamic surface this build can read"):
        clay.DynamicSurface.deserialize(blob[:-16])


def test_preflight_prices_the_export_before_it_is_paid():
    s = surface(6)
    stroke(clay.DynamicSculptor(s), topology_on(), steps=3, radius=0.35)

    priced = s.preflight_to_mesh()
    assert priced["allowed"] is True
    assert priced["peak_bytes"] > 0

    # The export splits a geometric vertex per distinct corner, so the estimate
    # must cover what to_mesh actually produced.
    exported = s.to_mesh()
    assert priced["peak_bytes"] >= len(np.asarray(exported.positions)) * 3 * 4

    refused = s.preflight_to_mesh(budget=1024)
    assert refused["allowed"] is False
    assert refused["peak_bytes"] == priced["peak_bytes"]

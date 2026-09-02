# Work that is not required for correctness, through pyclay (sculpt-runtime
# spec, add-extreme-poly-runtime tasks 3.3, 3.4 and 3.5).
#
# THREE THINGS A PYTHON HOST COULD NOT REACH BEFORE THIS. The queue that holds
# the jobs a runtime accumulates, the measurement behind the one job a host is
# most likely to decline, and the normal deferral that makes a drag driven
# stamp-by-stamp cost what a whole stroke costs. All three existed in C++ and
# nothing outside C++ could touch them, which is the same as not having shipped
# them.
#
# WHAT THESE CASES GATE THAT THE C++ AND C ONES DO NOT.
#
# `with queue.stroke():` IS THE POINT OF THE PYTHON BINDING. A stroke loop is
# exactly the code that raises — a bad radius, a mask of the wrong length, a
# KeyboardInterrupt on a long drag — and a gate left shut by an exception is a
# queue that silently never runs again for the rest of the session. `with` is
# the only form that cannot do that, and the case below raises INSIDE the block
# on purpose.
#
# AND THE DEFERRAL'S ONE CONTRACT, in numpy: two identical stroke sequences,
# one deferred and flushed and one not, compared array against array. That the
# flush ran is not the claim; that the arrays are equal afterwards is.

import numpy as np
import pytest

import pyclay as clay


def grid(n=16, half=1.0):
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


def meshed_sphere(voxel_size=0.04):
    # A MESH THAT CAME OUT OF THE LIBRARY CARRYING NORMALS, which a triangle
    # soup does not: `Mesh.from_triangles` carries positions and indices and
    # nothing else, and a sculptor never manufactures normals for a mesh that
    # has none — twelve bytes a vertex is a real cost to add behind a brush
    # stroke. So a deferral is unobservable on a bare soup.
    doc = clay.Document()
    body = doc.add_sdf_layer("body")
    body.add(clay.Sphere(r=0.5))
    return doc.mesh(voxel_size=voxel_size)


def test_a_request_folds_into_the_identical_one_already_queued():
    queue = clay.MaintenanceQueue()
    assert len(queue) == 0

    queue.request(clay.MaintenanceKind.index_rebuild, target=7)
    queue.request(clay.MaintenanceKind.index_rebuild, target=7, estimated_micros=250)
    # A DIFFERENT TARGET IS A DIFFERENT JOB. Folding on the kind alone would
    # collapse "rebuild level 7" and "rebuild level 9" into one item and leave
    # the other silently undone.
    queue.request(clay.MaintenanceKind.index_rebuild, target=9)

    assert len(queue) == 2
    assert queue.count == 2
    first = queue.items[0]
    assert first["kind"] == clay.MaintenanceKind.index_rebuild
    assert first["target"] == 7
    # The second request bumped the count rather than adding an entry, which is
    # what makes it safe to ask from inside a stamp.
    assert first["requests"] == 2
    # The latest non-zero estimate wins: a caller that has learned more about
    # the cost is telling the host something.
    assert first["estimated_micros"] == 250

    assert queue.has(clay.MaintenanceKind.index_rebuild, 9)
    assert not queue.has(clay.MaintenanceKind.normal_flush, 9)
    assert queue.bytes > 0


def test_the_kind_an_item_reports_is_the_kind_complete_takes():
    # A dict of raw integers would make a drain loop cast by hand, and a script
    # that got the enumeration's order wrong would complete the wrong job.
    queue = clay.MaintenanceQueue()
    queue.request(clay.MaintenanceKind.detail_promotion, target=3)
    item = queue.take_next()
    assert isinstance(item["kind"], clay.MaintenanceKind)
    assert queue.complete(item["kind"], item["target"])
    assert len(queue) == 0


def test_the_stroke_gate_stops_the_drain_and_not_the_request():
    queue = clay.MaintenanceQueue()
    queue.begin_stroke()
    assert queue.in_stroke

    # A stamp is where an item is DISCOVERED. Recording it mid-stroke is the
    # whole point; what the gate stops is running it.
    queue.request(clay.MaintenanceKind.chunk_compaction, target=3)
    assert len(queue) == 1

    # THE MECHANISM, not a convention: a script that wired the drain to a
    # pointer handler gets nothing done rather than a stutter it will blame on
    # the brush.
    assert queue.take_next() is None
    assert not queue.complete(clay.MaintenanceKind.chunk_compaction, 3)
    assert len(queue) == 1

    queue.end_stroke()
    assert not queue.in_stroke
    item = queue.take_next()
    assert item is not None
    assert item["kind"] == clay.MaintenanceKind.chunk_compaction


def test_the_stroke_block_reopens_the_gate_when_the_body_raises():
    # THE CASE THE CONTEXT MANAGER EXISTS FOR. A drag loop that raises would
    # otherwise leave the gate shut for the rest of the session, and every later
    # drain would report nothing to do — which is indistinguishable from an
    # empty queue.
    queue = clay.MaintenanceQueue()
    with pytest.raises(ValueError):
        with queue.stroke():
            assert queue.in_stroke
            queue.request(clay.MaintenanceKind.slot_pool_compaction, target=1)
            raise ValueError("the stroke loop blew up")

    assert not queue.in_stroke
    # The request survived; only the running of it was held.
    assert queue.take_next()["kind"] == clay.MaintenanceKind.slot_pool_compaction


def test_take_peeks_and_complete_removes():
    queue = clay.MaintenanceQueue()
    queue.request(clay.MaintenanceKind.normal_flush, target=1)
    queue.request(clay.MaintenanceKind.detail_promotion, target=2)

    first = queue.take_next()
    # A HOST THAT DECLINED HAS NOT DROPPED IT: taking twice without completing
    # returns the same item, which is what makes "I cannot afford this one right
    # now" expressible at all.
    assert queue.take_next() == first
    assert len(queue) == 2

    assert queue.complete(first["kind"], first["target"])
    assert len(queue) == 1
    # Completing one that is not there reports False rather than raising: a
    # script that serviced the same item twice has done no harm.
    assert not queue.complete(first["kind"], first["target"])

    # Queue order is the order it was asked for.
    assert queue.take_next()["kind"] == clay.MaintenanceKind.detail_promotion
    queue.clear()
    assert queue.take_next() is None


def test_a_full_drain_loop_is_four_lines():
    # The shape the binding is FOR, written the way a host writes it: a budget
    # the script owns, one item at a time, and work that is an ordinary call.
    positions, indices = grid(24)
    surface = clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    sculptor = clay.DynamicSculptor(surface)
    queue = clay.MaintenanceQueue()

    with queue.stroke():
        sculptor.stamp("draw", center=(0.0, 0.0, 0.0), radius=0.5, strength=0.6)
        sculptor.request_index_rebuild(queue, target=0)
        queue.request(clay.MaintenanceKind.normal_flush, target=0)

    serviced = []
    while True:
        item = queue.take_next()
        if item is None:
            break
        if item["kind"] == clay.MaintenanceKind.index_rebuild:
            sculptor.rebuild_index()
        serviced.append(item["kind"])
        queue.complete(item["kind"], item["target"])

    assert clay.MaintenanceKind.normal_flush in serviced
    assert len(queue) == 0


def test_the_engine_measures_the_index_and_the_host_decides():
    positions, indices = grid(24)
    surface = clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    sculptor = clay.DynamicSculptor(surface)

    quality = sculptor.index_quality
    assert quality["leaf_count"] > 0
    # A PROPERTY OF THE MEASURE, asserted rather than left to be discovered on a
    # flat model: `quality` is a volume ratio, so a surface with no volume
    # reports 0 and never wants a rebuild however far its partition drifts.
    assert quality["quality"] == 0.0
    assert quality["wants_rebuild"] is False

    sculptor.stamp("draw", center=(0.0, 0.0, 0.0), radius=0.5, strength=0.6)
    # Same tree, same faces: what changed is that the leaves now enclose a
    # volume, so the number means something again.
    assert sculptor.index_quality["quality"] > 0.0

    queue = clay.MaintenanceQueue()
    profile = clay.SculptMemoryProfile()
    # THE HOST'S HALF, ON ITS OWN. This is the case that would pass by accident
    # if the two conditions had been folded into one.
    profile.allow_index_rebuild = False
    assert sculptor.request_index_rebuild(queue, profile=profile, target=0) is False
    assert len(queue) == 0

    profile.allow_index_rebuild = True
    wants = sculptor.index_quality["wants_rebuild"]
    assert sculptor.request_index_rebuild(queue, profile=profile, target=0) is wants
    # No profile at all is the default profile, which is what a desktop script
    # that has never filled one passes.
    assert sculptor.request_index_rebuild(queue, target=0) is wants


def test_deferring_normals_changes_when_the_work_happens_and_nothing_else():
    stroke = [(-0.15, 0.48, 0.0), (0.0, 0.50, 0.0), (0.15, 0.48, 0.0)]

    eager_mesh = meshed_sphere()
    lazy_mesh = meshed_sphere()
    eager = clay.MeshSculptor(eager_mesh)
    lazy = clay.MeshSculptor(lazy_mesh)

    # Off by default: a moved vertex with a stale normal shades wrong
    # immediately, so this is opt-in rather than the arrangement everyone gets.
    assert lazy.defer_normals is False
    lazy.defer_normals = True
    assert lazy.defer_normals is True

    for x, y, z in stroke:
        assert eager.stamp("draw", center=(x, y, z), radius=0.25, strength=0.4) > 0
        assert lazy.stamp("draw", center=(x, y, z), radius=0.25, strength=0.4) > 0

    # THE GEOMETRY IS ALREADY IDENTICAL. Asserting it first is what makes the
    # normal difference below attributable to the deferral rather than to two
    # strokes that diverged.
    np.testing.assert_allclose(eager_mesh.positions, lazy_mesh.positions, atol=1e-6)
    # And they DO differ before the flush, so the comparison after it is a claim
    # about the flush rather than about two runs that were never apart.
    assert not np.allclose(eager_mesh.normals, lazy_mesh.normals, atol=1e-6)

    lazy.flush_normals()
    np.testing.assert_allclose(eager_mesh.normals, lazy_mesh.normals, atol=1e-6)

    # Flushing again is a no-op rather than a second pass over stale state,
    # which is what makes "call it at every stroke end" safe advice.
    lazy.flush_normals()
    np.testing.assert_allclose(eager_mesh.normals, lazy_mesh.normals, atol=1e-6)


def test_a_deferred_strokes_undo_is_exact_only_with_the_record():
    # The argument that is easy to miss. A deferred stroke's undo is exact only
    # if the FLUSH records the normals it changed into the same delta record the
    # stamps used; omitting it reverts positions and leaves shading from a
    # stroke that no longer exists.
    mesh = meshed_sphere()
    sculptor = clay.MeshSculptor(mesh)
    before = np.array(mesh.normals, copy=True)

    deltas = clay.VertexDeltas()
    sculptor.defer_normals = True
    assert sculptor.stamp("draw", center=(0.0, 0.5, 0.0), radius=0.25, strength=0.5,
                          deltas=deltas) > 0
    sculptor.flush_normals(deltas)
    deltas.revert(sculptor)

    np.testing.assert_allclose(mesh.normals, before, atol=1e-6)

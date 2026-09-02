# The seed token and the peak telemetry through pyclay (sculpt-runtime spec,
# add-extreme-poly-runtime tasks 3.2 and 7.7).
#
# TWO THINGS A PYTHON HOST CANNOT GET ANY OTHER WAY.
#
# The token, because pyclay's `raycast` is the caller most exposed to the
# defect it exists for: it hands back a `seed_class` a script is meant to feed
# straight into the next `stamp`, and a class picked in a numbering that has
# since been replaced is still IN BOUNDS. The walk then finds nothing within the
# radius and the stamp reports 0 moved — the same answer a fully masked stroke
# gives, so nothing in the script can tell the two apart.
#
# The peaks, because the number that catches an O(model) buffer is a high-water
# mark rather than an allocation count: a `bytearray` sized to the vertex count
# and reused forever allocates nothing per stamp and still costs what the model
# costs.

import numpy as np

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


def sculptor_over_a_grid(n=16, half=1.0):
    positions, indices = grid(n, half)
    mesh = clay.Mesh.from_triangles(positions, indices)
    # The mesh is returned too: a sculptor holds it by reference and the
    # fixture's copy is what keeps it alive for the test.
    return clay.MeshSculptor(mesh), mesh


def test_a_pick_hands_back_the_numbering_its_seed_was_taken_from():
    sculptor, _mesh = sculptor_over_a_grid()
    hit = sculptor.raycast((0, 1, 0), (0, -1, 0))
    assert hit is not None
    # TOGETHER, in one dict. A script that had to fetch the token separately is
    # a script that can forget to, and the failure is silent.
    assert hit["seed_revision"] == sculptor.seed_revision
    assert sculptor.seed_revision != 0

    moved = sculptor.stamp("draw", hit["position"], 0.4, 0.4,
                           seed_class=hit["seed_class"], seed_revision=hit["seed_revision"])
    assert moved > 0
    assert sculptor.stale_seeds_rejected == 0


def test_a_seed_from_another_sculptors_numbering_is_refused_rather_than_spent():
    # Two sculptors over two identical grids: the second's class space is
    # genuinely a different numbering, which is what a hierarchy produces on
    # every rebind. Each takes the same dab from an untouched grid, so the two
    # counts are comparable — a stamp changes the surface the next one reaches
    # across.
    picker, _first = sculptor_over_a_grid()
    stale, _second = sculptor_over_a_grid()
    honest, _third = sculptor_over_a_grid()

    hit = picker.raycast((0, 1, 0), (0, -1, 0))
    assert hit is not None
    assert hit["seed_revision"] != honest.seed_revision

    stale_moved = stale.stamp("draw", (0, 0, 0), 0.5, 0.5,
                              seed_class=hit["seed_class"], seed_revision=hit["seed_revision"])
    assert stale_moved > 0  # the dab still lands, through the scan it fell back to
    assert stale.stale_seeds_rejected == 1

    honest_moved = honest.stamp("draw", (0, 0, 0), 0.5, 0.5,
                                seed_class=hit["seed_class"],
                                seed_revision=honest.seed_revision)
    # THE CLAIM: refusing the seed changed what the stamp COST, not what it did.
    assert honest_moved == stale_moved
    assert honest.stale_seeds_rejected == 0


def test_a_hierarchy_renumbers_behind_the_host_and_the_token_says_so():
    positions, indices = grid(4, 2.0)
    surface = clay.MultiresSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    surface.add_level()
    surface.add_level()
    sculptor = clay.MultiresSculptor(surface)

    surface.sculpt_level = 1
    coarse = sculptor.seed_revision
    assert coarse != 0
    # Picked off the COARSE level, away from where the stamp lands: all that
    # matters is that it is farther from the centre than the radius, which is
    # when the surface walk gives up and returns nothing.
    level_pick = clay.MeshSculptor(surface.mesh_at_level(1))
    corner = level_pick.raycast((-1.5, 1, -1.5), (0, -1, 0))
    assert corner is not None

    surface.sculpt_level = 2
    fine = sculptor.seed_revision
    assert fine != coarse  # the rebind renumbered, and the token is how you know

    # WITHOUT the token — what every caller written before it sends. The seed is
    # trusted, the walk finds nothing within the radius of a place it should
    # never have started from, and the dab is LOST rather than misplaced.
    assert sculptor.stamp("draw", (0, 0, 0), 0.6, 0.5, seed_class=corner["seed_class"]) == 0
    # WITH it, the stale numbering is visible, the seed is refused, and the walk
    # finds its own way.
    assert sculptor.stamp("draw", (0, 0, 0), 0.6, 0.5,
                          seed_class=corner["seed_class"],
                          seed_revision=coarse) > 0


def test_the_peaks_are_high_water_marks_a_script_can_read_and_restart():
    sculptor, _mesh = sculptor_over_a_grid(24)
    assert sculptor.peak_telemetry["workset_vertices"] == 0

    sculptor.stamp("draw", (0, 0, 0), 0.8, 0.5)
    widest = sculptor.peak_telemetry["workset_vertices"]
    assert widest > 0
    # The fixed stamp path does not consume the scratch arena yet, and the
    # binding reports the 0 it measured rather than a number nothing filled.
    assert sculptor.peak_telemetry["scratch_bytes"] == 0

    # A HIGH-WATER MARK rather than the last value. Placed in a corner the wide
    # stamp did not reach: a second dab at the same centre would find the
    # surface displaced out from under it and gather nothing.
    assert sculptor.stamp("draw", (-0.9, 0, -0.9), 0.15, 0.5) > 0
    assert sculptor.peak_telemetry["workset_vertices"] == widest

    sculptor.reset_peak_telemetry()
    assert sculptor.peak_telemetry["workset_vertices"] == 0
    assert sculptor.stamp("draw", (0.9, 0, 0.9), 0.15, 0.5) > 0
    narrow = sculptor.peak_telemetry["workset_vertices"]
    assert 0 < narrow < widest


def test_the_adaptive_surface_reports_the_peak_only_it_has():
    positions, indices = grid(24)
    surface = clay.DynamicSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    sculptor = clay.DynamicSculptor(surface)
    assert sculptor.peak_telemetry["topology_ops"] == 0

    topology = clay.TopologySettings()
    topology.detail_mode = clay.DetailMode.BRUSH_RELATIVE
    topology.detail_resolution = 6.0
    sculptor.stamp("draw", (0, 0, 0), 0.4, 0.5, topology=topology)

    peak = sculptor.peak_telemetry
    # THE NUMBER NO OTHER REPRESENTATION HAS: a split allocates a slot, so the
    # stamp that split the most is the one a slot pool has to survive.
    assert peak["topology_ops"] > 0
    assert peak["workset_vertices"] > 0
    # The dirty set is the same one the transport drains, so this is what a
    # staging buffer must hold.
    assert peak["dirty_chunks"] > 0

    sculptor.reset_peak_telemetry()
    assert sculptor.peak_telemetry["topology_ops"] == 0


def test_a_hierarchys_peak_belongs_to_the_session_and_not_to_one_level():
    positions, indices = grid(4, 2.0)
    surface = clay.MultiresSurface.from_mesh(clay.Mesh.from_triangles(positions, indices))
    surface.add_level()
    surface.add_level()
    sculptor = clay.MultiresSculptor(surface)

    surface.sculpt_level = 2
    assert sculptor.stamp("draw", (0, 0, 0), 1.2, 0.5) > 0
    fine = sculptor.peak_telemetry["workset_vertices"]
    assert fine > 0

    # A LEVEL CHANGE REBINDS, building a new level sculptor underneath. The peak
    # must not restart with it: a host sizes one arena for a stroke, and a
    # stroke may cross levels.
    surface.sculpt_level = 1
    sculptor.stamp("draw", (0, 0, 0), 0.5, 0.5)
    assert sculptor.peak_telemetry["workset_vertices"] == fine


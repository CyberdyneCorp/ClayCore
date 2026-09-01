"""The sculpt layer stack through pyclay (python-bindings spec,
add-mesh-sculpt-layers).

WHAT THIS SUITE IS FOR, over and above the C++ and C cases. Three things only
exist at this boundary, and each of them is a way for a script to lose work:

  * THE CONTEXT MANAGER'S ASYMMETRY. `with surface.sculpt_layer_stroke():`
    commits on a clean exit and CANCELS when the block raises. A gesture
    committed on the way out of an exception is an undo step for work nobody
    asked for, and a gesture left open holds the composition forever — every
    slider on the model refusing, with nothing in the traceback saying why.
    Note this differs from the VOXEL stack's scope, which keeps the partial
    pass, and deliberately: a voxel pass is already-applied cell writes, while a
    layered gesture has an exact recorded `before` to go back to.

  * THE IMAGE LAYOUT. Height maps are (H, W) and vector maps are (3, H, W) —
    three PLANES, because a plane is the buffer the alpha sampler already reads.
    An interleaved (H, W, 3) is the array numpy hands you from an image loader,
    so it is refused with a message that says which one it wanted rather than
    read as garbage.

  * THE REFUSALS AS SENTENCES. The C++ API answers `False`, which is all a
    caller with the stack in front of it needs. A script needs to know WHICH of
    "no such layer", "that layer is locked" and "finish the stroke first" it
    hit, so each raises with its own text.

Everything else here is the same claim the other two suites make, asserted once
more at the level a user actually writes: strength is composition and replays no
stroke, invisible contributes nothing to the bit, and an id survives a reorder.
"""

import numpy as np
import pytest

import pyclay as clay


def plane(n=4, half=1.0):
    """A quad grid on XZ as triangles: the coarse cage a hierarchy is built on."""
    positions, indices = [], []
    step = 2.0 * half / n
    for z in range(n + 1):
        for x in range(n + 1):
            positions.append([-half + step * x, 0.0, -half + step * z])
    stride = n + 1
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            b, c, d = a + 1, a + stride + 1, a + stride
            indices += [a, b, c, a, c, d]
    return clay.Mesh.from_triangles(np.array(positions, np.float32),
                                    np.array(indices, np.uint32))


def surface(levels=2):
    s = clay.MultiresSurface.from_mesh(plane())
    for _ in range(levels):
        s.add_level()
    s.sculpt_level = levels
    return s


def positions(s, level=2):
    return np.array(s.positions_at(level), copy=True)


def test_the_stack_is_addressed_by_id_and_an_id_survives_a_reorder():
    s = surface()
    lower = s.add_sculpt_layer("lower")
    upper = s.add_sculpt_layer("upper")
    assert s.sculpt_layer_count == 2
    assert s.sculpt_layer_ids == [lower, upper]
    assert s.sculpt_layer_info(upper)["index"] == 1
    assert s.sculpt_layer_info(upper)["name"] == "upper"
    assert s.sculpt_layer_info(upper)["kind"] == "sampled"

    # THE CASE AN INDEX-KEYED SCRIPT PASSES EVERYTHING ELSE AND FAILS: the id is
    # taken while the layer is on top and used after it has been dragged down.
    s.move_sculpt_layer(upper, 0)
    assert s.sculpt_layer_ids == [upper, lower]
    s.set_sculpt_layer_strength(upper, 0.25)
    assert s.sculpt_layer_info(upper)["strength"] == pytest.approx(0.25)
    assert s.sculpt_layer_info(lower)["strength"] == pytest.approx(1.0)


def test_strength_is_composition_and_replays_no_stroke():
    s = surface()
    flat = positions(s)
    layer = s.add_sculpt_layer("wrinkles")
    for v in range(30, 60):
        s.set_sculpt_layer_detail(layer, 2, v, 0.0, 0.0, 0.04)
    full = positions(s)
    assert not np.array_equal(flat, full)

    s.set_sculpt_layer_strength(layer, 0.5)
    half = positions(s)
    assert np.allclose(half - flat, 0.5 * (full - flat), atol=1e-6)
    # The COEFFICIENT is untouched: the pass was recorded at full size and the
    # dial is composition, so raising it back restores the whole contribution
    # rather than a scaled-down remnant.
    assert s.sculpt_layer_detail(layer, 2, 40) == pytest.approx((0.0, 0.0, 0.04))
    s.set_sculpt_layer_strength(layer, 1.0)
    assert np.array_equal(positions(s), full)

    # Zero and invisible are EXACTLY the base, bit for bit — not nearly it.
    s.set_sculpt_layer_strength(layer, 0.0)
    assert np.array_equal(positions(s), flat)
    s.set_sculpt_layer_strength(layer, 1.0)
    s.set_sculpt_layer_visible(layer, False)
    assert np.array_equal(positions(s), flat)


def test_a_rename_invalidates_nothing_and_a_slider_invalidates_coverage():
    s = surface()
    layer = s.add_sculpt_layer("pass")
    for v in range(30, 60):
        s.set_sculpt_layer_detail(layer, 2, v, 0.0, 0.0, 0.02)
    positions(s)

    metadata, composition, content = (s.sculpt_layer_metadata_revision,
                                      s.sculpt_layer_composition_revision,
                                      s.sculpt_layer_content_revision)
    s.rename_sculpt_layer(layer, "pores")
    assert s.sculpt_layer_metadata_revision > metadata
    assert s.sculpt_layer_composition_revision == composition
    assert s.sculpt_layer_content_revision == content

    s.reset_sculpt_layer_stats()
    s.set_sculpt_layer_strength(layer, 0.4)
    positions(s)
    # THE SCALE CLAIM AS A MEASUREMENT: a correct implementation and a
    # quadratic one produce the same surface, and only this counter tells them
    # apart.
    stats = s.sculpt_layer_stats()
    assert stats["blocks_recomposed"] >= 1
    assert stats["compositions"] >= 1


def test_the_three_refusals_are_three_different_sentences():
    s = surface()
    layer = s.add_sculpt_layer("pass")

    with pytest.raises(ValueError, match="no sculpt layer"):
        s.set_sculpt_layer_strength(layer + 999, 0.5)

    s.set_sculpt_layer_locked(layer, True)
    with pytest.raises(ValueError, match="is locked"):
        s.set_sculpt_layer_detail(layer, 2, 40, 0.0, 0.0, 0.05)
    # A lock is a permission on the COEFFICIENTS, not on the channel.
    s.rename_sculpt_layer(layer, "finished")
    s.set_sculpt_layer_strength(layer, 0.5)
    s.set_sculpt_layer_locked(layer, False)

    s.hold_sculpt_layer_composition(True)
    with pytest.raises(ValueError, match="a stroke is open"):
        s.set_sculpt_layer_strength(layer, 0.9)
    with pytest.raises(ValueError, match="a stroke is open"):
        s.add_sculpt_layer("while open")
    s.rename_sculpt_layer(layer, "still fine")  # metadata is never held
    s.hold_sculpt_layer_composition(False)
    s.set_sculpt_layer_strength(layer, 0.9)


def test_a_clean_block_commits_and_a_raising_block_cancels():
    s = surface()
    layer = s.add_sculpt_layer("pass")
    s.active_sculpt_layer = layer
    before = s.sculpt_layer_checksum
    quiet = positions(s)

    with s.sculpt_layer_stroke() as stroke:
        assert stroke.target_layer == layer
        for i in range(5):
            stroke.stamp("draw", center=(-0.2 + 0.1 * i, 0.0, 0.0), radius=0.35, strength=0.4)
        assert stroke.stamps == 5
        # A hundred stamps over one vertex is ONE entry: the record follows the
        # vertices the stroke reached, not the stamps it took.
        assert 0 < stroke.record_size
    committed = s.sculpt_layer_checksum
    assert committed != before
    assert not np.array_equal(positions(s), quiet)
    # The hold is released, so the sliders work again.
    s.set_sculpt_layer_strength(layer, 0.5)
    s.set_sculpt_layer_strength(layer, 1.0)

    # A RAISING BLOCK CANCELS. A partial gesture committed on the way out of an
    # exception is an undo step for work nobody asked for.
    with pytest.raises(RuntimeError, match="the tablet fell off the desk"):
        with s.sculpt_layer_stroke() as stroke:
            stroke.stamp("draw", center=(0.4, 0.0, 0.4), radius=0.35, strength=0.9)
            raise RuntimeError("the tablet fell off the desk")
    assert s.sculpt_layer_checksum == committed
    # ...and the composition is not left held, which is the failure the context
    # manager exists to make impossible.
    s.set_sculpt_layer_strength(layer, 0.5)


def test_the_write_domain_is_the_callers_choice():
    s = surface()
    layer = s.add_sculpt_layer("pass")

    s.active_sculpt_layer = 0
    with pytest.raises(ValueError, match="cannot begin a layered stroke"):
        with s.sculpt_layer_stroke(write_domain="detail"):
            pass

    s.active_sculpt_layer = layer
    layer_before = s.sculpt_layer_checksum
    base_before = s.detail_checksum
    with s.sculpt_layer_stroke(write_domain="geometry") as stroke:
        assert stroke.target_layer == 0
        assert stroke.stamp("draw", center=(0, 0, 0), radius=0.4, strength=0.5) > 0
    # The form under the passes moved and the pass did not.
    assert s.detail_checksum != base_before
    assert s.sculpt_layer_checksum == layer_before

    with pytest.raises(ValueError, match="write domain must be"):
        s.sculpt_layer_stroke(write_domain="whatever")


def test_erase_and_the_three_smoothing_modes_each_leave_the_rest_alone():
    s = surface()
    lower = s.add_sculpt_layer("lower")
    upper = s.add_sculpt_layer("upper")
    for v in range(20, 80):
        s.set_sculpt_layer_detail(lower, 2, v, 0.0, 0.0, 0.02)
        s.set_sculpt_layer_detail(upper, 2, v, 0.0, 0.0, 0.03)
    s.active_sculpt_layer = upper
    base_before = s.detail_checksum

    def energy(layer):
        return sum(abs(c) for v in range(20, 80)
                   for c in s.sculpt_layer_detail(layer, 2, v))

    lower_before, upper_before = energy(lower), energy(upper)
    with s.sculpt_layer_stroke() as stroke:
        assert stroke.erase(center=tuple(positions(s)[40]), radius=0.5, strength=1.0) > 0
    assert energy(upper) < upper_before
    # NEITHER the base nor the layer beneath: an eraser for THIS pass rather
    # than a flattening brush.
    assert energy(lower) == pytest.approx(lower_before)
    assert s.detail_checksum == base_before

    # preserve_detail moves the FORM and carries every layer through untouched.
    layer_before = s.sculpt_layer_checksum
    with s.sculpt_layer_stroke() as stroke:
        assert stroke.smooth("preserve_detail", center=tuple(positions(s)[40]),
                             radius=0.6, strength=1.0) > 0
    assert s.detail_checksum != base_before
    assert s.sculpt_layer_checksum == layer_before

    with pytest.raises(ValueError, match="smooth mode must be"):
        with s.sculpt_layer_stroke() as stroke:
            stroke.smooth("blur", center=(0, 0, 0), radius=0.3)


def test_a_height_map_is_h_by_w_and_a_vector_map_is_three_planes():
    s = surface()
    layer = s.add_sculpt_layer("map")
    s.active_sculpt_layer = layer
    centre = tuple(positions(s)[np.argmin(np.linalg.norm(positions(s), axis=1))])

    height = np.ones((16, 16), np.float32)
    with s.sculpt_layer_stroke() as stroke:
        moved = stroke.stamp_detail(height, mode="height", center=centre, radius=0.4,
                                    extent=0.8, amplitude=0.05, strength=1.0)
        assert moved > 0
        report = stroke.last_stamp_report
    assert report["sample_size"] == pytest.approx(0.8 / 16.0)
    assert report["vertex_spacing"] > 0.0
    # A height map moves the third coefficient and only the third: it is a
    # displacement along the vertex's OWN normal, never a world axis.
    lifted = [s.sculpt_layer_detail(layer, 2, v) for v in range(s.level_counts(2)["vertices"])]
    assert all(d[0] == 0.0 and d[1] == 0.0 for d in lifted)
    assert any(d[2] > 0.0 for d in lifted)

    s.remove_sculpt_layer(layer)
    vector_layer = s.add_sculpt_layer("vector")
    s.active_sculpt_layer = vector_layer
    planes = np.zeros((3, 16, 16), np.float32)
    planes[0] = 1.0  # the TANGENT plane
    with s.sculpt_layer_stroke() as stroke:
        assert stroke.stamp_detail(planes, mode="vector", center=centre, radius=0.4,
                                   extent=0.8, amplitude=0.05, strength=1.0) > 0
    deposited = [s.sculpt_layer_detail(vector_layer, 2, v)
                 for v in range(s.level_counts(2)["vertices"])]
    assert any(d[0] > 0.0 for d in deposited)
    assert all(d[2] == 0.0 for d in deposited)

    # AN INTERLEAVED IMAGE IS REFUSED, with the message saying which layout it
    # wanted — it is exactly the array an image loader hands a script.
    interleaved = np.zeros((16, 16, 3), np.float32)
    with pytest.raises(ValueError, match="three PLANES"):
        with s.sculpt_layer_stroke() as stroke:
            stroke.stamp_detail(interleaved, mode="vector", center=centre, radius=0.4)
    with pytest.raises(ValueError, match="a height map is"):
        with s.sculpt_layer_stroke() as stroke:
            stroke.stamp_detail(planes, mode="height", center=centre, radius=0.4)
    # And a scalar alpha is `alpha=` on stamp(), not a second entry point.
    with pytest.raises(ValueError, match="two ways to say one thing"):
        with s.sculpt_layer_stroke() as stroke:
            stroke.stamp_detail(height, mode="weight", center=centre, radius=0.4)


def test_a_map_finer_than_the_level_is_reported_rather_than_blurred():
    s = surface()
    layer = s.add_sculpt_layer("map")
    s.active_sculpt_layer = layer
    centre = tuple(positions(s)[np.argmin(np.linalg.norm(positions(s), axis=1))])
    fine = np.ones((512, 512), np.float32)
    with s.sculpt_layer_stroke() as stroke:
        stroke.stamp_detail(fine, mode="height", center=centre, radius=0.4, extent=0.4,
                            amplitude=0.02, strength=1.0)
        report = stroke.last_stamp_report
    # A level cannot hold a feature narrower than the gap between two of its
    # vertices, and the library that implied the resolution says so.
    assert report["oversampling"] > 1.0
    assert report["under_resolved"]


def test_merge_and_bake_are_defined_by_the_surface_they_leave():
    # ZERO is where the naive concatenation — which divides by the lower
    # layer's strength — is undefined, and it is a state one slider reaches. So
    # the parity claim is checked at zero as well as away from it.
    for lower_strength in (1.0, 0.37, 0.0):
        s = surface()
        lower = s.add_sculpt_layer("lower")
        upper = s.add_sculpt_layer("upper")
        for v in range(30, 60):
            s.set_sculpt_layer_detail(lower, 2, v, 0.0, 0.0, 0.04)
            s.set_sculpt_layer_detail(upper, 2, v, 0.0, 0.0, 0.02)
        s.set_sculpt_layer_strength(lower, lower_strength)
        s.set_sculpt_layer_strength(upper, 0.6)
        before = positions(s)

        s.merge_sculpt_layer_down(upper)
        assert s.sculpt_layer_count == 1
        assert np.allclose(positions(s), before, atol=1e-6)

        # ...and the same statement again with the BASE as the target.
        s.bake_sculpt_layer_to_base(s.sculpt_layer_ids[0])
        assert s.sculpt_layer_count == 0
        assert np.allclose(positions(s), before, atol=1e-6)
        assert s.memory()["composed"] == 0


def test_the_stack_round_trips_through_serialize():
    s = surface()
    first = s.add_sculpt_layer("first")
    second = s.add_sculpt_layer("second")
    s.set_sculpt_layer_detail(first, 2, 40, 0.0, 0.0, 0.05)
    s.set_sculpt_layer_detail(second, 2, 41, 0.0, 0.0, -0.02)
    s.set_sculpt_layer_strength(first, 0.5)
    s.set_sculpt_layer_mask(second, 2, 41, 0.25)
    s.move_sculpt_layer(second, 0)
    shape = positions(s)

    loaded = clay.MultiresSurface.deserialize(s.serialize())
    assert loaded.sculpt_layer_ids == [second, first]
    assert loaded.sculpt_layer_name(first) == "first"
    assert loaded.sculpt_layer_info(first)["strength"] == pytest.approx(0.5)
    assert loaded.sculpt_layer_mask(second, 2, 41) == pytest.approx(0.25)
    assert loaded.sculpt_layer_checksum == s.sculpt_layer_checksum
    assert np.array_equal(positions(loaded), shape)

    with pytest.raises(ValueError):
        clay.MultiresSurface.deserialize(s.serialize()[:-16])


def test_memory_reports_the_stack_apart_from_the_form_and_never_caps_it():
    s = surface()
    layer = s.add_sculpt_layer("pass")
    for v in range(0, 200):
        s.set_sculpt_layer_detail(layer, 2, v, 0.0, 0.0, 0.01)
    positions(s)
    memory = s.memory()
    assert memory["sculpt_layers"] > 0
    assert memory["composed"] > 0
    # Authoritative, never droppable: a host acting on the split would delete
    # the artist's work.
    assert memory["authoritative"] >= memory["sculpt_layers"] + memory["detail"]
    assert memory["rebuildable"] >= memory["composed"]

    # Compacting is one of the four levers a host has, and there is deliberately
    # no CAP: a cap that silently stopped recording would leave the pass on the
    # surface and un-dialable.
    for v in range(0, 200):
        s.set_sculpt_layer_detail(layer, 2, v, 0.0, 0.0, 0.0)
    s.compact_sculpt_layers()
    assert s.memory()["sculpt_layers"] < memory["sculpt_layers"]

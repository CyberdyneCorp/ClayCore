

def test_warp_cost_accumulates_with_each_drag():
    """Issue #452: a grab is not free after it lands, and this is how a host asks.

    `consolidation_cost` says what baking the layer would cost. This says
    whether it has accumulated enough warps to be worth baking -- which is the
    question a host has first, and had no way to ask.
    """
    import pyclay

    doc = pyclay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(pyclay.Sphere(1.0))

    cost = layer.warp_cost()
    assert cost["items"] == 1
    assert cost["warps"] == 0
    assert cost["warped_items"] == 0

    layer.move_surface(centre=(0, 0, 1.0), displacement=(0, 0, 0.05), radius=0.5)
    after = layer.warp_cost()
    assert after["warps"] == 1
    assert after["warped_items"] == 1
    # A grab has finite support, so a culled tape can drop it where its region
    # does not reach -- which is what the per-brick paths win.
    assert after["finite_support_warps"] == 1

    # Nothing composes two drags, which is the whole of why the cost climbs.
    layer.move_surface(centre=(0.2, 0, 0.9), displacement=(0, 0, 0.05), radius=0.5)
    twice = layer.warp_cost()
    assert twice["warps"] == 2
    assert twice["warped_items"] == 1  # one item, carrying two warps


# The "a layer that cannot carry a warp reports zeroes" case is asserted in
# tests/unit/test_c_sdf_sculpt.cpp and not here: the C entry point takes a layer
# ID, so it can be handed a voxel layer, while pyclay's add_voxel_layer returns a
# VoxelGrid rather than a Layer and the case is unreachable through this binding.

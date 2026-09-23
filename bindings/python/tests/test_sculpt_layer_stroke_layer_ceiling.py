# The `layer` verb through a layered stroke: `SculptLayerStroke.stamp`.
#
# REGRESSION for #628. The stroke handed the engine a fixed layer height of
# zero and took no argument for it, so `stamp("layer", ...)` had a ceiling of
# zero, moved nothing and returned 0 — indistinguishable from a fully masked
# stamp. Same class as #627 on `MultiresSculptor.stamp`, fixed by #636.

import numpy as np

import pyclay as clay

CEILING = 0.08


def plane_quads(n=6, half=1.0):
    step = 2.0 * half / n
    positions = np.array(
        [[-half + step * x, 0.0, -half + step * z] for z in range(n + 1) for x in range(n + 1)],
        dtype=np.float32)
    stride = n + 1
    quads = []
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            quads += [a, a + 1, a + stride + 1, a + stride]
    return clay.Mesh.from_quads(positions, np.array(quads, dtype=np.uint32))


def layered_surface():
    s = clay.MultiresSurface.from_mesh(plane_quads())
    s.add_level()
    s.sculpt_level = 1
    s.active_sculpt_layer = s.add_sculpt_layer("pass")
    return s


def travel(was, now):
    return float(np.linalg.norm(np.asarray(now) - np.asarray(was), axis=1).max())


def moved_by(verb, **kwargs):
    s = layered_surface()
    with s.sculpt_layer_stroke() as stroke:
        return stroke.stamp(verb, (0.0, 0.0, 0.0), 0.6, 1.0, **kwargs)


def test_a_layer_stamp_in_a_layered_stroke_moves_what_draw_moves():
    draw = moved_by("draw")
    # PRECONDITION: the dab reaches vertices at all, so a zero below is the
    # verb's ceiling and not an empty region.
    assert draw > 0
    # The default ceiling is the engine's, as on every other stamp entry point.
    assert moved_by("layer") == draw
    assert moved_by("layer", layer_height=CEILING) == draw


def test_a_layer_stroke_settles_at_its_ceiling_and_lands_in_the_channel():
    s = layered_surface()
    before = np.array(s.positions_at(1))
    base = s.detail_checksum
    layers = s.sculpt_layer_checksum
    with s.sculpt_layer_stroke() as stroke:
        counts = [stroke.stamp("layer", (0.0, 0.0, 0.0), 0.6, 1.0, layer_height=CEILING)
                  for _ in range(6)]
    assert counts[0] > 0
    # A dab onto a surface already at its ceiling has nothing left to move.
    assert counts[1:] == [0] * 5
    moved = travel(before, s.positions_at(1))
    # Six full-strength dabs in one gesture: at the ceiling, not six times it.
    assert CEILING * 0.5 < moved <= CEILING + 1e-5
    # Into the stroke's channel, and nothing under it.
    assert s.sculpt_layer_checksum != layers
    assert s.detail_checksum == base

# The `layer` verb on a hierarchy through pyclay: that a stamp has a ceiling at
# all, and that the ceiling holds at a depth boundary when a trim lands in the
# middle of the gesture.
#
# `MultiresSculptor.stamp` used to hand the engine a layer height of zero, so a
# `layer` stamp moved nothing and reported 0 — the hierarchy's per-dab Layer
# was unreachable from Python, and with it every gate on its ceiling.

import numpy as np
import pytest

import pyclay as clay

CEILING = 0.08


def bumpy_quads(n=6, half=1.0):
    """The 6x6 cage the C++ regional gates use: patch z * 6 + x is quad z * 6 + x."""
    step = 2.0 * half / n
    positions = np.array(
        [[-half + step * x, 0.15 * ((x * 7 + z * 3) % 5), -half + step * z]
         for z in range(n + 1) for x in range(n + 1)],
        dtype=np.float32)
    stride = n + 1
    quads = []
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            quads += [a, a + 1, a + stride + 1, a + stride]
    return clay.Mesh.from_quads(positions, np.array(quads, dtype=np.uint32))


def regional():
    s = clay.MultiresSurface.from_mesh(bumpy_quads())
    s.refine_patches_to_level([z * 6 + x for z in (2, 3) for x in (2, 3)], 3)
    s.sculpt_level = 3
    assert not s.uniform_depth
    return s


def rim_centre(s):
    p = np.array(s.positions_at(3))
    return tuple(float(v) for v in p[np.argmin(np.linalg.norm(p - [-1.0 / 3.0, 0, 0], axis=1))])


def worst_travel(was, now):
    return float(np.linalg.norm(np.asarray(now) - np.asarray(was), axis=1).max())


def test_a_layer_stamp_on_a_hierarchy_deposits_to_its_ceiling():
    s = clay.MultiresSurface.from_mesh(bumpy_quads())
    s.add_level()
    s.sculpt_level = 1
    before = np.array(s.positions_at(1))
    sculptor = clay.MultiresSculptor(s)
    sculptor.begin_stroke()
    for _ in range(6):
        assert sculptor.stamp("layer", (0.0, 0.3, 0.0), 0.6, 1.0, layer_height=CEILING) > 0
    travel = worst_travel(before, s.positions_at(1))
    # Six full-strength dabs on one record: at the ceiling, not six times it.
    assert CEILING * 0.5 < travel <= CEILING + 1e-5


def layer_stroke(pressure):
    s = regional()
    pristine = np.array(s.positions_at(2))
    centre = rim_centre(s)
    sculptor = clay.MultiresSculptor(s)
    sculptor.begin_stroke()
    assert sculptor.stamp("layer", centre, 0.5, 1.0, layer_height=CEILING) > 0
    token = sculptor.seed_revision
    if pressure is not None:
        s.trim(pressure)
    assert sculptor.stamp("layer", centre, 0.5, 1.0, layer_height=CEILING) > 0
    # Read after the second dab: a new token is minted only when the level
    # sculptor is rebuilt, so a changed one is the rebind itself.
    rebound = sculptor.seed_revision != token
    return pristine, np.array(s.positions_at(2)), rebound


@pytest.mark.parametrize("pressure", [clay.Pressure.urgent, clay.Pressure.critical])
def test_a_trim_mid_stroke_does_not_lift_the_coarse_sides_ceiling(pressure):
    pristine, kept, kept_rebound = layer_stroke(None)
    assert not kept_rebound
    settled = worst_travel(pristine, kept)
    assert 0.01 < settled <= CEILING  # the stroke reached the coarse level at all

    _, trimmed, rebound = layer_stroke(pressure)
    assert rebound  # the precondition: the trim really rebound the sculptor
    assert worst_travel(pristine, trimmed) <= CEILING
    assert np.array_equal(trimmed, kept)

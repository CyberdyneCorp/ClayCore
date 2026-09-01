# The shared brush runtime through pyclay (python-bindings spec,
# add-shared-brush-runtime).
#
# WHY A PYTHON SUITE AND NOT ONLY THE C++ ONE. Two of this change's claims are
# claims about the SHIPPED WHEEL rather than about the engine, and a C++ test
# cannot make either:
#
#   - the automask an artist enables reaches all three representations THROUGH
#     ONE ARGUMENT. Before this change `MeshSculptor.stamp` had no `automask`
#     parameter at all, so the factor was reachable from Python only by going
#     through `apply_preset` and letting a preset carry it — which meant the
#     adaptive divergence was not merely present, it was unreachable from the
#     wheel on every representation.
#   - `arena_stats` is what lets a host budget for a stroke's scratch without a
#     C++ allocation counter, which is the whole reason those three numbers
#     cross the ABI at all.
#
# The numbers asserted here are the same ones `test_dynamic_shared_brush_parity`
# asserts in C++, deliberately: if the binding ever started building a different
# descriptor, the two suites would disagree and one of them would say so.

import numpy as np
import pytest

import pyclay as clay


# -- fixtures -----------------------------------------------------------------


def plane_grid(n=16, half=1.0):
    """A triangulated grid with no duplicated vertices.

    The vertex numbering survives every representation, which is what makes a
    byte comparison across them meaningful rather than a pairing heuristic.
    """
    xs = np.linspace(-half, half, n + 1, dtype=np.float32)
    positions = np.array(
        [(xs[x], 0.0, xs[z]) for z in range(n + 1) for x in range(n + 1)], np.float32
    )
    stride = n + 1
    indices = []
    for z in range(n):
        for x in range(n):
            a = z * stride + x
            b, c, d = a + 1, a + stride, a + stride + 1
            indices += [a, c, b, b, c, d]
    return positions, np.array(indices, np.uint32)


def cube_sphere(n=10, radius=1.0):
    """Curvature, built from a cube grid so the fixture itself rounds exactly."""
    axes = [(0, 1, 2), (0, 1, 2), (1, 2, 0), (1, 2, 0), (2, 0, 1), (2, 0, 1)]
    signs = [1.0, -1.0, 1.0, -1.0, 1.0, -1.0]
    positions, indices = [], []
    for f in range(6):
        base = len(positions)
        for v in range(n + 1):
            for u in range(n + 1):
                c = [0.0, 0.0, 0.0]
                c[axes[f][0]] = -1.0 + 2.0 * u / n
                c[axes[f][1]] = -1.0 + 2.0 * v / n
                c[axes[f][2]] = signs[f]
                length = float(np.sqrt(c[0] ** 2 + c[1] ** 2 + c[2] ** 2))
                positions.append(tuple(x / length * radius for x in c))
        stride = n + 1
        for v in range(n):
            for u in range(n):
                a = base + v * stride + u
                b, c2, d = a + 1, a + stride, a + stride + 1
                if signs[f] > 0.0:
                    indices += [a, c2, b, b, c2, d]
                else:
                    indices += [a, b, c2, b, d, c2]
    return np.array(positions, np.float32), np.array(indices, np.uint32)


def mesh_of(positions, indices):
    return clay.Mesh.from_triangles(positions, indices)


def topology_off():
    t = clay.TopologySettings()
    t.enabled = False
    return t


def automask(factors, normal_angle=None, boundary_rings=None):
    a = clay.AutomaskSettings()
    a.factors = int(factors)
    if normal_angle is not None:
        a.normal_angle = normal_angle
    if boundary_rings is not None:
        a.boundary_rings = boundary_rings
    return a


NORMAL_ANGLE = int(clay.AutomaskFactor.NORMAL_ANGLE.value)
TOPOLOGY_CONNECTED = int(clay.AutomaskFactor.TOPOLOGY_CONNECTED.value)
BOUNDARY = int(clay.AutomaskFactor.BOUNDARY.value)


# -- the automask reaches every representation through one argument -----------


def test_the_automask_argument_exists_on_every_sculptor():
    """The signature claim, before any behaviour.

    The brush-engine delta says the factors SHALL be settable on every sculptor
    that offers the automask, with the same signature. A divergence that was
    fixed on one representation and left on another would be a new asymmetry
    rather than a fix, and this is what says there is not one.
    """
    positions, indices = plane_grid()
    fixed = clay.MeshSculptor(mesh_of(positions, indices), 0.0)
    adaptive = clay.DynamicSculptor(clay.DynamicSurface.from_mesh(mesh_of(positions, indices)))

    for sculptor in (fixed, adaptive):
        assert "automask" in sculptor.stamp.__doc__
        assert "stamp_azimuth" in sculptor.stamp.__doc__
        assert hasattr(sculptor, "set_automask_inputs")
        assert hasattr(sculptor, "arena_stats")

    assert "automask" in clay.MultiresSculptor.stamp.__doc__
    assert "automask" in clay.MeshSculptor.apply_stroke.__doc__


def test_the_automask_moves_the_same_vertices_on_both_representations():
    """THE DIVERGENCE THIS CHANGE EXISTS TO CLOSE, from the wheel.

    Before it, the adaptive pair would have read 365 and 365: the descriptor
    carried the factor to `DynamicSculptor::gather`, which never read it. The
    fixed pair read 365 and 149 all along, so the two representations answered
    the same argument differently and nothing said so.
    """
    positions, indices = cube_sphere()

    def fixed_moved(factors):
        sculptor = clay.MeshSculptor(mesh_of(positions, indices), 1e-5)
        return sculptor.stamp(
            "grab",
            (0, 0, 1.0),
            1.6,
            0.4,
            direction=(0.0, 0.0, 0.125),
            geodesic=False,
            automask=automask(factors, normal_angle=0.5),
        )

    def adaptive_moved(factors):
        surface = clay.DynamicSurface.from_mesh(mesh_of(positions, indices))
        sculptor = clay.DynamicSculptor(surface)
        report = sculptor.stamp(
            "grab",
            (0, 0, 1.0),
            1.6,
            0.4,
            topology=topology_off(),
            direction=(0.0, 0.0, 0.125),
            geodesic=False,
            automask=automask(factors, normal_angle=0.5),
        )
        return report["moved"]

    assert fixed_moved(0) == 365
    assert adaptive_moved(0) == 365
    assert fixed_moved(NORMAL_ANGLE) == 149
    assert adaptive_moved(NORMAL_ANGLE) == 149


def test_a_topological_automask_masks_the_same_set_on_both():
    """`Boundary` and `TopologyConnected` are set-valued and purely topological,
    so on an identical vertex set the two representations must agree exactly —
    not approximately. That is the assertion that says the automask genuinely
    RUNS on the adaptive path rather than running and returning ones.
    """
    positions, indices = plane_grid()
    factors = BOUNDARY | TOPOLOGY_CONNECTED

    def moved(factors_value):
        fixed = clay.MeshSculptor(mesh_of(positions, indices), 0.0)
        surface = clay.DynamicSurface.from_mesh(mesh_of(positions, indices))
        adaptive = clay.DynamicSculptor(surface)
        args = dict(direction=(0.125, 0.25, 0.0), geodesic=False,
                    automask=automask(factors_value, boundary_rings=2))
        a = fixed.stamp("grab", (0, 0, 0), 1.5, 0.5, **args)
        b = adaptive.stamp("grab", (0, 0, 0), 1.5, 0.5, topology=topology_off(), **args)
        return a, b["moved"]

    open_fixed, open_adaptive = moved(0)
    masked_fixed, masked_adaptive = moved(factors)

    assert open_fixed == open_adaptive == 289  # the whole 17x17 patch
    assert masked_fixed == masked_adaptive == 225  # the 15x15 interior
    # ...and the automask actually did something, or the equality above is
    # satisfied by two representations that both did nothing.
    assert masked_adaptive < open_adaptive


def test_the_automask_reaches_the_hierarchy_too():
    positions, indices = plane_grid(n=8, half=2.0)
    surface = clay.MultiresSurface.from_mesh(mesh_of(positions, indices))
    surface.add_level()
    sculptor = clay.MultiresSculptor(surface)

    args = dict(direction=(0.125, 0.25, 0.0), geodesic=False)
    open_moved = sculptor.stamp("grab", (0, 0, 0), 3.0, 0.5, **args)

    surface2 = clay.MultiresSurface.from_mesh(mesh_of(positions, indices))
    surface2.add_level()
    masked = clay.MultiresSculptor(surface2).stamp(
        "grab", (0, 0, 0), 3.0, 0.5,
        automask=automask(BOUNDARY | TOPOLOGY_CONNECTED, boundary_rings=2), **args
    )

    assert open_moved > 0
    assert masked < open_moved


# -- the stamp's grain ---------------------------------------------------------


def test_a_zero_azimuth_writes_byte_identical_positions_to_naming_none():
    """D5, FROM THE WHEEL, and the reason `stamp_azimuth` defaults to an exact
    zero rather than to a rotation by zero.

    `make_stamp_frame` branches on precisely this value: turning the basis by
    `cos 0` and `sin 0` leaves a `-0.0f` where an unrotated axis has `+0.0f`,
    and a host that passed 0.0 explicitly must land on the same bits as one that
    passed nothing at all.
    """
    positions, indices = plane_grid()
    alpha = np.array([[0.0, 1.0], [1.0, 0.0]], np.float32)

    def stamped(**extra):
        mesh = mesh_of(positions, indices)
        sculptor = clay.MeshSculptor(mesh, 0.0)
        sculptor.stamp(
            "draw", (0, 0, 0), 0.5, 0.5,
            alpha=alpha, alpha_extent=1.0, alpha_direction=(0, 1, 0),
            geodesic=False, **extra,
        )
        return np.array(mesh.positions, np.float32)

    default = stamped()
    explicit_zero = stamped(stamp_azimuth=0.0)
    assert default.tobytes() == explicit_zero.tobytes()

    # ...and a non-zero azimuth turns the alpha, so the field is not simply
    # being ignored. This is the half that makes the equality above a gate.
    turned = stamped(stamp_azimuth=1.5707964)
    assert turned.tobytes() != default.tobytes()


def test_the_azimuth_is_settable_on_the_brush_settings_too():
    settings = clay.MeshBrushSettings()
    assert settings.stamp_azimuth == 0.0
    settings.stamp_azimuth = 0.75
    assert settings.stamp_azimuth == pytest.approx(0.75)


# -- what a stroke's scratch costs ---------------------------------------------


def test_arena_stats_start_at_zero_and_report_growth():
    """An arena that reported a reserve before the first stamp would charge a
    host for storage no stroke had asked for."""
    positions, indices = plane_grid()
    surface = clay.DynamicSurface.from_mesh(mesh_of(positions, indices))
    sculptor = clay.DynamicSculptor(surface)

    before = sculptor.arena_stats
    assert set(before) == {"capacity_bytes", "high_water_bytes", "growths"}
    assert before == {"capacity_bytes": 0, "high_water_bytes": 0, "growths": 0}

    sculptor.stamp("grab", (0, 0, 0), 0.5, 0.05, topology=topology_off(),
                   direction=(0.125, 0.25, 0.0), geodesic=False)

    after = sculptor.arena_stats
    assert after["capacity_bytes"] > 0
    assert after["growths"] > 0
    # A single stamp's peak cannot exceed what the arena owns.
    assert after["high_water_bytes"] <= after["capacity_bytes"]


def test_the_arena_stops_growing_over_a_stroke():
    """THE NUMBER A HOST WATCHES, and the one an allocation count cannot give:
    scratch that grows a little per stamp allocates nothing after warm-up and
    consumes memory without bound."""
    positions, indices = plane_grid()
    surface = clay.DynamicSurface.from_mesh(mesh_of(positions, indices))
    sculptor = clay.DynamicSculptor(surface)

    def dab(i):
        sculptor.stamp("grab", (-0.0625 + 0.03125 * (i % 6), 0.0, 0.0), 0.5, 0.02,
                       topology=topology_off(), direction=(0.125, 0.25, 0.0), geodesic=False)

    for i in range(8):
        dab(i)
    warm = sculptor.arena_stats
    assert warm["growths"] > 0

    for i in range(40):
        dab(i)
    settled = sculptor.arena_stats

    assert settled["growths"] == warm["growths"]
    assert settled["capacity_bytes"] == warm["capacity_bytes"]


def test_every_sculptor_reports_an_arena():
    positions, indices = plane_grid(n=8, half=2.0)

    fixed = clay.MeshSculptor(mesh_of(positions, indices), 0.0)
    assert fixed.arena_stats["growths"] == 0

    surface = clay.MultiresSurface.from_mesh(mesh_of(positions, indices))
    surface.add_level()
    hierarchy = clay.MultiresSculptor(surface)
    # BEFORE THE FIRST STAMP no level is bound, so there is no arena to report
    # and every field is zero. That is the truth rather than a placeholder: a
    # multiresolution stamp is the fixed sculptor's stamp on the bound level's
    # own mesh, and that is the arena that grows.
    assert hierarchy.arena_stats == {"capacity_bytes": 0, "high_water_bytes": 0, "growths": 0}

    hierarchy.stamp("grab", (0, 0, 0), 1.0, 0.25, direction=(0, 0.125, 0), geodesic=False,
                    automask=automask(BOUNDARY, boundary_rings=2))
    assert hierarchy.arena_stats["capacity_bytes"] > 0


def test_two_sculptors_do_not_share_an_arena():
    """ONE PER SCULPTOR, NEVER A PROCESS-GLOBAL. A shared mutable arena would
    make a sculptor's reported cost depend on what else the host was doing."""
    positions, indices = plane_grid()
    a = clay.DynamicSculptor(clay.DynamicSurface.from_mesh(mesh_of(positions, indices)))
    b = clay.DynamicSculptor(clay.DynamicSurface.from_mesh(mesh_of(positions, indices)))

    a.stamp("grab", (0, 0, 0), 0.5, 0.05, topology=topology_off(),
            direction=(0.125, 0.25, 0.0), geodesic=False)

    assert a.arena_stats["growths"] > 0
    assert b.arena_stats == {"capacity_bytes": 0, "high_water_bytes": 0, "growths": 0}


# -- the refusals --------------------------------------------------------------


def test_set_automask_inputs_refuses_a_python_callable():
    """NOT AN OVERSIGHT, A CONSTRAINT. `AutomaskInputs` holds two callables the
    engine evaluates per workset entry; a stamp releases the GIL and runs them
    from a worker thread, where calling back into the interpreter is a crash
    rather than a slowdown. So the binding takes baked fields and refuses a
    function, which is the only safe answer it can give.
    """
    positions, indices = plane_grid()
    sculptor = clay.MeshSculptor(mesh_of(positions, indices), 0.0)

    # A ValueError NAMING THE ARGUMENT, which is what this repository's other
    # malformed-argument paths give. REGRESSION: both of these raised
    # `RuntimeError: std::bad_cast` — nanobind's rendering of a bare
    # `nb::cast<T>` on the wrong type — which named neither the argument nor
    # what it should have been, on the one call whose obvious wrong guess is
    # exactly a callable.
    with pytest.raises(ValueError, match="cavity must be a MaskField"):
        sculptor.set_automask_inputs(cavity=lambda p: 0.5)
    with pytest.raises(ValueError, match="groups must be"):
        sculptor.set_automask_inputs(groups=lambda p: 1)
    # ...and the message says WHY a callable is not an option, because the
    # obvious next move is to try harder to pass one.
    with pytest.raises(ValueError, match="worker thread"):
        sculptor.set_automask_inputs(cavity=lambda p: 0.5)

    # Clearing both is always allowed and is how a stroke ends.
    sculptor.set_automask_inputs()
    sculptor.set_automask_inputs(cavity=None, groups=None, active_group=0)


def test_an_unknown_verb_and_a_bad_radius_are_refused():
    """The typed refusals on the paths this change widened. A stamp that
    silently became something else would be worse than one that failed."""
    positions, indices = plane_grid()
    surface = clay.DynamicSurface.from_mesh(mesh_of(positions, indices))
    sculptor = clay.DynamicSculptor(surface)

    with pytest.raises(Exception):
        sculptor.stamp("not-a-verb", (0, 0, 0), 0.5, 0.5, topology=topology_off())
    with pytest.raises(Exception):
        sculptor.stamp("grab", (0, 0, 0), -1.0, 0.5, topology=topology_off())
    with pytest.raises(Exception):
        sculptor.stamp("grab", (0, 0, 0), 0.0, 0.5, topology=topology_off())

    # ...and the well-formed call still works, so the refusals above are not the
    # binding failing for an unrelated reason.
    assert sculptor.stamp("grab", (0, 0, 0), 0.5, 0.5, topology=topology_off(),
                          direction=(0.125, 0.25, 0.0), geodesic=False)["moved"] > 0


def test_an_automask_of_the_wrong_type_is_refused():
    positions, indices = plane_grid()
    sculptor = clay.MeshSculptor(mesh_of(positions, indices), 0.0)
    # REGRESSION: `automask=3` raised `RuntimeError: std::bad_cast`. It is a
    # plausible mistake — the FACTORS are an integer, one level down — so the
    # refusal has to say what the argument is and how to build one.
    with pytest.raises(ValueError, match="automask must be an AutomaskSettings"):
        sculptor.stamp("grab", (0, 0, 0), 0.5, 0.5, automask=3)
    with pytest.raises(ValueError, match="AutomaskSettings"):
        sculptor.stamp("grab", (0, 0, 0), 0.5, 0.5, automask="normal_angle")
    # A well-formed one still works, so the refusals are not the binding
    # failing for an unrelated reason.
    assert sculptor.stamp("grab", (0, 0, 0), 0.5, 0.5, direction=(0.125, 0.25, 0.0),
                          geodesic=False, automask=automask(NORMAL_ANGLE)) > 0


# -- determinism ----------------------------------------------------------------


def test_an_automasked_stamp_is_deterministic():
    """Two runs of the same automasked stamp write the same bytes.

    The automask's two topological factors are breadth- and depth-first walks
    over arena scratch, and scratch that was read before it was written would
    produce a result that varied run to run — which is exactly the failure a
    single run cannot see.
    """
    positions, indices = plane_grid()

    def run():
        mesh = mesh_of(positions, indices)
        sculptor = clay.MeshSculptor(mesh, 0.0)
        for i in range(4):
            sculptor.stamp("grab", (-0.05 + 0.02 * i, 0.0, 0.0), 1.2, 0.1,
                           direction=(0.125, 0.25, 0.0), geodesic=False,
                           automask=automask(NORMAL_ANGLE | TOPOLOGY_CONNECTED | BOUNDARY,
                                             boundary_rings=2))
        return np.array(mesh.positions, np.float32).tobytes()

    assert run() == run()

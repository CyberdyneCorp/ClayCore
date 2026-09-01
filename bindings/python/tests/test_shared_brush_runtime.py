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

import gc
import math

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
SURFACE_GROUP = int(clay.AutomaskFactor.SURFACE_GROUP.value)


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


# -- the two estimators the C ABI cannot carry --------------------------------
#
# `set_automask_inputs` is the ONE call this change adds that pyclay has and the
# C ABI does not, and `tools/check_binding_parity.py` records all three of them
# as exemptions. An exemption is a promise that the Python side does something
# real; nothing in this suite kept that promise until these cases, which asserted
# only that the method existed and that it refused a callable. A binding that
# accepted the objects and dropped them on the floor would have passed every one
# of them.


def grouped_document(group=7):
    """A lattice that names the half-space x >= 0, and nothing else.

    A box fill rather than a painted mask because the boundary then falls on a
    plane the fixture's own vertices straddle exactly, which is what makes the
    two halves add back up to the whole below.
    """
    doc = clay.Document()
    groups = doc.groups(0.05)
    groups.fill(((0.0, -0.5, -1.5), (1.5, 0.5, 1.5)), group)
    # The document owns the lattice; the handle only borrows it. Returned
    # together so a caller cannot drop the document and keep the lattice.
    return doc, groups


def test_the_surface_group_estimator_reaches_every_representation():
    """THE EXEMPTION'S OWN GATE, on the change's headline claim.

    `SurfaceGroup` is the factor a mesh module structurally cannot compute for
    itself — the answer lives on the document's world lattice — so it is the
    factor that says the wiring is real rather than that the argument parsed.
    The three representations must agree exactly, not approximately: the lattice
    answers a world point, and all three ask it about the same points.
    """
    positions, indices = plane_grid()
    doc, groups = grouped_document()
    factor = automask(SURFACE_GROUP)

    def fixed(wire):
        sculptor = clay.MeshSculptor(mesh_of(positions, indices), 0.0)
        if wire:
            sculptor.set_automask_inputs(groups=groups, active_group=7)
        return sculptor.stamp("draw", (0, 0, 0), 1.5, 0.3, automask=factor)

    def adaptive(wire):
        surface = clay.DynamicSurface.from_mesh(mesh_of(positions, indices))
        sculptor = clay.DynamicSculptor(surface)
        if wire:
            sculptor.set_automask_inputs(groups=groups, active_group=7)
        return sculptor.stamp("draw", (0, 0, 0), 1.5, 0.3, topology=topology_off(),
                              automask=factor)["moved"]

    def hierarchy(wire):
        sculptor = clay.MultiresSculptor(
            clay.MultiresSurface.from_mesh(mesh_of(positions, indices)))
        if wire:
            sculptor.set_automask_inputs(groups=groups, active_group=7)
        return sculptor.stamp("draw", (0, 0, 0), 1.5, 0.3, automask=factor)

    # An UNWIRED sculptor asked for the surface-group factor has no lattice to
    # ask, and the documented answer is that the factor does nothing rather than
    # that the stamp fails: a host that has not named any groups yet is not in
    # error. That is what makes the pair below a measurement of the wiring and
    # not of the flag.
    for representation in (fixed, adaptive, hierarchy):
        assert representation(False) == 289
        assert representation(True) == 153

    # AND THE TWO HALVES ADD UP. Isolating group 7 and isolating the ungrouped
    # remainder partition the stamp's region exactly, which is the claim a bare
    # "fewer vertices moved" cannot make — a wiring that returned a constant
    # would also move fewer, and would move the same fewer whichever group was
    # active.
    ungrouped = clay.MeshSculptor(mesh_of(positions, indices), 0.0)
    ungrouped.set_automask_inputs(groups=groups, active_group=0)
    assert ungrouped.stamp("draw", (0, 0, 0), 1.5, 0.3, automask=factor) == 136
    assert 153 + 136 == 289


def test_clearing_the_estimators_reopens_the_stamp():
    """A stroke ENDS by clearing these, and the sculptor is then what it was.

    An estimator that outlived the stroke that set it would mask the next one
    against a lattice the artist has moved on from, which is the failure mode
    that looks like the brush having gone wrong rather than like state having
    leaked.
    """
    positions, indices = plane_grid()
    doc, groups = grouped_document()
    factor = automask(SURFACE_GROUP)
    sculptor = clay.MeshSculptor(mesh_of(positions, indices), 0.0)

    sculptor.set_automask_inputs(groups=groups, active_group=7)
    assert sculptor.stamp("draw", (0, 0, 0), 1.5, 0.3, automask=factor) == 153
    sculptor.set_automask_inputs()
    assert sculptor.stamp("draw", (0, 0, 0), 1.5, 0.3, automask=factor) == 289


def test_the_sculptor_holds_the_estimator_objects_it_was_given():
    """The binding stores a raw pointer into the lattice and keeps the Python
    object alive with `nb::keep_alive`, because the alternative — copying the
    lattice per stroke — is the allocation the docstring tells hosts to avoid.

    WHAT THIS DOES AND DOES NOT PROVE, stated plainly: with the `keep_alive`
    removed the handle's last reference dies at the `del` and the stamp below
    reads freed memory, which a sanitizer catches and an ordinary run may not.
    So this is the SHAPE the mechanism has to keep — a caller may drop the
    handle and go on stamping — rather than the proof that it is kept. The
    proof is the asan-ubsan run over the same file.
    """
    positions, indices = plane_grid()
    doc, groups = grouped_document()
    sculptor = clay.MeshSculptor(mesh_of(positions, indices), 0.0)
    sculptor.set_automask_inputs(groups=groups, active_group=7)

    del groups
    gc.collect()

    assert sculptor.stamp("draw", (0, 0, 0), 1.5, 0.3, automask=automask(SURFACE_GROUP)) == 153


def test_the_cavity_estimator_reaches_a_stamp():
    """The other exemption, and the one whose input is a baked field.

    `mask_from_surface('cavity', ...)` bakes `brush::measure_at` onto a lattice,
    so this is the estimator the C++ path evaluates directly, SAMPLED. The
    fixture is two overlapping spheres — a genuine crease, and the same shape
    `examples/64_measuring_the_surface.py` uses and says why a torus is wrong
    for it.

    Asserted as a SUBSET rather than as a count, because the count is a
    marching-cubes vertex count and the subset is what a multiplicative mask
    MEANS: a masked stamp may move fewer vertices, never a different set.
    """
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.Sphere(r=0.5, position=(0.32, 0, 0)))
    layer.add(clay.Sphere(r=0.5, position=(-0.32, 0, 0)))
    cavity = doc.mask_from_surface("cavity", ((-1.0, -0.7, -0.7), (1.0, 0.7, 0.7)),
                                   cell_size=0.04, band=0.06, params={"scale": 0.25})
    # Without this the whole case is satisfied by a mask that painted nothing
    # and an automask that therefore had nothing to remove.
    assert cavity.painted_count > 0

    factor = clay.AutomaskSettings()
    factor.factors = int(clay.AutomaskFactor.CAVITY.value)
    factor.cavity_strength = 1.0
    # The crease, found from the two radii rather than guessed at.
    crease = (0.0, float(np.sqrt(0.5 ** 2 - 0.32 ** 2)), 0.0)

    def moved_set(wire):
        mesh = doc.mesh(resolution=32)
        before = np.array(mesh.positions, np.float32).copy()
        sculptor = clay.MeshSculptor(mesh, 1e-5)
        if wire:
            sculptor.set_automask_inputs(cavity=cavity)
        sculptor.stamp("draw", crease, 0.4, strength=0.3, automask=factor)
        after = np.array(mesh.positions, np.float32).copy()
        return set(np.flatnonzero(np.abs(after - before).sum(axis=1) > 0).tolist())

    open_set = moved_set(False)
    masked_set = moved_set(True)

    assert open_set
    assert masked_set < open_set


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


# -- the azimuth crosses the preset format ------------------------------------
#
# THE ONE PLACE AN ARTIST CAN PUT AN AZIMUTH AND KEEP IT. Nothing in `brush`
# resolves the azimuth from a stroke's direction of travel yet, so a rotated
# chisel is not something the stroke engine derives — it is something a person
# sets on a brush and saves. `BrushPreset` is that file format, and until this
# change it wrote every other identity field and stopped one short of this one.
#
# It is gated from Python as well as from C++ because this is the surface a host
# actually saves a library through: `pyclay_module.cpp` binds `serialize` and
# `deserialize` and, before these three cases, no Python test touched
# `BrushPreset` at all — the whole format was reachable from the wheel and
# ungated in it.


def test_preset_carries_the_stamp_azimuth_across_bytes():
    """A turned brush comes back turned.

    Every preset in the reference library has an azimuth of zero, so a
    round-trip over the library cannot see a field the format never writes. A
    non-default value is the only value that distinguishes carrying it from
    dropping it.
    """
    preset = clay.BrushPreset.by_name("Standard")
    preset.settings.stamp_azimuth = 0.75

    back = clay.BrushPreset.deserialize(preset.serialize())
    assert back.settings.stamp_azimuth == 0.75
    # And the rest of the record is unmoved: an appended field that had shifted
    # the earlier ones would show up here rather than as a wrong azimuth.
    assert back.name == preset.name
    assert back.settings.strength == preset.settings.strength


def test_preset_default_azimuth_survives_as_an_exact_zero():
    """Zero is the value the engine branches on, so it has to come back exact.

    `make_stamp_frame` returns the unrotated basis without evaluating the
    rotation when the azimuth is zero, and that branch is a correctness rule
    rather than an optimisation (design D5). A round trip that gave back -0.0 or
    a denormal would take the rotation path on a brush nobody turned.
    """
    for preset in clay.BrushPreset.library():
        back = clay.BrushPreset.deserialize(preset.serialize())
        assert back.settings.stamp_azimuth == 0.0
        # `math.copysign` rather than `== 0.0`, which -0.0 also satisfies.
        assert math.copysign(1.0, back.settings.stamp_azimuth) == 1.0


def test_preset_refuses_a_newer_schema_rather_than_reading_a_prefix():
    """The version gate, from the side a host meets it.

    A newer layout read as this one gives a brush that is not the brush somebody
    saved, which is worse than an error because it looks like it worked. The
    binding turns that refusal into an exception rather than a default-built
    preset.
    """
    assert clay.BrushPreset.version >= 2  # the azimuth's own version
    data = bytearray(clay.BrushPreset.by_name("Standard").serialize())
    data[4] = clay.BrushPreset.version + 1
    with pytest.raises(ValueError):
        clay.BrushPreset.deserialize(bytes(data))

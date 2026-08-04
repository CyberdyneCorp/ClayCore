"""The binding parity gate itself (c-abi spec: binding parity gate).

tools/check_binding_parity.py is what keeps the C ABI from falling behind
pyclay again. These cases run it against the module actually under test, and
then feed it surfaces it must reject, so a gate that has quietly stopped
checking anything fails here rather than in six months.
"""

import sys
from pathlib import Path

import pytest

import pyclay as clay

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

parity = pytest.importorskip("check_binding_parity")


@pytest.fixture(scope="module")
def declared():
    functions, enumerators = parity.c_surface()
    return functions | enumerators


def test_the_gate_passes_on_the_module_under_test(capsys):
    """The real run: pyclay imported, every capability mapped or exempted."""
    argv = sys.argv
    sys.argv = ["check_binding_parity.py", "--require-import"]
    try:
        code = parity.main()
    finally:
        sys.argv = argv
    out = capsys.readouterr()
    assert code == 0, f"the parity gate failed:\n{out.err}"
    assert "parity: OK" in out.out, out.out


def test_every_exemption_is_visible_in_the_output(capsys):
    """Spec: "the gate passes and the exemption is visible in its output"."""
    argv = sys.argv
    sys.argv = ["check_binding_parity.py", "--require-import"]
    try:
        parity.main()
    finally:
        sys.argv = argv
    out = capsys.readouterr().out
    for capability, reason in parity.EXEMPT.items():
        assert f"exempt {capability}" in out, f"{capability} is exempt but unreported"
        assert reason.split(";")[0][:30] in out, f"{capability} is reported without its reason"


def test_a_new_capability_without_a_c_entry_point_fails(declared):
    """Spec: "the parity gate fails in CI naming the missing capability"."""
    surface = {"VoxelGrid": ["sculpt_emboss"]}
    errors, _ = parity.check_members(surface, declared)
    assert len(errors) == 1, errors
    assert "VoxelGrid.sculpt_emboss" in errors[0]
    assert "clay_voxel_sculpt_emboss" in errors[0], (
        f"the failure does not say what was looked for: {errors[0]}")


def test_the_gate_exits_nonzero_on_a_missing_capability(monkeypatch, capsys):
    """End to end, not just the helper: a capability pyclay grew and the C ABI
    did not must take the whole gate down."""
    surface = dict(parity.parsed_surface())
    surface["VoxelGrid"] = surface["VoxelGrid"] + ["sculpt_emboss"]
    monkeypatch.setattr(parity, "load_module", lambda explicit: None)
    monkeypatch.setattr(parity, "parsed_surface", lambda: surface)
    argv = sys.argv
    sys.argv = ["check_binding_parity.py"]
    try:
        code = parity.main()
    finally:
        sys.argv = argv
    err = capsys.readouterr().err
    assert code == 1, "the gate passed a pyclay capability with no C entry point"
    assert "VoxelGrid.sculpt_emboss" in err, err


def test_a_new_class_without_a_recorded_constructor_fails(declared):
    surface = {"Superellipsoid": []}
    errors = parity.check_constructors(surface, declared)
    assert len(errors) == 1 and "Superellipsoid" in errors[0], errors


def test_a_class_with_no_prefix_rule_fails(declared):
    surface = {"Timeline": ["play"]}
    errors, _ = parity.check_members(surface, declared)
    assert len(errors) == 1 and "Timeline" in errors[0], errors


def test_a_stale_exemption_fails(declared, monkeypatch):
    """An exemption for something the C ABI now reaches must be removed, or
    the list becomes a record of things nobody rechecked."""
    monkeypatch.setitem(parity.EXEMPT, "VoxelGrid.get", "no longer true")
    errors, _ = parity.check_members({"VoxelGrid": ["get"]}, declared)
    assert len(errors) == 1 and "stale" in errors[0], errors


def test_a_new_string_choice_needs_an_enumerator(monkeypatch):
    """pyclay's brush shapes, falloffs, meshers and mirror axes are choices a
    C caller must be able to name too."""
    assert parity.check_string_choices(parity.c_surface()[1]) == []
    monkeypatch.setattr(parity, "STRING_CHOICES",
                        (("parse_brush_shape", "brush shape", "CLAY_NO_SUCH_"),))
    errors = parity.check_string_choices(parity.c_surface()[1])
    assert len(errors) == 2, errors  # cube and sphere
    assert "CLAY_NO_SUCH_CUBE" in " ".join(errors)


def test_the_parser_and_the_module_see_the_same_surface():
    """The static parse is the gate's fallback on a bare checkout, so it has
    to be the same reading of pyclay that importing it gives."""
    imported = parity.imported_surface(clay)
    assert parity.compare_surfaces(imported, parity.parsed_surface()) == []


def test_mesh_exposes_only_mesh_capabilities():
    """Regression: five Document methods were bound onto Mesh by a class chain
    that never closed. They cannot work — the self cast fails at call time —
    and they made Mesh advertise a document's surface."""
    document_only = {"add_voxel_layer", "voxel_layer", "raycast", "raycast_many",
                     "snap_to_surface", "add_sdf_layer", "eval", "gradients"}
    leaked = document_only & set(vars(clay.Mesh))
    assert not leaked, f"pyclay.Mesh exposes document methods: {sorted(leaked)}"
    for name in sorted(document_only & {"raycast", "snap_to_surface", "voxel_layer"}):
        assert hasattr(clay.Document, name), f"Document lost {name}"

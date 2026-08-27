"""docs/09's latency table quotes the baseline, and this is what checks it.

The table is what a host reads to size progress UI; `tests/device/baseline.json`
is what the release gate compares against. They were copied apart by hand and
drifted in BOTH directions (issue #273) — `pose_region` read 7.6x optimistic,
`magnify_pinch` 2.9x, while a dozen voxel rows read 2-3x pessimistic, and
`sdf_relax`'s figure matched no point on the axis its column header names.

An optimistic row is the one that hurts: a host sizing an affordance from it
under-budgets the work.
"""

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

doc = pytest.importorskip("check_doc_latency")


@pytest.fixture(autouse=True)
def _argv():
    """`main` reads sys.argv[1]; under pytest that is the test file."""
    saved = sys.argv
    sys.argv = ["check_doc_latency.py"]
    yield
    sys.argv = saved


def test_the_committed_table_agrees_with_the_committed_baseline():
    """The gate itself, run over the real files."""
    assert doc.main() == 0


def test_a_batched_case_is_quoted_per_application():
    """Or the table would publish a 128-dab drag as though it were one dab."""
    baseline = json.loads(doc.BASELINE.read_text())
    per_app, bundle = doc.per_application(baseline, "voxel_smooth")
    case = next(c for c in baseline["cases"] if c["name"] == "voxel_smooth")
    batch = max(m.get("batch", 1) for m in case["measurements"])
    assert batch > 1, "voxel_smooth is a batched case; this test is about that"
    assert per_app == pytest.approx(
        baseline["budgets"]["voxel_smooth"]["measuredMs"] / batch)
    assert bundle == "devicemeasure"


def test_an_unbatched_case_is_quoted_as_measured():
    baseline = json.loads(doc.BASELINE.read_text())
    per_app, _ = doc.per_application(baseline, "sdf_consolidate")
    assert per_app == pytest.approx(
        baseline["budgets"]["sdf_consolidate"]["measuredMs"])


def test_the_gallery_is_allowed_the_spread_it_actually_has():
    """Per-case reliability is not uniform and a single tolerance would either
    nag about the measure bundle or wave the gallery through."""
    assert doc.TOLERANCE["devicegallery"] > doc.TOLERANCE["devicemeasure"]


def test_a_drifted_row_fails(tmp_path):
    """The defect itself: a row quoting a figure the baseline does not carry."""
    text = doc.DOC.read_text()
    line = next(l for l in text.splitlines()
                if doc.ROW.match(l) and "`voxel_smooth`" in l)
    m = doc.ROW.match(line)
    drifted = line.replace(m.group(2), str(float(m.group(2)) * 10), 1)
    scratch = tmp_path / "drifted.md"
    scratch.write_text(text.replace(line, drifted))
    sys.argv = ["check_doc_latency.py", str(scratch)]
    assert doc.main() == 1


def test_every_held_exemption_carries_a_reason():
    """An exemption nobody can read is an omission nobody noticed — the rule
    check_device_coverage.py applies to its own.

    HELD is empty today. `sdf_move` sat in it for one release while
    layer_move_surface was 1.5x slower than the figure its baseline held as the
    target (#358); the fix landed and the row quotes the baseline again.
    """
    for name, reason in doc.HELD.items():
        assert reason.strip(), f"{name} is held with an empty reason"
        assert len(reason) > 40, f"{name}'s reason says too little to act on"


def test_a_held_row_is_exempt_from_the_ratio(tmp_path, monkeypatch):
    """A held row may quote a figure the baseline does not carry."""
    text = doc.DOC.read_text()
    line = next(l for l in text.splitlines()
                if doc.ROW.match(l) and "`voxel_smooth`" in l)
    m = doc.ROW.match(line)
    drifted = line.replace(m.group(2), str(float(m.group(2)) * 10), 1)
    scratch = tmp_path / "held.md"
    scratch.write_text(text.replace(line, drifted))
    sys.argv = ["check_doc_latency.py", str(scratch)]
    assert doc.main() == 1                                   # drifted, and checked
    monkeypatch.setattr(doc, "HELD", {"voxel_smooth": "held for a reason " * 4})
    assert doc.main() == 0                                   # drifted, and held


def test_a_held_row_that_vanished_fails(tmp_path, monkeypatch):
    """An exemption for a row that no longer exists stops applying silently."""
    monkeypatch.setattr(doc, "HELD", {"a_case_no_row_names": "gone " * 12})
    sys.argv = ["check_doc_latency.py"]
    assert doc.main() == 1

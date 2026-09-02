"""A frame share is about one dab, so a batched case must be divided by its batch.

`INTERACTIVE_FRAME_SHARE_MS` is half a 120 Hz frame: the engine's share of the
time a hand waits for a single application of a verb. Most interactive cases in
this suite do not time a single application. They time 128 of them, or 24, or 8,
because a figure under 0.125 ms cannot be objected to at any ratio (#337) and
batching is what puts the number where the tolerance means something.

Comparing that batched total against half a frame asks a 128-dab drag to fit in
one frame, which is not a claim anyone made. It fired on real cases: on the
2026-09-02 gate `sdf_stroke_smooth_bricks` was reported as missing a frame share
at 4.373 ms while docs/09 quoted the same case, correctly, at 0.182 ms — because
check_doc_latency.py has divided by the batch since the table existed and this
tool did not. Two tools, one case, a 24x disagreement.
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))

bench = pytest.importorskip("check_device_bench")


def case(*points):
    """A case from (p95Ms, batch) pairs."""
    return {"measurements": [{"p95Ms": ms, "batch": b} for ms, b in points]}


def test_an_unbatched_case_is_unchanged():
    """The three sdf transform cases are batch 1 and must still be reported."""
    for peak in (13.997, 28.563, 14.974):
        c = case((peak, 1))
        assert bench.case_batch(c) == 1
        assert bench.frame_share_peak(c) == pytest.approx(peak)
        assert bench.frame_share_peak(c) > bench.INTERACTIVE_FRAME_SHARE_MS


def test_a_measurement_with_no_batch_key_counts_as_one():
    c = {"measurements": [{"p95Ms": 9.0}]}
    assert bench.case_batch(c) == 1
    assert bench.frame_share_peak(c) == pytest.approx(9.0)


def test_the_two_false_alarms_this_was_found_by_stop_firing():
    """Both from the 2026-09-02 gate, both batch 24, both well inside a frame."""
    smooth = case((4.373, 24))
    in_group = case((22.597, 24))
    assert bench.frame_share_peak(smooth) == pytest.approx(0.182, rel=1e-2)
    assert bench.frame_share_peak(in_group) == pytest.approx(0.942, rel=1e-2)
    for c in (smooth, in_group):
        assert bench.frame_share_peak(c) < bench.INTERACTIVE_FRAME_SHARE_MS


def test_a_batched_case_that_genuinely_misses_a_frame_still_fires():
    """dyntopo_stamp_fine: 21.572 ms over batch 4 is 5.39 ms for ONE dab.

    The point of dividing is not to silence the note. This case is the reason
    the fine variant exists — a host that lets an artist raise detail is asking
    for a dab that does not fit a frame — and it must survive the fix.
    """
    fine = case((20.643, 4), (20.979, 4), (21.572, 4))
    assert bench.frame_share_peak(fine) == pytest.approx(5.393, rel=1e-3)
    assert bench.frame_share_peak(fine) > bench.INTERACTIVE_FRAME_SHARE_MS


def test_the_adaptive_dab_is_not_reported_once_it_is_divided():
    """dyntopo_stamp: 5.033 ms over batch 8 is 0.63 ms, comfortably inside."""
    stamp = case((3.993, 8), (4.090, 8), (5.033, 8))
    assert bench.frame_share_peak(stamp) == pytest.approx(0.629, rel=1e-2)
    assert bench.frame_share_peak(stamp) < bench.INTERACTIVE_FRAME_SHARE_MS


def test_the_widest_batch_on_the_axis_is_the_divisor():
    """A case whose points disagree divides by the batch that overstates least."""
    assert bench.case_batch(case((1.0, 4), (1.0, 16), (1.0, 8))) == 16

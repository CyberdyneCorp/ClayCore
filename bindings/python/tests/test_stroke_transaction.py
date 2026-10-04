# A stroke resolved as it arrives, through pyclay (#670).
#
# StrokeTransaction is the C ABI's clay_stroke_tx, and this is the harness a
# host's parity test runs on: the same path appended in batches of 1, 5, 40 and
# an uneven schedule resolves, once ended, to exactly StrokePreset.resolve of
# the whole path -- for every preset field, bit for bit.

import numpy as np
import pytest

import pyclay as clay


def wandering_path(n=80):
    """(N, 8) samples with every channel moving."""
    rows = []
    x = -1.0
    for i in range(n):
        x += 0.02 + 0.015 * (i % 3)
        rows.append([x, 0.15 * np.sin(i * 0.3), 0.05 * np.cos(i * 0.17),
                     0.35 + 0.6 * ((i * 7) % 11) / 10.0, 0.1 * (i % 5), 0.2 * i,
                     0.5 + 0.25 * (i % 4), 0.004 * i])
    return np.array(rows, np.float32)


def preset(**fields):
    p = clay.StrokePreset()
    p.radius = 0.08
    p.spacing = 0.25
    for name, value in fields.items():
        setattr(p, name, value)
    return p


PRESETS = [
    {},
    {"taper_end": 0.3},
    {"taper_start": 0.2, "taper_end": 0.2},
    {"steady": 0.6},
    {"jitter_position": 0.3, "jitter_size": 0.4, "jitter_rotation": 0.8, "seed": 11},
    {"rotate_along_stroke": True},
    {"rotate_to_azimuth": True},
]


def schedules(n):
    out = [[n]]
    for size in (1, 5, 40):
        out.append([min(size, n - d) for d in range(0, n, size)])
    rng = np.random.default_rng(7)
    uneven, done = [], 0
    while done < n:
        size = int(min(rng.integers(1, 12), n - done))
        uneven.append(size)
        done += size
    out.append(uneven)
    return out


@pytest.mark.parametrize("fields", PRESETS)
def test_batches_resolve_to_the_whole_path(fields):
    samples = wandering_path()
    p = preset(**fields)
    whole = p.resolve(samples)
    assert len(whole["radii"]) > 10
    for schedule in schedules(len(samples)):
        tx = clay.StrokeTransaction(p)
        done, grown = 0, 0
        for size in schedule:
            new, revised = tx.append(samples[done:done + size])
            done += size
            grown += new
            assert revised <= tx.status()["stamps"]
        tx.end()
        got = tx.stamps()
        for key in ("positions", "radii", "strengths", "along"):
            assert np.array_equal(got[key], whole[key]), key
        status = tx.status()
        assert status["ended"] and status["settled"] == status["stamps"] == grown
        assert status["samples"] == len(samples)


def test_settled_stamps_are_final_and_the_end_taper_waits():
    samples = wandering_path()
    p = preset(taper_end=0.3)
    whole = p.resolve(samples)
    tx = clay.StrokeTransaction(p)
    tx.append(samples[:40])
    settled = tx.status()["settled"]
    assert 0 < settled < tx.status()["stamps"]
    # What is settled now is the finished stroke's, already.
    assert np.array_equal(tx.stamps()["radii"][:settled], whole["radii"][:settled])
    tx.end()
    with pytest.raises(ValueError):
        tx.append(samples[40:])


def test_a_start_taper_settles_nothing_before_the_end():
    samples = wandering_path()
    tx = clay.StrokeTransaction(preset(taper_start=0.2))
    tx.append(samples[:60])
    assert tx.status()["settled"] == 0
    tx.end()
    assert tx.status()["settled"] == tx.status()["stamps"] > 0

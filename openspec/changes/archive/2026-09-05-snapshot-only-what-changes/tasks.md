# Tasks: snapshot only what a pass will overwrite

## 1. The snapshot

- [x] 1.1 `FieldVolume::snapshot_region(region)` copies the bricks
      `rewrite_region` selects for the SAME region, and nothing else.
- [x] 1.2 Reads outside them are served from the volume itself. Correct for the
      reason the region limit is: an unselected brick is never written, so it is
      still its own "before".
- [x] 1.3 The brick range is computed by ONE function both it and
      `rewrite_region` call. They must not disagree about which bricks are in
      play, since the snapshot is only correct for the ones the rewrite writes.
- [x] 1.4 The ordering precondition is on the class, as a requirement with its
      failure mode: snapshot, then rewrite the same region — a snapshot of a
      different region reads half-written bricks and is silently wrong.

## 2. The read path, which is the whole difference

- [x] 2.1 The canonical brick first: every coordinate has exactly one whose
      local index is inside `[0, kBrickDim)`, and a stencil walking a region hits
      it every time.
- [x] 2.2 The eight-way search for shared face samples survives underneath, and
      the fall-through to the volume below that.
- [x] 2.3 VERIFIED why: the obvious version — eight candidates before the
      canonical one — was CORRECT and 4% SLOWER over a stroke than the copy it
      replaced. A radius-1 stencil is seven taps a sample and a dab is a few
      hundred thousand of them, so 0.3 ns each outweighs one 0.162 ms copy. The
      comment says so, because nothing about the design does.

## 3. Measured

- [x] 3.1 A 24-dab stroke, cell 0.01: first dab 2.96 -> 2.31 ms (1.28x), steady
      dab 1.81 -> 1.60 ms (1.13x).
- [x] 3.2 Cell 0.02: level. The copy was already small there, and so is the
      locality difference — 1.5 MB against six.
- [x] 3.3 The gain exceeds the copy it removes (9% removed, 11% gained) and the
      difference is the read path: a snapshot is a few hundred kilobytes and
      stays in cache where a tap walks a sparse index into six megabytes.

## 4. Tests

- [x] 4.1 The invariant directly: a snapshot taken of a region, used while THAT
      region is rewritten, reads the volume as it stood beforehand — everywhere,
      not only inside the region.
- [x] 4.2 The region is rewritten to a CONSTANT, so a snapshot reading through
      to the live volume by mistake comes back with the constant rather than the
      original rather than merely looking plausible.
- [x] 4.3 Four regions per cell size, off the brick lattice on purpose — an
      aligned box never puts a snapshotted and an un-snapshotted brick either
      side of a shared face sample, which is what the slow path is for — plus
      covers-everything and meets-nothing.
- [x] 4.4 Full unit suite 1420, `clay_c_smoke` OK, `check_bench.py` OK.

## 5. Not in this change

- [ ] 5.1 Ping-pong buffers, #278's alternative. They avoid the allocation and
      not the copy; the sample array is still duplicated per pass.
- [ ] 5.2 Threading a pass. Bricks share face samples, so parallel writes need
      an ownership rule, and no measurement is asking for it yet.

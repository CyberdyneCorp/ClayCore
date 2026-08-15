# Tasks: twist and bend, confined to a span

## 1. The kernels

- [x] 1.1 `ctwist_range_point` and `cbend_range_point` — the same rotations with
      the angle ramped across a span and held beyond, written as the unranged
      form with the angle substituted.
- [x] 1.2 Two opcodes appended to `CDeformType` (14, 15), so no existing
      serialized value moves, plus their dispatch. Both fit the existing slot
      budget: k, t0, t1 and the ease slot.
- [x] 1.3 `Deformer::twist_range` / `bend_range`.

## 2. Bounds and exactness

- [x] 2.1 The AABB hull reuses the unranged cases — a bounded rotation is
      contained by the unbounded one's cylinder.
- [x] 2.2 The Lipschitz is charged `k * ease_max_slope`, the convention `grab`,
      `pose` and `magnify` already follow, because an eased ramp is steeper in
      the middle than its average rate.

## 3. Bindings

- [x] 3.1 `CLAY_DEFORM_TWIST_RANGE` / `_BEND_RANGE`, parameter counts, and the
      static asserts that keep the C enum pinned to the tape opcode.
- [x] 3.2 A zero-width span refused on both sides, with the same message shape
      `bend_radial`'s band already uses.
- [x] 3.3 `p.twist_range(...)` / `p.bend_range(...)` in pyclay.
- [x] 3.4 `check_binding_parity.py` passes (325 capabilities).

## 4. Evidence

- [x] 4.1 Equivalence: ranged with a linear ease over its whole span equals the
      unranged form, for twist and for bend.
- [x] 4.2 Holding: two points above the span rotate identically, where the
      unranged twist keeps winding — and the two ends differ, so the range is
      doing something.
- [x] 4.3 The ranged pair added to the house `check_conservative_steps` loop
      with an EASED ramp, which is the case that catches a bound taken from the
      average rate.
- [x] 4.4 A steeper ease reports a tighter step scale, which is what says the
      ease reaches the bound at all.
- [x] 4.5 Parity corpus scene with a non-linear ease.
- [x] 4.6 `examples/03_deformers.py` — its coverage gate reads the warps off the
      binding, so a new one shows up as a gap; both are covered, and the render
      shows a column straight at both ends with the twist confined to the middle.

## 5. Docs

- [x] 5.1 `docs/07` deformer table, ZBrush-equivalent rows (including that
      Gizmo Lattice is still absent and why), and the reachability table.
- [x] 5.2 The stale counts: README, ROADMAP and `sculpt_comparison` said 14
      deformers; `build-packaging` and `docs/06` said the fixture exercised 5 of
      14. Recounted from the source rather than incremented: 7 of 16.

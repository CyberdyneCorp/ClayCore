# What a gate covers — and how to tell, before a user tells you

[09](09-brush-latency-and-coverage.md) is an inventory: which brushes have a
test, a device case and a committed render. This is the other question, and it
is the one that has actually cost this engine working features:

> A test exists, it sets the parameter, and it is green. **Does it cover
> anything?**

It exists because the answer was no four times in one audit (issue #596), and
because the first framing of that audit was itself wrong twice. Both of those
are recorded below, because the corrections are the useful part.

## The rule

**For every field a parameters struct carries, name the observable quantity it
controls, then check that something asserts on that quantity — on every path the
field travels.**

Three of those words do the work.

**QUANTITY.** A gate that compares distances can only catch parameters whose
effect is on distances. `clay_volume_params.feather` shapes the NORMALS across a
replace boundary and does not move the zero set at all, so no distance
comparison can ever see it. `bindings/c/clay.h` says so in the field's own
documentation, citing the issue the feather was built to fix:

> min/max branch switching between two fields that touch is what corrugates the
> normals at the cell wavelength (issue #67) — **the zero set is exact, the
> shading is not** — while the box faces meet the outside field at a hard edge.

That sentence sits at `bindings/c/clay.h:4692`, and it was there before a host reported the feather
being silently dropped (#593). The engine knew which quantity the field
controlled; the suite did not.

**EVERY PATH.** This is the half that is easy to believe you have done.

- A normal-angle gate for `feather` already existed —
  `tests/unit/test_c_volume.cpp:793`, added in the same commit as the feather.
  It covers the PLACEMENT, a feather set on a bake. Nothing covered the VERBS
  that rebuild a volume, and `move_topological` rebuilds by re-sampling through
  a callable that knows nothing about the volume it came from. The existing gate
  could not have fired, because it never runs a verb (#597).
- A surface gate for `gesture_id` existed too (#598) — driving
  `clay_layer_move_surface`. `clay_sdf_move_begin` is the other door, and it
  dropped the field for three releases (#603).

Two different shapes of the same mistake, two weeks apart. **Covering a field's
effect on one path it travels is not covering the field.**

**ASSERTS ON.** See the next section, which is the failure that hides best.

## A-vs-A: the test that moves with the defect

The most common false positive is a test that *sets* the field and asserts that
two paths **agree with each other**. Drop the field and both sides move
together, stay equal, and the test stays green. It is a wiring check, and a
wiring check is worth having — it is simply not coverage.

Four of these were found in one audit. One says so in its own comment:

```
// The automask the sculptor ended up holding IS the estimator, so asking it
// and asking `measure_at` directly must give the same number. Not close —
// the same call.
```
— `tests/unit/test_mesh_sculpt.cpp:2019`, on `clay_measure_params.h`

The others:

| site | what makes it A-vs-A |
|---|---|
| `test_c_move_brush.cpp:1007` | compares two drags that **both pass `gesture_id` 0** |
| `test_mask_extrude.cpp:266` | sets `border_smooth` as a vehicle for a *non-consumption* check |
| `test_parity.cpp` | asserts gradient direction, but hands the **same compiled tape** to both sides |

That last one is worth knowing before anyone cites it as derivative coverage. It
catches backend disagreement, which is its job, and is structurally blind to any
defect upstream of the tape — including a feather dropped before compile.

**How to spot one:** ask what the test would do if the field were deleted from
the struct. If both sides of its comparison change identically, it will pass.

## Mutate before you write the gate

A claimed coverage gap is a **hypothesis**. What makes it a finding is a
mutation narrow enough to sit inside the claimed gap and nowhere else.

```
delete the parameter's effect  ->  run the WHOLE suite  ->  count failing CASES
```

- **Zero failures** proves the gap. Three of the four #596 gaps failed **zero**
  of ~16.6 M assertions before their gate existed.
- **Failures in existing tests** prove the opposite: the path is covered and the
  claim needs narrowing. A coarse mutation of `moved_chain` tripped SIX existing
  cases; the surgical one, confined to the named-gesture path, failed exactly
  one — the new test. Only the second measurement says anything about the hole.

Do this **before** writing the gate, and certainly before writing the commit
message. A claim in a merged message is expensive to walk back.

## Write a check that can fail

A bar nothing can cross is not a gate, and this repository has shipped several:
a parity row that passes vacuously when a backend is absent (#578), a bindings
gate that compared source to itself. Where a fixture has a known-bad
configuration, **assert that it crosses the bar**, in the same test:

```cpp
CHECK(ratio < 1.30);        // the cure
CHECK(hard_ratio > 1.30);   // and the defect, so the bar is known crossable
```

If the second half ever fails while the first passes, the metric has gone blind
and the first is no longer evidence of anything.

## Choosing the statistic is part of the work

Picking *what to measure* decided more than the implementation did, when a
mesh-level roughness gate was calibrated for the feather
(`benchmarks/feather_roughness_probe.cpp`). Ratios of a hard replace against the
same shape feathered:

| statistic | ratio | why |
|---|---|---|
| mean | 1.05x | **dilutes** — the defect is a seam of ~28 k edges in 422 k |
| **p99** | **1.87x** | separates; what the fixture uses |
| p99.9, max | 1.00x | **saturated** — the control already carries 90° edges |
| seam-local mean | 0.80x | **inverted** — see below |

The seam-local mean was the *intended* design. A spatial shell around the
replace box also selects where the surface runs tangent to the lattice, which is
rougher on the CONTROL too (1.19° against 0.78° overall), so it reported the
defect as an **improvement**. Localise by the field, or not at all.

**Report a ratio, not a quantity.** An absolute roughness belongs to the
tessellation, the cell size and the marcher. The ratio against the same shape
through the same path with the defect absent belongs to the defect — which is
what lets one bar hold across backends whose triangle counts differ by an order
of magnitude.

## The two corrections this audit needed

Recorded because the method is what survives, and both were caught by the method
rather than by care.

**The premise was too strong.** #596 was opened as "every gate compares
distances". False — `test_c_volume.cpp:793` gates on normal angle and predates
the issue. The corrected statement ("every path the field travels") is narrower
and more useful, and it is what caught #603.

**An adversarial verifier found it, and a gap-finder would not have.** The audit
ran one agent per candidate gap instructed to **refute** it and to default to
"refuted" when unsure. That is the stage that surfaced the 2026-08-11 test. A
pass told to find gaps confirms your own framing back to you.

## Checklist

When adding or reviewing a parameter:

- [ ] Name the observable quantity it controls: distance, normal/derivative,
      extent, topology, attribute, cost.
- [ ] List every entry point the field travels through. C ABI and C++ both.
- [ ] For each, is there a test asserting on **that quantity** — not merely
      exercising the field?
- [ ] Would that test pass if the field were deleted? (A-vs-A check.)
- [ ] Delete the field's effect and run the whole suite. Count failing cases.
- [ ] Does the new gate's bar have a configuration that crosses it, asserted in
      the same test?
- [ ] Is the measurement a ratio against a control, rather than an absolute?

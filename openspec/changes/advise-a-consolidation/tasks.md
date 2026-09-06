## 1. The derivation, in the scene layer

- [ ] 1.1 `scene::consolidation_advice(const Layer&, float advise_below_step_scale)`
      beside `report_layer` in `include/clay/scene/consolidate.h`, returning the
      params, the projected cost and the verdict together — one function, so the
      verdict and the numbers it is keyed on cannot drift
- [ ] 1.2 Derive from the LOCAL frame: the `tape.bounds` of `local_view(layer)`,
      the same box `bake_tape_with` samples. NOT `clay_layer_bounds`, which
      composes the layer transform and is wrong by the layer's scale — and wrong
      invisibly on every layer at identity
- [ ] 1.3 `F = min` over drawable nodes of (`K * cell_size` for a volume node,
      the smallest axis of `item_geometry_bound` otherwise); `cell = clamp(F/K,
      E/512, E/32)`; `band = 3*cell`; `padding = band`; `skip_redistance = 0`
- [ ] 1.4 A layer whose finest content is a volume at `c` is advised exactly `c`
      — assert the identity, it is the whole answer to "a number nobody chose"
- [ ] 1.5 MEASURE `K`. Sweep 2, 4, 8 on a fixture whose smallest item is a small
      dab on a large form, report surface agreement against the parametric
      field, and if 4 is wrong CHANGE IT and write what it measured into
      `design.md`. Same for the `512`/`32` clamp if a real layer lands outside it

## 2. The verdict is keyed on the projection

- [ ] 2.1 Advised only when `report_layer` advises at the same threshold AND the
      projected `safe_step_scale` reaches that threshold
- [ ] 2.2 A threshold above `1/sqrt(3) = 0.577` is never advised, because no
      redistanced volume can reach it — case it explicitly rather than leaving
      it to fall out
- [ ] 2.3 VERIFY the projection equals what the layer reports after the bake, on
      a MIRRORED and a NON-UNIFORMLY-SCALED layer. If they differ, key the
      verdict on the number that actually results and record the difference in
      `design.md`
- [ ] 2.4 The sampling runs even when the cost is not wanted; there is no cheap
      arm, because the verdict is defined by the projection

## 3. The C entry point

- [ ] 3.1 `clay_layer_consolidation_advice` in `bindings/c/clay.h` /
      `clay_c.cpp`. Purely additive: no existing signature or struct layout
      moves, and `clay_consolidation_params.cell_size` stays required and > 0
- [ ] 3.2 Not advised => `out_params` and `out_cost` ZEROED to the caller's
      declared `struct_size`, `struct_size` preserved. `cell_size == 0` is
      already refused downstream, so ignoring `*out_advises` fails loudly
- [ ] 3.3 Refusals, before any sampling: `CLAY_ERROR_NOT_FOUND` for an unknown
      layer; `CLAY_ERROR_INVALID_ARGUMENT` for `advise_below_step_scale <= 0`,
      for a null `out_params` or `out_advises`, and for a `struct_size` below
      the original layout
- [ ] 3.4 Non-SDF, protected, empty: success, not advised, zeroed — a host
      walking a mixed stack should not special-case them
- [ ] 3.5 Header documentation states, beside the call: the derivation and its
      three constants; that it does NOT bake and does NOT sever an instance's
      sharing; that the advice is not optimal, not stable across edits, not a
      memory bound, not a fidelity claim, and carries no pinned region; that
      NULL `out_cost` is NOT a fast path; and that a stepping-bound Lipschitz
      was REJECTED as the source of the resolution, with the reason
- [ ] 3.6 `clay_field_report.advises_consolidation`'s own comment points at this
      call as what to do with the flag

## 4. pyclay follows

- [ ] 4.1 `Layer.consolidation_advice(advise_below_step_scale)` -> an object with
      `advises`, `params`, `cost`
- [ ] 4.2 `params` and `cost` are `None` when not advised — the Pythonic form of
      the zeroed struct: passing `None` to `Layer.consolidate` raises at the
      call rather than baking
- [ ] 4.3 `ALIASES` entry if the name collides with a struct field, and
      `check_binding_parity.py` green with `--pyclay <build>/bindings/python
      --require-import`. READ THE LINE IT PRINTS: `imported <path>` is a real
      check, `parsed bindings/python/pyclay_module.cpp` is not

## 5. Gates

- [ ] 5.1 THE PROPERTY: a layer that `clay_layer_field_report` advises at a
      threshold, consolidated with the params THIS call returned, comes back
      from a second report with `advises_consolidation == 0`, `degradation ==
      CLAY_DEGRADATION_NONE`, and `safe_step_scale` at or above the threshold
- [ ] 5.2 A `CLAY_DEGRADATION_DEFORMERS` layer is not advised and its params are
      zeroed — #387's case, one step further along
- [ ] 5.3 The document is byte-identical across the call, and an instance layer
      still reports its link through `clay_document_layer_info` afterwards
- [ ] 5.4 An older caller passing the earlier `struct_size` for either
      descriptor gets its own fields and nothing written past its end
- [ ] 5.5 Round trip: advised params fed to `clay_layer_consolidation_cost`
      quote the same `brick_count` and `bytes` the advice reported
- [ ] 5.6 Unit suite, pyclay suite, and `python3 tools/release_check.py
      --skip-slow` green
- [ ] 5.7 Per-function cognitive complexity within the backend target of 15; if
      the derivation is genuinely irreducible, say so with its score rather than
      splitting it into functions nobody can follow
- [ ] 5.8 **ABI 0.85.0 -> 0.86.0** in `CMakeLists.txt`, `bindings/c/clay.h`
      (`CLAY_ABI_MAJOR`/`MINOR`/`PATCH`) and `pyproject.toml`. This change adds
      an entry point, so the minor moves in the PR that adds it
- [ ] 5.9 `docs/05` gains the call
- [ ] 5.10 CI green

## 6. Settled, and what stays open

- [ ] 6.1 ROADMAP #15 is settled as a recommendation, not an autonomous action.
      The ROADMAP edit is NOT part of this change
- [ ] 6.2 Still open, deliberately: a region-scoped advice. This advises a WHOLE
      layer, and `clay_layer_consolidate_region` is what a sculptor working a
      patch actually wants. Deriving a cell size for a closure means deriving it
      for a box the caller chose, which is a different question with a different
      wrong answer, and it needs its own measurement

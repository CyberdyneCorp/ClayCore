## 1. The derivation, in the scene layer

- [x] 1.1 `scene::consolidation_advice(const Layer&, float advise_below_step_scale)`
      beside `report_layer` in `include/clay/scene/consolidate.h`, returning the
      params, the projected cost and the verdict together — one function, so the
      verdict and the numbers it is keyed on cannot drift
- [x] 1.2 Derive from the LOCAL frame: the `tape.bounds` of `local_view(layer)`,
      the same box `bake_tape_with` samples. NOT `clay_layer_bounds`, which
      composes the layer transform and is wrong by the layer's scale — and wrong
      invisibly on every layer at identity
- [x] 1.3 `F = min` over drawable nodes of (`K * cell_size` for a volume node,
      the smallest axis of `item_geometry_bound` otherwise); `cell = clamp(F/K,
      E/512, E/32)`; `band = 3*cell`; `padding = band`; `skip_redistance = 0`
- [x] 1.4 A layer whose finest content is a volume at `c` is advised exactly `c`
      — assert the identity, it is the whole answer to "a number nobody chose"
- [x] 1.5 MEASURE `K`. Sweep 2, 4, 8 on a fixture whose smallest item is a small
      dab on a large form, report surface agreement against the parametric
      field, and if 4 is wrong CHANGE IT and write what it measured into
      `design.md`. Same for the `512`/`32` clamp if a real layer lands outside it

## 2. The verdict is keyed on the projection

- [x] 2.1 Advised only when `report_layer` advises at the same threshold AND the
      projected `safe_step_scale` reaches that threshold
- [x] 2.2 A threshold above `1/sqrt(3) = 0.577` is never advised, because no
      redistanced volume can reach it — case it explicitly rather than leaving
      it to fall out
- [x] 2.3 VERIFY the projection equals what the layer reports after the bake, on
      a MIRRORED and a NON-UNIFORMLY-SCALED layer. If they differ, key the
      verdict on the number that actually results and record the difference in
      `design.md`
- [x] 2.4 The sampling runs even when the cost is not wanted; there is no cheap
      arm, because the verdict is defined by the projection

## 3. The C entry point

- [x] 3.1 `clay_layer_consolidation_advice` in `bindings/c/clay.h` /
      `clay_c.cpp`. Purely additive: no existing signature or struct layout
      moves, and `clay_consolidation_params.cell_size` stays required and > 0
- [x] 3.2 Not advised => `out_params` and `out_cost` ZEROED to the caller's
      declared `struct_size`, `struct_size` preserved. `cell_size == 0` is
      already refused downstream, so ignoring `*out_advises` fails loudly
- [x] 3.3 Refusals, before any sampling: `CLAY_ERROR_NOT_FOUND` for an unknown
      layer; `CLAY_ERROR_INVALID_ARGUMENT` for `advise_below_step_scale <= 0`,
      for a null `out_params` or `out_advises`, and for a `struct_size` below
      the original layout
- [x] 3.4 Non-SDF, protected, empty: success, not advised, zeroed — a host
      walking a mixed stack should not special-case them
- [x] 3.5 Header documentation states, beside the call: the derivation and its
      three constants; that it does NOT bake and does NOT sever an instance's
      sharing; that the advice is not optimal, not stable across edits, not a
      memory bound, not a fidelity claim, and carries no pinned region; that
      NULL `out_cost` is NOT a fast path; and that a stepping-bound Lipschitz
      was REJECTED as the source of the resolution, with the reason
- [x] 3.6 `clay_field_report.advises_consolidation`'s own comment points at this
      call as what to do with the flag

## 4. pyclay follows

- [x] 4.1 `Layer.consolidation_advice(advise_below_step_scale)` -> an object with
      `advises`, `params`, `cost`
- [x] 4.2 `params` and `cost` are `None` when not advised — the Pythonic form of
      the zeroed struct: passing `None` to `Layer.consolidate` raises at the
      call rather than baking
- [x] 4.3 `ALIASES` entry if the name collides with a struct field, and
      `check_binding_parity.py` green with `--pyclay <build>/bindings/python
      --require-import`. READ THE LINE IT PRINTS: `imported <path>` is a real
      check, `parsed bindings/python/pyclay_module.cpp` is not

## 5. Gates

- [x] 5.1 THE PROPERTY: a layer that `clay_layer_field_report` advises at a
      threshold, consolidated with the params THIS call returned, comes back
      from a second report with `advises_consolidation == 0`, `degradation ==
      CLAY_DEGRADATION_NONE`, and `safe_step_scale` at or above the threshold
- [x] 5.2 A `CLAY_DEGRADATION_DEFORMERS` layer is not advised and its params are
      zeroed — #387's case, one step further along
- [x] 5.3 The document is byte-identical across the call, and an instance layer
      still reports its link through `clay_document_layer_info` afterwards
- [x] 5.4 An older caller passing the earlier `struct_size` for either
      descriptor gets its own fields and nothing written past its end
- [x] 5.5 Round trip: advised params fed to `clay_layer_consolidation_cost`
      quote the same `brick_count` and `bytes` the advice reported
- [x] 5.6 Unit suite, pyclay suite, and `python3 tools/release_check.py
      --skip-slow` green
- [x] 5.7 Per-function cognitive complexity within the backend target of 15; if
      the derivation is genuinely irreducible, say so with its score rather than
      splitting it into functions nobody can follow
- [x] 5.8 **ABI 0.85.0 -> 0.86.0** in `CMakeLists.txt`, `bindings/c/clay.h`
      (`CLAY_ABI_MAJOR`/`MINOR`/`PATCH`) and `pyproject.toml`. This change adds
      an entry point, so the minor moves in the PR that adds it. All three lines
      moved together in the reconciling commit, which owns them for the four
      entry points this PR adds
- [x] 5.9 `docs/05` gains the call — "What to bake at, and whether to bake at
      all (ABI 0.86.0)" in section 3, beside the paragraph on what a bake costs
      the march, plus `layer.consolidation_advice` in the pyclay section
- [ ] 5.10 CI green

## 6. Settled, and what stays open

- [x] 6.1 ROADMAP #15 is settled as a recommendation, not an autonomous action.
      The ROADMAP edit is NOT part of this change
- [x] 6.2 Still open, deliberately: a region-scoped advice. This advises a WHOLE
      layer, and `clay_layer_consolidate_region` is what a sculptor working a
      patch actually wants. Deriving a cell size for a closure means deriving it
      for a box the caller chose, which is a different question with a different
      wrong answer, and it needs its own measurement

## What landed, and where the numbers are

- **1.3 amended by measurement.** The analytic arm reads `item_local_bounds`,
  not `item_geometry_bound`: that box is dilated by rounding and blend support
  and advised the K = 2 grid while claiming K = 4. Both arms now carry their
  length into the bake's frame with `placed_distance_scale`. `design.md`,
  "What building it found" §1 and §2.
- **1.5 measured, 4 stands.** 576 rays against the parametric field on a 0.06
  dab blended onto a unit form: 27.2% / 7.0% / 1.8% of the dab's radius at K =
  2 / 4 / 8, for 0.32 / 1.42 / 5.51 MB. Table in `design.md` §4. The E/512 and
  E/32 clamp was not moved; no fixture landed outside it.
- **2.3 verified, no divergence.** The projected `safe_step_scale` equals the
  `report_layer` taken after consolidating with the advised params to
  0.0000000, on identity, mirrored and `scale_axes = (2, 0.5, 1.3)`. Three
  tests hold it.
- **4.3 needed no `ALIASES` entry** — `Layer.consolidation_advice` resolves to
  `clay_layer_consolidation_advice` by the ordinary prefix rule.
  `check_binding_parity.py --pyclay build/cpu-only/bindings/python
  --require-import` prints `imported .../pyclay.cpython-311-darwin.so`, so it
  is a real check: 740 capabilities, 34 exempt, OK.
- **5.5 round trip** holds in both languages: the advised params fed back to
  `clay_layer_consolidation_cost` quote the same `brick_count` and `bytes`.
- **5.7 cognitive complexity**, clang-tidy
  `readability-function-cognitive-complexity`: `node_feature` 4,
  `smallest_feature` 10, `advised_params` 6, `scene::consolidation_advice` 9,
  `clay_layer_consolidation_advice` 11. All inside the backend target of 15.
  `smallest_feature` measured 18 before `node_feature` was split out of it.
- **5.9 (`docs/05`) and 5.8 (the version lines) are NOT done here.** This
  branch takes **ABI 0.85.0 -> 0.86.0** and this change is the entry point that
  moves the minor, but the bump and the reference page are owned by the
  reconciling pass that closes the branch, so that three files and one docs
  page do not get edited by two hands.

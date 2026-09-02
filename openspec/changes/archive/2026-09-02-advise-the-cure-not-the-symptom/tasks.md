## 1. Report the mechanism

- [x] 1.1 `steepest_deformer_chain` — the deformer mechanism's own factor, so
      there is a number to weigh against `steepest_volume`
- [x] 1.2 `drawable_count` — of `item_count`, the nodes that are evaluated
- [x] 1.3 `Degradation` names which mechanism is costing the marcher

## 2. Key the advice on it

- [x] 2.1 `advises_consolidation` requires an edit list to absorb or a stacked
      volume to redistance, not merely a step scale below the threshold
- [x] 2.2 A threshold of zero still measures without judging

## 3. Carry it to the bindings

- [x] 3.1 The three fields appended to `clay_field_report` behind `struct_size`,
      with `clay_degradation`
- [x] 3.2 `Layer.field_report` gains them, `degradation` as a string

## 4. Proof

- [x] 4.1 A deep chain on ONE item is not advised, and reports `deformers`
- [x] 4.2 The same chain over twenty items IS advised, and reports `both`
- [x] 4.3 A stacked volume is advised, and reports `volumes`
- [x] 4.4 A lone item wrapped in a GROUP is not advised — `item_count` counts
      the group and `drawable_count` does not
- [x] 4.5 The chain's factor separates two chains of equal length
- [x] 4.6 A caller built against the ORIGINAL struct still works, and nothing is
      written past the end of it
- [x] 4.7 The three existing tests that asserted the old advice are updated,
      since they encoded the defect
- [x] 4.8 Each claim fails when the advice is reverted to the step scale alone,
      and keying on `item_count` instead of `drawable_count` fails 4.4

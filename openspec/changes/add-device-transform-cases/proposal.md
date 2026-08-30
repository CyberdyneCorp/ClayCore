# Proposal: the gate has never measured a gizmo drag

## Why

`tests/device/baseline.json` holds 62 cases. Not one of them transforms
anything. The Move/Rotate/Scale gizmo — which an artist reaches for between
every stroke — has no device number, no budget, and no way to regress
visibly.

`tools/check_device_coverage.py` cannot report the absence either: its
`VERB_PATTERNS` list decides what "uncovered" means, and no pattern in it
matches `clay_layer_set_transform` or `clay_document_set_layer_transform`. So
the gap is not even a gap on the record — it is invisible, which is the exact
failure that list's own comment names tube, Trim Curve and the level stack for.

Measured off-device (this Mac, CPU backend, 5832 bricks of 8³ at 0.05 voxel,
one drag frame over a 1000-item blob), against a 4.17 ms frame share:

| drag frame | flat roots | inside one group |
|---|---:|---:|
| the same refill with no edit | 17.4 ms | 11.3 ms |
| node transform + display | 26.8 ms | 89.9 ms |
| layer transform + display | 95.7 ms | 89.8 ms |

Desktop numbers are not device numbers and none of these may be quoted as one.
What they establish is that the case is worth building: the spread between the
three rows is a factor of three, which is a shape a gate can hold.

## What Changes

- Five cases, all on the brick path, because the display half is where the cost
  is and the edit alone is 40-130 us:
  - `sdf_node_transform_bricks` — dragging one item that sits at the layer root;
  - `sdf_group_transform_bricks` — the same item, inside a group holding the
    document, which is how an artist leaves a document worth grouping;
  - `sdf_layer_transform_bricks` — dragging the whole layer's placement;
  - `sdf_stamp_after_drag_bricks` and `sdf_stamp_after_group_drag_bricks` — an
    ordinary stamp that FOLLOWS a drag frame.
- The unit of the first three is a DRAG FRAME: set the placement, dirty, refill.
  A drag is a sequence of independent frames — unlike a stroke, there is no
  chain to break — so those cases need no reset and take none, and the placement
  walks a path rather than alternating between two points, because consecutive
  frames of a real drag overlap and a two-point flip would resume perfectly and
  measure the wrong thing.
- The last two exist because the first three could not see what a wide
  invalidation costs, and finding that out took a device run. A drag frame
  refills what the frame before it dirtied, grouped or not, so the two node rows
  agree to inside the noise. The group-wide seed drop is paid by whatever is
  edited NEXT, so the timed unit there is a stamp with an untimed drag frame in
  the reset ahead of it.
- `VERB_PATTERNS` gains the transform entry points, so a transform verb without
  a case is an error from here on.
- Coverage-table entries for all of them, and an exemption for the per-axis
  placement form.
- Budgets recorded from a run on the reference iPad and written by hand, per
  `device-gate`'s existing rule that a case without a declared budget fails.

## What it measured

Reference iPad (`iPad15,5`, iOS 26.5.2), ABI 0.60.0, CPU, p95, worst axis point
(1000 items), against the 4.17 ms interactive frame share:

| case | p95 | vs frame share | bricks/frame |
|---|---:|---:|---:|
| `sdf_node_transform_bricks` | 14.28 ms | **3.4x over** | 66.9 |
| `sdf_group_transform_bricks` | 13.74 ms | **3.3x over** | 66.9 |
| `sdf_layer_transform_bricks` | 27.03 ms | **6.5x over** | 269.4 |
| `sdf_stamp_after_drag_bricks` | 1.10 ms | inside | 12.6 |
| `sdf_stamp_after_group_drag_bricks` | 4.32 ms | **just over** | 12.5 |

Two readings, and the gate now holds both. A gizmo drag does not fit an
interactive frame at a thousand items, by three to six times. And an ordinary
stamp costs 3.9x more after a drag in a grouped document than after the same
drag in a flat one, at an identical brick count — so what an artist pays for
grouping is not the drag, it is the next thing they do.

Reproducibility: two runs of the three drag cases agreed to within 0.7%, and
the canary held at x1.14 across the run against a x1.3 tolerance. The after-drag
pair was confirmed on an independent run at 3.82x against the 3.91x above; its
absolute figures moved ~9%, which is the position effect this suite already
documents — the pair ran alone and therefore colder — and is why the ratio is
the claim and the budget is a ceiling rather than a target.

## Capabilities

### New Capabilities
None.

### Modified Capabilities
- `device-gate`: what a drag frame is as a timed unit, and that the transform
  verbs are covered-or-exempt like every other verb.

## Impact

- `tests/device/Shared/SceneBuilder.swift` — the drag path, and a grouped
  document.
- `tests/device/Measure/LatencyCases.swift` — the three cases.
- `tests/device/Shared/Coverage.swift` — three entries.
- `tools/check_device_coverage.py` — the verb patterns.
- `tests/device/baseline.json` — three budgets, from a device run.
- `docs/09-brush-latency-and-coverage.md` — the inventory rows.

## Non-goals

**Gating the optimisations.** These cases measure what a drag costs TODAY.
`drag-a-layer-without-a-refill` and `bound-an-edit-by-the-node-it-names` are
what make the numbers smaller; this change is what makes them visible, and it
lands first so that both have a before to be measured against.

**A Metal row.** `sdf_stamp_bricks` records why the brick path is measured on
the CPU — Metal costs 288 us per brick against the CPU's 114 and does not win
at any thread count, because 512 samples is too little work to cover a
dispatch. A transform frame refills the same bricks the same way.

**A mesh-path row.** A host that rasterises pays a remesh instead of a refill.
That is a second axis and needs the per-layer meshing
`drag-a-layer-without-a-refill` adds before it can be measured honestly.

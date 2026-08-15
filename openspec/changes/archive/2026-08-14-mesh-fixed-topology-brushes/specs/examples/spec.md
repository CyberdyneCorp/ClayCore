# examples — the gallery shows what the verbs do

Delta for `mesh-fixed-topology-brushes`.

## ADDED Requirements

### Requirement: The mesh brush gallery
The examples gallery SHALL gain entries covering the fixed-topology mesh brushes, with committed renders, run by `examples/run_all.py` like every other entry.

The M1 verbs SHALL be shown on an imported model AND on a quad-exported re-import, so the claim that a quad mesh survives sculpting is visible rather than asserted.

Each M2 verb SHALL be shown doing the thing its name promises: `clay`'s flat-topped strips against `draw`'s swell, `crease`'s closed fold, `polish` keeping a hard edge while a plain `smooth` at the same settings ruins it, `scrape`'s facet, and `snakehook`'s pull with its stretched triangles left visible rather than hidden.

An entry SHALL show the surface-measured falloff on a shape where the straight-line one is wrong — a brush on one side of a narrow gap that does not reach across it.

An entry SHALL show a mask protecting half a region under one stroke, for a displacement verb and for `smooth`.

An entry SHALL show a stroke undone from its vertex deltas, with the render before and after and the equality stated in the output.

The examples SHALL print the quad and triangle counts before and after, because "topology never changes" is the claim and a number is how a reader checks it.

#### Scenario: The gallery runs
- **WHEN** `python examples/run_all.py` runs
- **THEN** the new entries run to completion and write their renders

#### Scenario: A reader can see the difference between two verbs
- **WHEN** a reader opens the clay and polish renders
- **THEN** the flat-topped strip and the surviving hard edge are visible, each beside the verb it is being distinguished from

## MODIFIED Requirements

### Requirement: A degraded layer reports whether consolidation is its cure

`report_layer` SHALL name what has degraded a layer's safe step scale, and SHALL
advise consolidation only where consolidating is an improvement.

A layer degraded by stacked volumes or by a long edit list SHALL be advised:
the bake absorbs the list and redistances the samples, which is what
consolidation wins back.

A layer of one drawable item carrying a brush chain SHALL be named
`Deformers` — the bake there swaps a cheap analytic item for a dense volume,
measured 6x worse per sample on a real gesture.

**It SHALL nevertheless be advised once its safe step scale has fallen below
the point at which marching the chain costs more than marching the volume**,
which is measured rather than chosen and sits near 0.15 on the fixture in
`benchmarks/move_collapse_crossover_probe.cpp`. Below that floor the per-sample
penalty is repaid by the iterations saved, and the margin grows without bound
while a redistanced volume's own step scale stays constant.

The floor SHALL be a property of the scene model and not the caller's
`advise_below_step_scale`. The caller's threshold answers "is this layer
degraded by my standards"; the floor answers "does the cure apply", and a host
that raises its own tolerance SHALL NOT thereby be advised to bake a layer the
bake would make worse.

#### Scenario: A stack of volumes is advised
- **WHEN** a layer whose steepest volume exceeds 1, or which holds more than one drawable, is reported below the caller's threshold
- **THEN** it is named as degraded by volumes and consolidation is advised

#### Scenario: A shallow brush chain is named but not advised
- **WHEN** a layer of one drawable carrying a brush chain is reported below the caller's threshold, and its safe step scale is above the floor
- **THEN** it is named `Deformers` and consolidation is NOT advised

#### Scenario: A brush chain past the floor is advised
- **WHEN** the same layer's safe step scale falls below the floor
- **THEN** it is still named `Deformers` and consolidation IS advised

#### Scenario: The advice cures what was named
- **WHEN** a layer advised past the floor is consolidated at the advised parameters and reported again
- **THEN** its safe step scale is above the floor, nothing is advised, and no degradation is named

#### Scenario: The caller's tolerance does not move the floor
- **WHEN** a layer carrying a brush chain above the floor is reported at a much larger `advise_below_step_scale`
- **THEN** it is named `Deformers` and consolidation is still NOT advised

#### Scenario: An undegraded layer is neither named nor advised
- **WHEN** a layer whose safe step scale is within the caller's tolerance is reported
- **THEN** no degradation is named and consolidation is not advised

#### Scenario: Reporting changes nothing
- **WHEN** a layer is reported
- **THEN** its items, its field and its consolidation state are what they were

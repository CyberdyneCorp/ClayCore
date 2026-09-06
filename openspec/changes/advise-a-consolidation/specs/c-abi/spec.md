## ADDED Requirements

### Requirement: A host is told what to bake at, not only that it should bake

The C API SHALL expose one call that turns `advises_consolidation` into a
recommendation a host can act on: given a layer and the caller's step-scale
tolerance, it SHALL report a suggested `clay_consolidation_params`, the cost
projected at those params, and whether the bake is advised.

The flag alone is not actionable. `clay_consolidation_params.cell_size` is
required and must be greater than zero, so a host holding the flag must invent a
resolution before it can even ask what the bake would cost — and the sculptor,
who is the only person present, cannot be expected to know what consolidation
means, let alone at what resolution.

The suggested resolution SHALL be derived from things the layer itself carries —
its own extent in the frame the bake samples, and the finest content it already
holds — and SHALL NOT be a constant, a stored document value, or a number
derived from the declared Lipschitz, which is a stepping bound rather than a
bound on the field's slope and therefore does not determine a sampling rate.

`clay_consolidation_params.cell_size` SHALL REMAIN required and greater than
zero. The engine SHALL NOT gain a default resolution, a zero-means-guess mode,
or a resolution stored in the document: the advice is a value written into a
descriptor the caller owns, passes on, edits or discards.

The advice SHALL be keyed on the PROJECTED result and not on the flag alone: it
SHALL be withheld when the projected safe step scale would not reach the
caller's tolerance, because a consolidated layer's step scale is bounded above
by that of a sampled volume, and above that bound the bake trades a parametric
layer for a dense one and still misses the budget.

When the bake is not advised, the params and the cost SHALL be zeroed rather
than left untouched, so that a caller which ignores the verdict and passes the
params on is refused by the next call rather than baking at a resolution nobody
chose.

The call SHALL NOT bake, SHALL NOT change the document, and SHALL NOT sever an
instance layer's shared edit list — asking whether a bake is advisable must
never be the thing that unlinks a subtool, for the same reason asking what one
costs must not.

The documentation beside the call SHALL state what it does not promise: that the
suggested resolution is not optimal and a host with knowledge of its own
viewport can do better; that it is not stable across edits; that it bounds the
grid and not the memory; that it pins no region; and that it is not a cheap
call.

#### Scenario: The advised params lift the layer out of the degraded state
- **WHEN** a host consolidates an advised layer with exactly the params it was advised
- **THEN** a second field report at the same threshold no longer advises consolidation, names no degradation, and reports a safe step scale at or above that threshold

#### Scenario: A tolerance no bake can reach is not advised
- **WHEN** a degraded layer is queried with a step-scale tolerance above what a sampled volume can declare
- **THEN** the call succeeds, advises nothing, and returns zeroed params rather than params that would make the layer worse

#### Scenario: Ignoring the verdict fails loudly
- **WHEN** a host that does not read the verdict passes the returned params straight to a consolidate
- **THEN** that call is refused with an invalid-argument error and the document is unchanged

#### Scenario: A layer whose content already chose a resolution
- **WHEN** the finest content the layer carries is a baked volume
- **THEN** the advised cell size is that volume's own cell size, so a re-bake loses no detail already stored

#### Scenario: Asking does not unlink a subtool
- **WHEN** the advice is asked for a layer sharing its edit list with another
- **THEN** the sharing is intact afterwards and both layers still report the link

#### Scenario: A layer consolidation does not apply to
- **WHEN** the advice is asked for a layer that is not an SDF layer, is protected, or holds nothing
- **THEN** the call succeeds, advises nothing, and returns zeroed params rather than an error, so a host walking a mixed stack does not special-case it

#### Scenario: A layer that is not there
- **WHEN** the advice is asked for a layer that does not exist
- **THEN** the call reports not-found, which is a different answer from "not advised"

#### Scenario: An older caller is unaffected
- **WHEN** the call is made with the struct_size of an earlier descriptor layout
- **THEN** it fills the fields that layout has and writes nothing past its end

## MODIFIED Requirements

### Requirement: The trigger is advisory across the ABI
The C API SHALL let a host measure a layer's degradation and SHALL NOT consolidate on its own. The threshold that turns a measurement into advice SHALL be an argument of the query rather than document state, because a tolerance for marching cost belongs to a viewport, a device and a frame budget rather than to the artwork — and storing it would need a document format bump to carry it.

Advisory SHALL mean actionable, not merely informational: the API SHALL make the
recommendation reachable in one call, complete enough to act on, and SHALL still
require the host to make the call that bakes. Consolidation is destructive and
undoable, so an engine that fired it on a background thread would be mutating a
document behind a host that may be mid-undo-group or mid-save, and would be
discarding parameters on an artist's behalf. Recommending costs the host one
call to ignore; acting costs it a document it did not agree to change.

#### Scenario: A host is told, and decides
- **WHEN** a host asks for a layer's field report with a step-scale threshold
- **THEN** it is told whether the layer has degraded past that threshold, and nothing is baked

#### Scenario: A measurement without a threshold makes no recommendation
- **WHEN** a host asks for a field report with a threshold of zero
- **THEN** it gets the numbers and no advice

#### Scenario: The recommendation is complete enough to act on
- **WHEN** a host receives the advice for a degraded layer
- **THEN** it holds everything the consolidate call requires, and the projected cost of paying it, without having invented a number of its own

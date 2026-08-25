# c-abi

## ADDED Requirements

### Requirement: The full mesh validation report crosses the ABI
The C API SHALL expose every quantity `mesh::ValidationReport` computes, through a versioned output descriptor rather than through individual out-parameters.

The descriptor SHALL carry the vertex and triangle counts, the watertight, manifold and oriented predicates, the boundary-edge, non-manifold-edge, degenerate-triangle, sliver-triangle and intersecting-pair counts, the Euler characteristic, and the derived clean predicate. It SHALL carry a leading `struct_size` and SHALL be filled bounded by the size the caller declares, per the versioned-descriptor rule.

The entry point SHALL accept the sampled self-intersection cap that the engine's validator accepts, so that the self-intersection pass named in the meshing capability is reachable from a binding at all. A cap of zero SHALL skip the pass, matching the engine's own default.

The report SHALL state whether the self-intersection pass ran, by carrying back the cap it was given. A caller SHALL be able to distinguish "no intersecting pairs were found" from "no intersecting pairs were looked for", because both leave the count at zero.

The existing two-boolean entry point SHALL keep working with identical results, defined as sugar over the report, so no consumer is broken.

#### Scenario: A hole is reported, not merely detected
- **WHEN** a mesh with one deleted triangle is validated through the report
- **THEN** the watertight predicate is false AND the boundary-edge count names how many edges are open, rather than the caller learning only that something is wrong

#### Scenario: The self-intersection pass is reachable
- **WHEN** a caller passes a non-zero self-intersection cap
- **THEN** the pass runs, the intersecting-pair count reflects it, and the report shows the cap that was used

#### Scenario: Not tested is distinguishable from none found
- **WHEN** a caller passes a cap of zero
- **THEN** the intersecting-pair count is zero, the reported cap is zero, and the caller can tell the pass did not run

#### Scenario: The old entry point is unchanged
- **WHEN** a caller uses the two-boolean validate entry point
- **THEN** it returns exactly the watertight and manifold values it returned before this change

#### Scenario: The descriptor obeys the versioned-descriptor rule
- **WHEN** a caller declares a `struct_size` below the report's layout, or a value too large to be any descriptor
- **THEN** the call is rejected with `CLAY_ERROR_INVALID_ARGUMENT` rather than reading or writing past the caller's object

#### Scenario: A newer caller's tail is ignored
- **WHEN** a caller declares a `struct_size` larger than this build's layout
- **THEN** the write is clamped to what the build knows, the unknown tail is left untouched, and the size the caller declared is returned unchanged

### Requirement: A mesh reports its volume and area
The C API SHALL expose the signed volume and the surface area of a mesh.

Both SHALL cross as `double`, matching the precision the engine computes them at, because a signed-volume sum over triangles cancels heavily and narrowing it at the boundary would discard the precision the engine chose deliberately.

The signed volume SHALL be positive when triangle normals point outward, so its sign is usable as an orientation check, and SHALL be reported for any mesh rather than refused for one that is not watertight — an open mesh has a divergence-theorem sum, and refusing to state it hides the number a caller uses to notice the mesh is open.

#### Scenario: Volume and area of a closed mesh
- **WHEN** a caller measures a closed, outward-oriented mesh
- **THEN** the signed volume is positive and both figures match the engine's own values

#### Scenario: An inverted mesh reports a negative volume
- **WHEN** a caller measures a closed mesh whose triangles are wound inward
- **THEN** the signed volume is negative, which is what makes it an orientation check

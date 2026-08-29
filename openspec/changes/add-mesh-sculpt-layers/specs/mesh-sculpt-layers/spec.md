# mesh-sculpt-layers

Delta for `add-mesh-sculpt-layers`. A new capability: non-destructive detail
passes on a mesh, and the high-frequency stamping that fills them.

## ADDED Requirements

### Requirement: A sculpt layer is an addressable contribution, not a brush mode
The library SHALL provide a stack of sculpt layers over a multiresolution surface, each with a stable identity, a name, a strength, a visibility flag, a lock and its own detail per level.

A layer's identity SHALL be an opaque id and SHALL NOT be its position in the stack: reordering changes positions, and a host, a serialized document and the C ABI all hold identities across a reorder.

The evaluated surface SHALL be the base plus the base detail plus the sum, over visible layers, of each layer's detail scaled by its strength and its own mask.

A layer SHALL be able to carry detail at more than one level. An anatomy pass belongs at a coarse level and a pore pass at a fine one, and forcing every layer to the finest level wastes the hierarchy the layers sit on.

A layer SHALL be able to carry base deformation at level zero, so that a non-destructive proportion pass is possible and not only a non-destructive detail pass.

A sculpt layer SHALL be distinguished in name and type from the `Layer` brush algorithm, which deposits to a ceiling above the stroke's starting surface and is not a persistent channel. The two SHALL NOT share an unqualified name in any public surface.

#### Scenario: A pass is dialled without being re-sculpted
- **WHEN** a recorded layer's strength is set to 0, then 0.5, then 1
- **THEN** the evaluated surface shows none, half and all of the pass, and no stroke is replayed

#### Scenario: An identity survives a reorder
- **WHEN** layers are reordered and a layer is then addressed by the identity held before the reorder
- **THEN** it names the same layer

#### Scenario: A locked layer refuses a write
- **WHEN** a brush stroke targets a locked layer
- **THEN** the write is refused and the layer's detail is unchanged

### Requirement: Additive layers commute, and the documentation says so
Layer contributions in this stack are additive displacement, so their sum does not depend on their order. Reordering SHALL change organisation and SHALL NOT change the evaluated surface.

This SHALL be stated rather than hidden. Voxel sculpt layers replay cell writes and ARE order-dependent, with a test pinning which order wins; a reader who carries that intuition across will otherwise expect an ordering effect that does not exist. Order dependence SHALL NOT be manufactured to make the two match.

Reordering SHALL remain available: hosts present a stack, later blend modes may not commute, and a procedural layer may read the stack beneath it.

#### Scenario: Reordering two overlapping layers changes nothing
- **WHEN** two layers whose coverage overlaps are swapped
- **THEN** the evaluated surface is bit-identical

### Requirement: A stroke records its full contribution regardless of strength
A stroke on a layer whose strength is less than one SHALL record the contribution it would have made at full strength. Strength SHALL affect evaluation only.

The stored edit is authoritative and the strength knob is composition. The alternative surprises exactly the artist the feature is for: someone who sculpts at low strength and later raises it would find their work underpowered, with no record of how much was lost.

A temporary brush gate SHALL control where a stroke WRITES. A layer's own mask SHALL control where a stored layer CONTRIBUTES. A write SHALL NOT be multiplied by the layer's mask unless the caller explicitly asks for that mode.

#### Scenario: Sculpting at half strength and raising it
- **WHEN** a stroke is made on a layer at strength 0.5 and the strength is then set to 1
- **THEN** the evaluated contribution is twice what was visible during the stroke

### Requirement: Merge and bake preserve the evaluated surface
Merging a layer down and baking a layer into the base SHALL be defined by VISUAL PARITY: the evaluated surface after the operation SHALL equal the evaluated surface before it.

They SHALL NOT be defined by concatenating coefficients. The naive arithmetic scales the upper layer's detail by the ratio of the two strengths and is undefined when the lower layer's strength is zero — which is a state a host reaches with one slider.

Removing a layer SHALL re-evaluate its coverage only. It SHALL NOT replay strokes and SHALL NOT disturb other layers.

Baking SHALL be undoable where history is enabled, and SHALL be a deliberate flattening rather than an implicit consequence of another operation.

#### Scenario: A merge is invisible
- **WHEN** an upper layer is merged into a lower one whose strength is not 1
- **THEN** the evaluated surface is unchanged within the stated tolerance

#### Scenario: A merge with a zero-strength target is still correct
- **WHEN** a layer is merged into a layer whose strength is 0
- **THEN** the evaluated surface is unchanged and the operation does not fail

#### Scenario: Removing a layer leaves the others intact
- **WHEN** a layer overlapping two others is removed
- **THEN** the remaining layers' contributions are unchanged and only the removed layer's coverage re-evaluates

### Requirement: A gesture on a layer is one transaction and one undo step
Writing to a layer SHALL be transactional — begin, stamp, commit, cancel — following the shape the SDF sculpt transaction established.

Cancel SHALL restore the layer exactly. Commit SHALL produce one undo delta for the whole gesture, coalesced so a vertex touched by many stamps appears once.

Under symmetry, every mirrored write SHALL enter the SAME active layer and the same undo step, with the dirty coverage as the union. A mirrored stroke SHALL NOT create a second layer.

Layer PROPERTY operations — rename, strength, visibility, reorder, lock, add, remove, merge, bake — SHALL be undoable. Voxel sculpt-layer property changes are outside the history today; this stack SHALL NOT repeat that.

#### Scenario: A cancelled stroke leaves no trace
- **WHEN** a stroke of many stamps is cancelled
- **THEN** the layer's detail is bit-identical to before the stroke began

#### Scenario: A property change undoes
- **WHEN** a layer's strength is changed and then undone
- **THEN** the previous strength is restored and the evaluated surface matches

### Requirement: High-frequency detail is stamped, not scaled
The library SHALL support stamping detail from a caller-supplied HEIGHT image and from a caller-supplied TANGENT-SPACE VECTOR DISPLACEMENT image, in addition to the existing scalar alpha that scales a brush weight.

The three SHALL be distinguished: an alpha is a weight in [0,1], a height stamp is a signed displacement along a chosen direction, and a vector displacement carries a direction of its own and can fold detail over itself.

Vector displacement SHALL be interpreted in the surface's tangent frame and SHALL NOT be interpreted in world space, which would make one stamp orientation-dependent and unusable over a curved surface.

Image data SHALL be BORROWED for the duration of the call, as the existing mesh alpha already requires. The engine SHALL decode no images.

A stamp SHALL write into detail, leaving the base geometry untouched, when the caller selects that write domain.

The library SHALL report the resolution a level actually supports — mean edge length and the smallest feature that is stable on it — rather than implying that an alpha's pixel count is available as geometry.

#### Scenario: A height stamp shapes the surface
- **WHEN** a height stamp is applied to a layer at a level whose spacing supports it
- **THEN** the evaluated surface carries the stamped relief and the base geometry is unchanged

#### Scenario: A vector stamp rotates with the surface
- **WHEN** the same vector-displacement stamp is applied at two orientations on a curved surface
- **THEN** the displacement follows the local tangent frame in each case

#### Scenario: A stamp finer than the level says so
- **WHEN** a stamp whose features are finer than the active level can represent is applied
- **THEN** the library reports the resolution shortfall rather than silently producing a smoothed result

### Requirement: Layer evaluation costs the coverage, not the surface
Changing a layer's strength, visibility or mask SHALL re-evaluate that layer's COVERAGE and SHALL NOT scan the whole surface. A layer whose coverage is genuinely global is correctly a global operation.

Evaluating the stack SHALL be cached per block against a stack revision, so a stamp on a deep stack does not sum every layer beneath it over unrelated geometry.

Revisions SHALL be separated by kind: renaming a layer is metadata, changing strength or visibility is composition, and a brush write is content. A rename SHALL NOT invalidate geometry caches.

Recording SHALL NEVER stop silently at a memory limit. The library SHALL report layer bytes and let the host merge, bake, delete or compact — a cap that silently stopped recording would leave the pass on the surface and un-dialable, which is a correctness bug wearing a memory limit's clothes.

#### Scenario: A strength change costs the coverage
- **WHEN** the strength of a layer touching a small fraction of a large surface is changed
- **THEN** the vertices re-evaluated are those the layer covers

#### Scenario: A deep stack does not re-sum from zero
- **WHEN** a stamp is made on the top layer of a stack of many layers
- **THEN** the evaluation reads cached block contributions rather than summing every layer over unrelated geometry

### Requirement: Smoothing can preserve the detail it passes over
Smoothing SHALL offer modes that distinguish the frequency it acts on: the current geometric smoothing, smoothing of detail coefficients only, and smoothing of the underlying form while the high-frequency detail is preserved.

A plain Laplacian pass over pores removes the pores. An artist correcting anatomy under a detailed surface is asking for the third mode and has no way to say so today.

An erase mode SHALL move the ACTIVE layer's detail toward zero without touching the base, the base detail or any other layer.

#### Scenario: Detail survives a form correction
- **WHEN** the preserve-detail smoothing mode runs over a surface carrying fine detail
- **THEN** the broad form is smoothed and the fine detail remains present

#### Scenario: Erasing touches one layer
- **WHEN** the erase mode runs over a region covered by two layers with the first active
- **THEN** the first layer's detail moves toward zero there and the second layer's detail is unchanged

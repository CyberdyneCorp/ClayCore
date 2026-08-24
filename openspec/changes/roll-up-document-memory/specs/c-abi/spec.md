# c-abi

## ADDED Requirements

### Requirement: A document reports what it costs, broken down by subsystem
The library SHALL report, in one call, the memory a document holds, separated into per-subsystem figures.

The report SHALL cover every subsystem a document owns: the edit list, voxel content, the sculpt layers held beside that content, masks, mesh layers, the undo history, and the passthrough blobs a document carries without interpreting. A subsystem the document owns SHALL NOT be omitted from the breakdown, because a figure that silently excludes the largest thing in the document is worse than no figure.

Voxel content and voxel sculpt layers SHALL be reported as SEPARATE figures. They live in the same object and one is the user's model while the other is undo for it; a combined figure would hide the only voxel bytes a host is permitted to release.

The individual figures SHALL sum to the reported total, so a host can attribute every byte it is told about.

Figures SHALL account for what containers have ALLOCATED, not for what they logically hold, since that is what the process is charged for. A report MAY therefore exceed the size the same document serializes to, and the interface SHALL say so rather than let a host read the difference as a defect.

The report SHALL be a versioned descriptor, since every subsystem added later becomes a field in it.

A memory report SHALL NOT include memory the document does not own. In particular the evaluation brick cache, which belongs to an evaluator and has its own accounting and its own trim, SHALL NOT be counted in a document figure.

#### Scenario: The report moves with the content
- **WHEN** content is rasterized into a voxel layer of a document
- **THEN** the reported voxel content figure rises, and the reported total rises with it

#### Scenario: The parts account for the whole
- **WHEN** a memory report is read for a document holding an edit list, a voxel layer, a mask and history
- **THEN** the per-subsystem figures sum to the reported total

#### Scenario: A cost is attributed to the subsystem that incurred it
- **WHEN** a mask is painted and nothing else is changed
- **THEN** the mask figure rises and the voxel content and edit list figures are unchanged

#### Scenario: An empty document reports a total
- **WHEN** a memory report is read for a document with no layers
- **THEN** the call succeeds and reports a total rather than an error

### Requirement: Memory is reportable for a single layer
The library SHALL report the same breakdown for ONE layer of a document, so a host can attribute a large document to the layer responsible for it rather than only learn that it is large.

A per-layer report SHALL use the SAME descriptor as the document-wide report. Fields that are document-wide rather than per-layer SHALL be reported as zero and documented as such, so that one struct serves both.

The CONTENT figures — voxel content, voxel sculpt layers, masks and mesh layers — SHALL sum exactly across the layers to the document-wide figures, since every byte of content belongs to exactly one layer.

The edit-list figure SHALL NOT be required to sum exactly, and the interface SHALL say why rather than let a host discover the gap and read it as a defect. Two things make it inexact and both are deliberate: the document-wide figure includes container overhead that belongs to no single layer, and content SHARED by instance layers is counted ONCE document-wide while each instance reports it in full. Reporting a shared payload once per instance would tell a host to release memory that was never allocated; reporting zero for an instance would tell it the layer is free.

Requesting a report for a layer that does not exist SHALL be an error and SHALL NOT return a zeroed report, which a host would read as an empty layer.

#### Scenario: A heavy layer is identifiable
- **WHEN** two voxel layers hold different amounts of content and each is reported
- **THEN** the layer holding more content reports the larger voxel content figure

#### Scenario: The layers account for all the content
- **WHEN** every layer of a document is reported and the per-layer content figures are summed
- **THEN** the sum equals the document-wide content figures exactly

#### Scenario: Shared instance content is counted once for the document
- **WHEN** a layer is instanced, so that two layers share one edit list
- **THEN** the document-wide edit-list figure counts the shared content once, and each layer reports it in full

#### Scenario: An unknown layer is an error
- **WHEN** a report is requested for a layer id that does not exist
- **THEN** the call fails rather than reporting an empty layer

### Requirement: Transient memory is reported separately from resident memory
Memory held only for the duration of an in-flight operation SHALL be reported as its own figure rather than folded into the subsystem it belongs to.

A mask copies its storage when a recorded step opens, so a mask costs roughly double for the duration of that step. A figure that is about to fall on its own SHALL NOT be indistinguishable from one that will still be held afterwards.

The interface SHALL state where this figure can and cannot be observed, rather than describe a behaviour a caller cannot reach. Through the C ABI it is always zero, because every mask entry point opens its step and closes it before returning and calls on one document must be serialized — so no caller can hold a handle while a step is open. The field SHALL still be reported, so that the total remains the sum of the reported fields if an entry point spanning a step is ever added; a total that silently gained bytes belonging to no reported field would be worse than a field that reads zero.

#### Scenario: A snapshot taken during a step is visible and then released
- **WHEN** memory is reported by an embedder that holds a recorded mask step open across two edits, and again after the step closes
- **THEN** the transient figure is non-zero while the step is open and zero after it

#### Scenario: The C ABI reports no transient memory
- **WHEN** a document is asked for its memory after any sequence of mask edits through the C entry points
- **THEN** the transient figure is zero, because no step can be open at that moment

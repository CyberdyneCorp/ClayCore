# c-abi — a layer can be instanced

Delta for `instance-a-layer`.

## ADDED Requirements

### Requirement: A layer can be instanced without copying its edit list
The C API SHALL offer an entry point that adds a layer SHARING an existing SDF layer's edit list, rather than copying it. The cost of the call SHALL NOT grow with the size of the source's edit list, which is the whole reason the call exists: a host duplicating a subtool today pays memory and time proportional to everything the artist has already sculpted, per copy.

The instance SHALL carry its OWN transform, name, visibility, protection, mirror and radial mode. Those SHALL start at the source's values, because the instance is a copy of the layer, and SHALL diverge freely afterwards — that is what makes ten instances of one blockout ten placements rather than ten identical layers.

An edit through EITHER layer SHALL be an edit to the shared content and SHALL be visible through both, and the dirty bounds reported for it SHALL cover every layer sharing the content, as the undo bounds contract already states.

Creation SHALL go through the same command vocabulary the other layer-creating entry points use, so that an enabled undo stack records it as ONE step and a single undo removes the instance.

Instancing an INSTANCE SHALL share the same content as the original, not chain. There is one allocation and the relation between the layers holding it is symmetric; a chain would invent a parent whose removal would then have to mean something.

A source that is not an SDF layer SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` and a message saying so, since a voxel grid and a mesh are held beside the document by layer id rather than by a shared pointer. A source id that does not exist SHALL be `CLAY_ERROR_NOT_FOUND`. A NULL or empty name SHALL be refused, matching the rename entry point: an empty name is what a cleared text field submits.

#### Scenario: An edit through the instance reaches the source
- **WHEN** a layer is instanced and an item is added through the INSTANCE's id
- **THEN** the source layer evaluates with that item too

#### Scenario: One edit list, two placements
- **WHEN** an instance is given a different layer transform from its source
- **THEN** the document evaluates the shared edit list at both placements

#### Scenario: Creating an instance is one undo step
- **WHEN** an instance is created on a document with undo enabled and one undo is performed
- **THEN** the instance is gone and the source is unchanged, and a redo brings the instance back still sharing the content

#### Scenario: The instance's own properties diverge
- **WHEN** an instance is renamed, hidden, ghosted or given a mirror
- **THEN** the source keeps the properties it had

#### Scenario: A voxel or mesh source is refused
- **WHEN** an instance is asked for with a voxel or mesh layer as the source
- **THEN** the call fails with an invalid-argument error naming the layer's representation

#### Scenario: An unknown source is not found
- **WHEN** an instance is asked for with a layer id the document does not have
- **THEN** the call fails with a not-found error

#### Scenario: An empty name is refused
- **WHEN** an instance is asked for with a NULL or empty name
- **THEN** the call fails and no layer is added

#### Scenario: An instance of an instance shares one edit list
- **WHEN** an instance is itself instanced
- **THEN** all three layers share one edit list and the document counts it once

### Requirement: An instance survives a save and load as a reference
A document holding instanced layers SHALL serialize the shared edit list ONCE and SHALL restore the sharing on load. A round trip SHALL NOT multiply the allocation the memory report promises to count once, and SHALL NOT silently unlink the layers.

The document-wide memory report for a document of N instances SHALL therefore be unchanged, within container overhead, by a save and reload — and each instance SHALL still report the content in full, exactly as it did before the round trip.

Removing the SOURCE layer while instances remain SHALL be legal: the content stays alive because the instances hold it. A surviving instance SHALL still evaluate, SHALL still save, and SHALL reload with its content intact.

#### Scenario: Ten instances reload as one allocation
- **WHEN** a document holding a source layer and nine instances is saved and reloaded
- **THEN** the document-wide edit-list figure is what it was before the round trip, and does not scale with the instance count

#### Scenario: The share survives the round trip
- **WHEN** a document with an instance is saved, reloaded, and an item is added through one of the two layers
- **THEN** the other layer evaluates with that item too

#### Scenario: An orphaned instance keeps its content
- **WHEN** the source layer is removed and the document is saved and reloaded
- **THEN** the surviving instance still evaluates to the same field

### Requirement: A host can see that a layer is an instance
The layer information descriptor SHALL report which layer a given layer takes its content from, and how many layers share that content. Without both, a host cannot draw the link: the id alone marks the following end of it and leaves the SOURCE indistinguishable from an ordinary layer.

The reported source SHALL be the FIRST layer in stack order holding that content, and a layer that IS that first layer SHALL report 0. This is the same rule the writer uses to decide which layer owns the content in the file, so what a host is told is what a save would write, and the answer SHALL be unchanged by a save and reload.

It follows that removing a source layer SHALL NOT leave a dangling reference: the first surviving sharer becomes the owner and reports 0, and any others report its id.

These fields SHALL be APPENDED to the existing descriptor, so a host compiled against the older layout declares a shorter `struct_size` and simply does not receive them.

#### Scenario: An instance names its source
- **WHEN** the layer information is read for an instanced layer
- **THEN** the reported content source is the source layer's id and the reported share count is 2

#### Scenario: A source reports no source of its own
- **WHEN** the layer information is read for the layer that was instanced
- **THEN** the reported content source is 0 and the reported share count is 2

#### Scenario: An ordinary layer shares with nobody
- **WHEN** the layer information is read for a layer nothing instances
- **THEN** the reported content source is 0 and the reported share count is 1

#### Scenario: The link survives a reload
- **WHEN** a document with an instance is saved and reloaded and the layer information is read again
- **THEN** the same source and the same share count are reported

#### Scenario: Removing the source re-homes the link
- **WHEN** the source layer of two instances is removed
- **THEN** the first surviving instance reports 0 and the second reports the first's id

### Requirement: Consolidating an instance severs it
Consolidating a layer whose edit list is shared SHALL give that layer a PRIVATE copy of the content before baking, so the bake replaces that layer's edit list alone and every other layer sharing the content is untouched.

Baking in place would rewrite the edit list of every instance, turning nine subtools into volumes because the artist baked the tenth. That is not a reading of "this shape is finished" anyone asks for, and it is silent.

The sever SHALL be part of the SAME undo step as the bake, so that one undo restores the layer with its original shared content and the link intact. A host SHALL be able to observe that the layers stopped sharing, through the layer information descriptor.

Measuring what a consolidation would cost SHALL NOT sever anything, since it changes nothing.

#### Scenario: Baking one instance leaves the other parametric
- **WHEN** a layer with two items is instanced and the instance is consolidated
- **THEN** the instance holds a single baked item and the source still holds its two

#### Scenario: A severed instance stops following the source
- **WHEN** an item is added to the source after its instance was consolidated
- **THEN** the consolidated layer is unchanged

#### Scenario: Undoing a bake restores the share
- **WHEN** an instance is consolidated and the consolidation is undone
- **THEN** the layer holds its original items again and shares them with the source

#### Scenario: Measuring a consolidation changes nothing
- **WHEN** the cost of consolidating an instance is read
- **THEN** the layers still share one edit list

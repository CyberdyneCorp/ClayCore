# scene-model — sharing survives the command vocabulary

Delta for `instance-a-layer`.

## ADDED Requirements

### Requirement: An added layer may name the content it shares
The layer-add command SHALL be able to name a CONTENT SOURCE layer instead of carrying the content itself, so that creating an instance costs a reference wherever the command travels — the undo stack, the journal and the document file alike.

Applying such a command SHALL resolve the named layer and share its content. It SHALL refuse when the id does not resolve, rather than fall back to an empty or copied edit list: a replay that silently produced an unshared layer would reintroduce, one recovery later, exactly the multiplication the format change removes.

A command that carries content SHALL be applied unchanged, so the in-memory path — where the command already holds the shared pointer — is unaffected.

#### Scenario: Replaying an instance creation shares
- **WHEN** a journal containing an instance creation is replayed onto the snapshot it was taken against
- **THEN** the replayed instance shares the source layer's content rather than holding a copy

#### Scenario: A named source that is gone refuses
- **WHEN** a layer-add command naming a content source is applied to a document without that layer
- **THEN** the command is refused and the document is unchanged

### Requirement: Consolidation gives a shared layer its own content first
Consolidating a layer SHALL affect that layer only. When the layer's edit list is shared with other layers, consolidation SHALL first replace it with a private copy, and that replacement SHALL be part of the same undo step as the bake so that undoing the consolidation restores the shared content.

#### Scenario: A bake does not reach the other sharers
- **WHEN** one of two layers sharing an edit list is consolidated
- **THEN** the other layer's edit list is exactly what it was

#### Scenario: Undo restores the sharing
- **WHEN** the consolidation of a shared layer is undone
- **THEN** the layer's content is the shared content again, and an edit through either layer is visible in both

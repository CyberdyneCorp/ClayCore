# scene-model — sculpt layers

Delta for `add-sculpt-layers`.

## ADDED Requirements

### Requirement: Sculpt layers persist with the document
A document carrying sculpt layers SHALL save and reload them, evaluating bit-identically, and SHALL state the memory a layer costs so a host can show it.

The format SHALL stay backward-open: a reader that does not know sculpt layers SHALL open the document flattened rather than refusing it.

#### Scenario: An older reader opens a document with sculpt layers
- **WHEN** a reader without sculpt-layer support opens a document that has them
- **THEN** it opens the flattened result rather than failing

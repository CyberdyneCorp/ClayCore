# python-bindings — the cut tool

Delta for `add-cut-tool`.

## ADDED Requirements

### Requirement: Cuts from Python
The module SHALL expose resolving a cut frame and shape into an item, and placing it on a layer with a chosen op, defaulting the swept region to the document's own bounds.

#### Scenario: Cutting a hole
- **WHEN** a script cuts a circle from a solid and evaluates the document
- **THEN** the field reports empty inside the circle's sweep and solid outside it

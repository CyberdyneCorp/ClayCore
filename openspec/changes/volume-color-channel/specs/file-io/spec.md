# file-io — a volume's colour survives a save

Delta for `volume-color-channel`.

## ADDED Requirements

### Requirement: A volume's colour is written and read back
A serialised volume SHALL carry its colour section when it has one, and SHALL record its absence when it does not. A colour a document cannot save is a colour a sculptor loses on reload, which is the failure this whole change exists to remove one level up.

The section SHALL be absent-or-present as a whole, with its own length, so an uncoloured volume costs a marker rather than an empty array.

The document format minor SHALL move to 9, and the container and scene payload versions SHALL move together as the existing static assertion requires.

A document written at minor 8 SHALL open: its volumes have no colour section and SHALL read as uncoloured, which is what they are. A document written at minor 9 SHALL be REFUSED by an older reader under the existing forward-refuse rule, rather than being read with a corrupt tail.

#### Scenario: Colour survives a round trip through a file
- **WHEN** a document containing a coloured volume is saved and loaded
- **THEN** the volume's per-sample colours are what they were, and evaluating the document reports the same colours

#### Scenario: An older document still opens
- **WHEN** a minor-8 document containing a volume is loaded by this build
- **THEN** it opens, the volume reads as uncoloured, and evaluation reports the item's constant colour as it always did

#### Scenario: A newer document is refused rather than misread
- **WHEN** a minor-9 document is opened by a reader built before this change
- **THEN** it is refused on version grounds

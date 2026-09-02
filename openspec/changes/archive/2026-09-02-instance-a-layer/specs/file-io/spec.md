# file-io — a layer record names its content

Delta for `instance-a-layer`.

## ADDED Requirements

### Requirement: A layer record may reference another layer's content
From scene minor 15 a layer record SHALL carry a content-source layer id: 0 meaning the layer owns the content that follows in the record, any other id meaning the layer shares the content of the layer with that id and carries none of its own.

Ownership SHALL be derived at write time from the identity of the content itself, in STACK ORDER: the first layer holding a given edit list owns it and every later holder names it. No flag is stored for it, so the file cannot disagree with the document, and a document whose original source layer was removed while its instances remain writes correctly with no special case.

A reader SHALL resolve the names in a second pass, after every layer record is read, and SHALL REFUSE a document naming a source it does not have rather than open it with an empty or duplicated edit list.

Writing at minor 14 or below SHALL write every layer's content inline as it always did. Such a document opens in an older build with the instances as INDEPENDENT COPIES: the shapes are right and the share is gone, so an edit through one no longer reaches the others. That is the recoverable direction, and it is why the writer takes a minor.

A build that predates minor 15 reading a minor 15 document SHALL fail rather than misread, which is the layer record's existing trade: the record is not length-prefixed, so a field it does not expect desynchronises every record after it and the reader's bounds and count checks reject the stream.

#### Scenario: Shared content is written once
- **WHEN** a document with a source layer and nine instances of it is saved
- **THEN** the edit list appears once in the file and the nine instances carry a reference to it

#### Scenario: A reload restores the sharing
- **WHEN** such a document is loaded
- **THEN** the ten layers hold one edit list and an edit through any of them is visible through all

#### Scenario: A removed source still writes
- **WHEN** the layer that was originally instanced is removed and the document is saved and reloaded
- **THEN** the surviving instances hold one edit list, owned by the first of them in stack order

#### Scenario: Writing at an older minor drops only the sharing
- **WHEN** a document with an instance is written at minor 14
- **THEN** each layer carries its own copy of the content, and both evaluate as they did

#### Scenario: A reference to a layer that is not in the file is refused
- **WHEN** a document names a content source it does not contain
- **THEN** the load fails

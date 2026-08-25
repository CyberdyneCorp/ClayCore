# c-abi

## ADDED Requirements

### Requirement: Serialized bytes cross the ABI
A caller SHALL be able to serialize a document or a mesh to memory, and to construct one from memory, without naming a filesystem path.

Serialized output SHALL be returned as an opaque owner handle carrying the bytes, with borrowing accessors for the pointer and the length and an explicit destroy — the pattern the mesh and tape handles already use. It SHALL NOT be returned by the size-query pattern, because answering the size would mean serializing the payload twice.

The borrowed pointer SHALL remain valid until the handle is destroyed and SHALL be unaffected by any subsequent edit to the object it came from, so a host may hand the bytes to an asynchronous writer without copying them first.

The bytes a memory save produces SHALL be byte-identical to what the corresponding file save writes for the same object, and a memory load SHALL accept exactly what a file load accepts. Neither direction SHALL introduce a format, a header, or a framing of its own.

A mesh memory entry point SHALL take the format by NAME rather than deriving it from an extension, because a buffer has none. The names SHALL be the file extensions without the leading dot and SHALL be matched case-insensitively, consistent with the existing extension rule. An unknown name SHALL be refused with the same code an unsupported extension is refused with, and SHALL NOT fall back to a default format.

#### Scenario: A document round-trips through memory
- **WHEN** a document is saved to memory and loaded back from those bytes
- **THEN** the loaded document evaluates identically to the original at every probe point, and its layers, names and stack order are recovered

#### Scenario: Memory and file agree byte for byte
- **WHEN** the same object is saved to a path and to memory
- **THEN** the file's contents and the buffer's bytes are identical

#### Scenario: The borrowed bytes survive an edit
- **WHEN** a host saves a document to memory and then edits the document before reading the buffer
- **THEN** the buffer still holds what was serialized, and destroying it is still the caller's single obligation

#### Scenario: An unknown format name is refused
- **WHEN** a caller asks to save a mesh to memory naming a format the library does not write
- **THEN** the call is refused rather than served in some default format

#### Scenario: Null and empty inputs
- **WHEN** a memory loader is given a null pointer, a zero length, or bytes that are not the format claimed
- **THEN** it fails with a typed error and produces no handle for the caller to free

### Requirement: The import guardrails apply to bytes where they still mean something
Every limit that guards a load from a path and that still has something to bound SHALL guard a load from memory identically. A limit that has nothing left to bound SHALL be stated as inapplicable rather than accepted and ignored.

The mesh import budget's vertex and triangle ceilings SHALL be accepted and enforced by the mesh memory loader, unchanged. A buffer is the more likely untrusted input of the two — it is what arrives from a network, a pasteboard or another process — so a memory path that skipped them would invert the protection they exist to give.

The budget's file-byte ceiling SHALL NOT be a parameter of any memory loader. It bounds the bytes a loader will read into memory before sizing a buffer, and a caller holding a buffer has already performed that read; accepting it there would be a parameter that cannot do anything, which is worse than not offering it. The document memory loader therefore SHALL take no budget at all, and the header SHALL say why rather than leave a reader to infer it from an absence.

Bounds checking SHALL NOT depend on the budget. Every memory loader SHALL refuse a truncated, corrupt or mis-declared buffer without reading past the length it was given, which is a property of the readers rather than of any ceiling.

#### Scenario: A budget refuses an oversized buffer
- **WHEN** a mesh is loaded from memory under a budget smaller than the mesh
- **THEN** the load is refused with the same error the file loader gives, and no mesh handle is produced

#### Scenario: A malformed buffer stays in bounds
- **WHEN** a truncated or corrupt buffer is loaded from memory
- **THEN** the loader refuses it without reading past the length it was given, whether or not a budget was supplied

## MODIFIED Requirements

### Requirement: A file extension is matched case-insensitively
An importer SHALL match a file's extension without regard to case, because a file named `MODEL.OBJ` is an OBJ file. The Python loader has always done so; the C one did not, and refused such a file as an unknown format.

An EXPORTER SHALL match it the same way, and by the same normalisation the importer uses. The two disagreed: `clay_mesh_load` lowercased the extension and `clay_mesh_save` compared it as given, so a host could load `MODEL.OBJ` and then be refused when it saved back to the path it had just read from. A format name given to a memory entry point SHALL be normalised identically, so that one rule covers reading, writing, paths and buffers.

#### Scenario: An upper-case extension loads
- **WHEN** a mesh file whose extension is upper-case is loaded
- **THEN** it loads, rather than being reported as an unknown format

#### Scenario: An upper-case extension saves
- **WHEN** a mesh is saved to a path whose extension is upper-case
- **THEN** it is written in that format, rather than refused as unknown

#### Scenario: One rule for names and extensions
- **WHEN** the same format is named in upper case to a memory entry point and in upper case as an extension to a path entry point
- **THEN** both are accepted and produce the same format

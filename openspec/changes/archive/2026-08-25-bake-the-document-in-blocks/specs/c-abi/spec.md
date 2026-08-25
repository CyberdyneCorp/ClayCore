# c-abi — bake a document through the pool, not one point at a time

Delta for `bake-the-document-in-blocks`.

## MODIFIED Requirements

### Requirement: The C ABI can bake a document into a volume
The C ABI SHALL sample a document's own field into an item carrying a volume, with an optional explicit region.

Without it the volume operations are unreachable for anything an app made itself: relax and flatten both act on a volume, and a mesh was the only source, so an app could smooth an imported scan but not its own sculpt, and could not collapse a long edit list into a single item.

A cell size SHALL be required rather than derived. A mesh's bounds imply one; a document has no intrinsic scale, and after a bake the resolution IS the shape — so guessing would silently pick a resolution the caller never chose.

A region containing no surface SHALL be refused rather than returning an item that contributes nothing. Note that such a volume is not "empty" in the structural sense: it has a full brick index and stores no samples, and it evaluates perfectly well as a lower bound on the distance to anything.

Every entry point that bakes a DOCUMENT SHALL evaluate the tape in BLOCKS of sample positions rather than one point at a time, through the same batched evaluator a layer bake uses. A tape instruction costs roughly ten times its own arithmetic, so the interpreter is most of a bake and a per-point walk pays that interpreter once per sample instead of once per block; the difference is more than an order of magnitude on a document of a few hundred items. The result SHALL be byte-identical to the per-point walk, not merely close — each sample depends only on its own position and blocks are assembled in slot order however they were computed, so no scheduling can change any of them — and a benchmark pairing the two SHALL gate it, because this is a property that was already true of one bake path and silently untrue of the others.

A verb that SHAPES the field as it bakes — flatten is the one — SHALL apply that shaping to the sampled block BEFORE the sparsity decision is taken, not to the volume afterwards. Which bricks a bake keeps is decided from the values it is handed, and a verb that moves the surface by more than a band would otherwise keep the bricks around the surface its SOURCE had and leave the result's own surface in a region nothing sampled.

#### Scenario: A document becomes a volume
- **WHEN** a C caller bakes a document containing a shape
- **THEN** the resulting item's field is that shape

#### Scenario: The baked volume can then be relaxed and flattened
- **WHEN** a C caller bakes a document and applies relax or flatten to the result
- **THEN** both succeed, which they could not before for anything but an imported mesh

#### Scenario: A missing cell size is refused
- **WHEN** a C caller bakes a document without giving a cell size
- **THEN** the call fails rather than choosing a resolution on the caller's behalf

#### Scenario: A region with no surface is refused
- **WHEN** a C caller bakes a region of a document that contains no surface
- **THEN** the call fails rather than returning an item that would contribute nothing

#### Scenario: The batched bake and the per-point walk agree exactly
- **GIVEN** a document whose field is steep enough that kept bricks hold values far past the band
- **WHEN** it is baked once through the batched evaluator and once through a per-point walk of the same tape, at the same cell and band
- **THEN** the two volumes serialize to the same bytes — the same samples, the same sparsity, the same bounds

#### Scenario: A document-sourced flatten keeps the bricks around the flattened surface
- **WHEN** a document-sourced flatten draws the surface onto a plane several band widths from where the source put it
- **THEN** the bricks the result stores are the ones around the flattened surface, and its band tracks that surface rather than the source's

#### Scenario: A bake without a backend still bakes
- **GIVEN** a build in which no CPU backend is registered
- **WHEN** a document is baked
- **THEN** it produces the same volume, evaluated serially, rather than failing

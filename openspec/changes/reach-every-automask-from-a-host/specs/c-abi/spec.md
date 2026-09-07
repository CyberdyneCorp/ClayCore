## ADDED Requirements

### Requirement: A host reaches every automask factor the engine has

The C ABI SHALL let a host enable and supply EVERY automask factor the engine
implements, including the two whose inputs are not scalars — the cavity measure
and the surface-group lattice.

Those two SHALL be supplied as the world-addressed lattices the engine already
has handles for, and SHALL NOT be supplied as a host callback: they are
evaluated per vertex from worker threads, and a re-entrant callback into a host
from inside a stamp is a shape this boundary does not offer.

They SHALL be named ONCE PER SESSION rather than per stamp, because the engine
holds them as closures and rebuilding those per dab is an allocation per dab.

Naming the inputs SHALL NOT enable any factor. Which factors run SHALL remain a
property of the brush descriptor, so that a host may wire its inputs at session
start without changing a single stamp.

A factor whose bit is set with no input SHALL remain inert rather than
becoming an error, so that a host may carry an automask preset before it has
built the lattice that preset needs.

The same descriptor SHALL be accepted by the fixed-mesh, adaptive and
multiresolution sculptors, because the factors mean the same thing whichever
surface is under the brush.

#### Scenario: A factor's bit alone still changes nothing
- **WHEN** a stamp sets an automask bit whose input has not been supplied
- **THEN** the result is the result that stamp had before the factor was reachable

#### Scenario: An input alone changes nothing
- **WHEN** a session names its automask inputs and stamps with no automask bits set
- **THEN** the result is the result that stamp had with no inputs named

#### Scenario: A factor with its input gates the stamp
- **WHEN** a stamp sets a factor's bit and its input has been supplied
- **THEN** the per-vertex weight is scaled by that factor and the displacement changes accordingly

#### Scenario: The inputs can be released
- **WHEN** a session clears its automask inputs
- **THEN** both factors are inert again and the session holds nothing of what they named

#### Scenario: An input that has left its document is refused
- **WHEN** a session names a borrowed lattice that is no longer in its document
- **THEN** the call is refused as not found rather than accepted and silently ineffective

### Requirement: A sculptor's declared frame places the point a world lattice is asked about

A sculpting session's vertices are in the mesh's own space and the mask, cavity
and group lattices are world-addressed. Every one of those lattices SHALL be
sampled at the vertex's placed position, using the frame the session declared.

The frame SHALL be read when a lattice is sampled rather than captured when the
inputs are named, so that declaring a frame after naming inputs and naming
inputs after declaring a frame produce the same result.

A session that declares no frame SHALL be sampled where its vertices are. The
adaptive and multiresolution sessions declare none, so their lattices — these
two and the painted mask they already took — are sampled at the surface's own
positions, which is the reading their mask gate already had.

#### Scenario: A placed session reaches the region it was placed into
- **WHEN** a session declares a frame and its automask inputs describe the region the mesh was placed into
- **THEN** the automask reads that region rather than the region the untransformed mesh occupies

#### Scenario: The order of the two calls does not matter
- **WHEN** a session declares its frame after naming its automask inputs
- **THEN** the result is the same as declaring it before

### Requirement: A node's colour is readable

Every value the C ABI can write onto a placed node SHALL be readable back, the
node's colour included.

The reader SHALL be total: there is no unset colour to report. An item's colour
is the one its creation descriptor carried, and a group's — which takes none at
creation — is the engine's own default.

A group SHALL answer, because a group holds a colour its own setter writes.

The value SHALL be what THIS NODE holds rather than what the composed surface
shows at a point, which is a different question with a different answer.

#### Scenario: What was written comes back
- **WHEN** a node's colour is set and then read
- **THEN** the value read is the value written

#### Scenario: A node nobody coloured answers
- **WHEN** a node whose colour was never set through the setter is read
- **THEN** the colour it was created with is reported and the call succeeds

#### Scenario: The value survives history
- **WHEN** a colour edit is undone and redone
- **THEN** the reader reports the colour of the state the document is in

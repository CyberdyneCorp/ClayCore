# meshing — the attribute pass costs what the backend charges

Delta for `batch-the-mesh-attribute-pass`.

## MODIFIED Requirements

### Requirement: Mesh attributes
Meshers SHALL emit vertex colors sampled from the scene color field (faithful to blend gradients via the material-mix factor), normals from field gradient or face normals (caller choice), and SHALL offer an optional box-projection UV utility.

For a brick mesh, gradient normals and vertex colors SHALL be evaluated through per-brick culled tapes — each vertex against a tape culled to the band-dilated region of the brick owning its position, the same region the refill path culls against — so that the attribute cost follows the bricks being meshed rather than the total size of the document. Re-meshing a fixed brick set with gradient normals SHALL NOT scale with the number of document nodes outside those bricks' influence.

Culling SHALL NOT change the attributes: inside a brick's band-dilated cull region the culled tape's band-clamped results are bit-identical to the full tape's, mesh vertices lie on the surface far inside the band, and the gradient taps move by the gradient epsilon, which the cull region is additionally dilated by. The normals and colors SHALL equal a full-document-tape evaluation at every vertex.

EVERY mesher's attribute pass SHALL evaluate its vertices through the evaluation backend as ONE batch rather than a tap at a time. A colour is one walk of the tape and a gradient is four, so a mesh of a hundred thousand vertices asks for half a million walks; taken one at a time on one thread they are the whole cost of the mesh — measured at 96% of a coloured `mesh_tape`, and 58x slower than the same taps batched. A mesher that reaches for the tape directly is paying for an interpreter per point that the backend walks once per BLOCK of points, and for one core out of however many the machine has.

Batching SHALL NOT change the attributes. The backend evaluates the same taps against the same tape at the same points, so the colours and normals SHALL be identical to the serial walk BIT FOR BIT rather than within a tolerance — a bound would admit a reordered gradient tap, which is the one mistake this is likely to make.

A build with NO evaluation backend registered SHALL still produce attributes, by the serial walk, rather than producing none.

#### Scenario: Blend gradient in vertex colors
- **WHEN** a mesh is emitted across a blend between two colored items
- **THEN** the vertex colors vary smoothly across the blend rather than stepping at the seam

#### Scenario: Far nodes do not change a brick mesh's attributes
- **WHEN** a fixed brick set is meshed with gradient normals and colors from a document holding hundreds of nodes far outside those bricks' influence, and from a document holding only the nearby nodes
- **THEN** the two meshes carry identical geometry and equal normals and colors, and both equal the full document tape's gradient at every vertex

#### Scenario: Gradient normals cost the bricks, not the document
- **WHEN** the same fixed brick set is re-meshed with gradient normals as the document grows with far-away edits
- **THEN** the meshing time follows the brick count rather than the node count

#### Scenario: The batched attribute pass is the serial one
- **WHEN** a mesh is given colors and gradient normals through the backend's batch, and the same tape is walked at the same vertices a tap at a time
- **THEN** every colour and every normal is the same float, bit for bit

#### Scenario: Attributes survive a build with no backend
- **WHEN** a mesh is given colors and gradient normals with no evaluation backend registered
- **THEN** it carries the same attributes, evaluated serially

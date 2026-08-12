# meshing — brick-mesh field attributes go through per-brick culled tapes

Delta for `cull-brick-mesh-attributes` (issue #73). Modifies the mesh
attributes requirement: attributes of a brick mesh were evaluated against the
whole document's tape at every vertex, so a fixed re-mesh grew with
everything already sculpted while the marching itself stayed flat.

## MODIFIED Requirements

### Requirement: Mesh attributes
Meshers SHALL emit vertex colors sampled from the scene color field (faithful to blend gradients via the material-mix factor), normals from field gradient or face normals (caller choice), and SHALL offer an optional box-projection UV utility.

For a brick mesh, gradient normals and vertex colors SHALL be evaluated through per-brick culled tapes — each vertex against a tape culled to the band-dilated region of the brick owning its position, the same region the refill path culls against — so that the attribute cost follows the bricks being meshed rather than the total size of the document. Re-meshing a fixed brick set with gradient normals SHALL NOT scale with the number of document nodes outside those bricks' influence.

Culling SHALL NOT change the attributes: inside a brick's band-dilated cull region the culled tape's band-clamped results are bit-identical to the full tape's, mesh vertices lie on the surface far inside the band, and the gradient taps move by the gradient epsilon, which the cull region is additionally dilated by. The normals and colors SHALL equal a full-document-tape evaluation at every vertex.

#### Scenario: Blend gradient in vertex colors
- **WHEN** two differently colored shapes joined by a smooth blend are meshed
- **THEN** vertex colors across the joint interpolate following the blend's material-mix falloff, not a hard color seam

#### Scenario: Far nodes do not change a brick mesh's attributes
- **WHEN** a fixed brick set is meshed with gradient normals and colors from a document holding hundreds of nodes far outside those bricks' influence, and from a document holding only the nearby nodes
- **THEN** the two meshes carry identical geometry and equal normals and colors, and both equal the full document tape's gradient at every vertex

#### Scenario: Gradient normals cost the bricks, not the document
- **WHEN** the same fixed brick set is re-meshed with gradient normals as the document grows with far-away edits
- **THEN** the meshing time stays with the brick set rather than growing linearly with the document's node count, which the benchmark gate enforces as a ratio

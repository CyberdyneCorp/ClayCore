# Every crossing, audited

The roadmap describes this as "a mesh layer can carry a layer transform but its
stored triangle arrays are not transformed". That is true and it is not the
defect. The arrays being layer-local is the RIGHT contract — baking a transform
into vertices on every layer move is expensive, lossy and hostile to history.

**The defect is that two calls a host makes back to back are in different
spaces, and neither says so.**

## The table

| Operation | Input space | Internal | Output space | Stated in the header? |
|---|---|---|---|---|
| `clay_mesh_positions` / `_indices` (layer storage) | — | local | **local** | no |
| `clay_layer_bounds` (`layer_world_bounds`) | — | local | **world** | yes |
| `clay_mesh_sculptor_raycast` | **world** ray + `clay_mesh_frame` | local ray | **world** hit position and normal | yes, in `pick.h` |
| `clay_mesh_sculptor_stamp` | **local** centre, radius, direction | local | local | **no** |
| `clay_mesh_sculptor_apply_stroke` | **local** samples | local | local | in `brush/stroke.h`, not in `clay.h` |
| `clay_mesh_sculptor_apply_preset` | **local** samples | local | local | as above |
| `clay_mesh_sculptor_lattice` / `_deformer` | **local** cage / axes | local | local | no |
| `clay_document_mesh_combined` (export) | — | local | **world** | yes |
| `clay_mesh_transform` / `_nonuniform` | explicit | — | explicit | yes |
| `clay_document_voxel_remesh_layer` | — | local | local | no |
| `clay_document_mesh_to_field` / volume-from-mesh | — | ? | ? | **to determine** |

## The reachable failure

1. A host raycasts to find where the finger is. `clay_mesh_sculptor_raycast`
   takes a **world** ray and a `clay_mesh_frame`, and hands back a **world**
   hit position — `pick.h` says the conversion is done there deliberately,
   "because a caller doing it by hand gets a brush whose radius changes when a
   layer is scaled, and gets it wrong silently".
2. The host passes that hit position straight into
   `clay_mesh_sculptor_stamp` as the brush centre, which reads it as **local**.

On any layer with a non-identity transform the dab lands in the wrong place,
and the radius is in the wrong units. The header that warns about exactly this
failure is one call away from the call that causes it.

## What `mesh_to_world` on the stroke calls is NOT

`MeshStrokeOptions::mesh_to_world` looks like the fix and is not: `brush/stroke.h`
states it is used ONLY to find a vertex on the world-addressed mask lattice.
The stamps themselves stay in the mesh's own space. A host that passes its
layer transform there and assumes its samples are world gets a correctly masked
stroke in the wrong place.

## The contract to land

```text
mesh vertex arrays = layer-local
layer.xform        = local -> document/world
```

unchanged — and every ClayCore call crossing the boundary takes the same
`clay_mesh_frame`, means the same thing by it, and says so beside itself.

Normals go through the inverse transpose, never the position map; the ABI
already states this for `clay_mesh_transform_nonuniform` and it must hold for
every crossing that produces one.

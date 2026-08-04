// Swift smoke for the C ABI (c-abi spec: "Swift consumption via SwiftPM").
//
// This is the check that the header is actually usable from Swift rather than
// merely valid C: it exercises the surface ClaySpace needs — a composed SDF
// edit through the item builder, voxel sculpting through a borrowed layer
// handle, and meshing — and asserts results, not just return codes.
//
// Built against the xcframework's macOS slice by tools/check_swift_smoke.sh.

import Foundation
import claycore

var failures = 0

func check(_ condition: Bool, _ what: String) {
    if condition {
        print("  ok   \(what)")
    } else {
        print("  FAIL \(what)")
        failures += 1
    }
}

func lastError() -> String {
    guard let message = clay_last_error() else { return "<none>" }
    return String(cString: message)
}

// -- version ----------------------------------------------------------------

var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
clay_version(&major, &minor, &patch)
print("claycore ABI \(major).\(minor).\(patch)")
check(major == CLAY_ABI_MAJOR && minor == CLAY_ABI_MINOR,
      "the linked library matches the header it was compiled against")

// -- a composed SDF edit through the item builder ----------------------------

guard let doc = clay_document_create() else {
    print("FAIL could not create a document")
    exit(1)
}
defer { clay_document_destroy(doc) }

var layer: clay_layer_id = 0
check(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK, "added an SDF layer")

// A sphere, twisted then bent, arrayed about the axis: the composed case that
// the flat descriptor could never express.
var params: [Float] = [0.6]
guard let item = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &params, 1) else {
    print("FAIL could not create an item builder: \(lastError())")
    exit(1)
}
var origin: [Float] = [0, 0, 0]
check(clay_item_set_position(item, &origin) == CLAY_OK, "set the item position")
var twist: [Float] = [1.5]        // TWIST takes k alone
check(clay_item_add_deformer(item, Int32(CLAY_DEFORM_TWIST.rawValue), &twist, 1, 0) == CLAY_OK,
      "appended a twist")
var bend: [Float] = [0.6]         // BEND likewise
check(clay_item_add_deformer(item, Int32(CLAY_DEFORM_BEND.rawValue), &bend, 1, 0) == CLAY_OK,
      "appended a bend after it")
check(clay_item_set_repeat_radial(item, 6, 1.0) == CLAY_OK, "set a radial array")
var tint: [Float] = [0.2, 0.6, 0.8]
check(clay_item_set_color(item, &tint) == CLAY_OK, "set a colour")

var node: clay_node_id = 0
check(clay_layer_add_item(doc, layer, item, &node) == CLAY_OK,
      "added the composed item to the layer")
clay_item_destroy(item)

// A radial array offsets the sampled point, so the origin lands inside a copy
// and a point out at the offset radius lands outside it. These are the values
// pyclay reports for the identical construction, which is the real assertion
// here: the C boundary authors the same document the Python one does.
var points: [Float] = [0, 0, 0, 1.0, 0, 0]
var distances = [Float](repeating: 0, count: 2)
check(clay_eval_points(doc, nil, &points, 2, &distances, nil) == CLAY_OK,
      "evaluated the field")
check(distances[0] < 0, "the origin is inside the array (\(distances[0]))")
check(distances[1] > 0, "a point at the offset radius is outside it (\(distances[1]))")
check(abs(distances[0] - -0.6) < 1e-4 && abs(distances[1] - 0.4) < 1e-4,
      "the field matches what pyclay reports for the same construction")

// -- voxel sculpting through a borrowed layer handle -------------------------

var voxelLayer: clay_layer_id = 0
var grid: OpaquePointer? = nil
check(clay_document_add_voxel_layer(doc, "blocks", 0.1, &voxelLayer, &grid) == CLAY_OK,
      "added a voxel layer and borrowed its grid")

var index: Int32 = 0
var rgb: [Float] = [0.9, 0.5, 0.2]
check(clay_voxel_palette_add(grid, &rgb, &index) == CLAY_OK && index > 0,
      "added a palette entry")

var brush = clay_brush_params()
brush.struct_size = UInt32(MemoryLayout<clay_brush_params>.size)
brush.size = 9
brush.shape = Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue)
brush.falloff = Int32(CLAY_BRUSH_FALLOFF_CONSTANT.rawValue)
brush.strength = 1.0
brush.seed = 0

var cell: [Int32] = [0, 0, 0]
check(clay_voxel_set_brush(grid, &cell, &brush, index) == CLAY_OK, "stamped a sphere brush")

var occupied: Int = 0
check(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied > 0,
      "the brush left material behind (\(occupied) cells)")

let stamped = occupied
check(clay_voxel_sculpt_smooth(grid, &cell, &brush) == CLAY_OK, "ran the smooth verb")
check(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK, "read the count back")
check(occupied != stamped, "smoothing changed the occupied set (\(stamped) -> \(occupied))")

// The document owns this grid: destroying it must be refused, not obeyed.
check(clay_voxel_grid_destroy(grid) != CLAY_OK,
      "destroying a borrowed layer handle is refused")
check(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK,
      "the document survived that attempt")

// -- meshing -----------------------------------------------------------------

var meshParams = clay_mesh_params()
meshParams.struct_size = UInt32(MemoryLayout<clay_mesh_params>.size)
meshParams.resolution = 48

var mesh: OpaquePointer? = nil
check(clay_document_mesh(doc, &meshParams, &mesh) == CLAY_OK, "meshed the document")
if let mesh = mesh {
    let vertices = clay_mesh_vertex_count(mesh)
    let indices = clay_mesh_index_count(mesh)
    check(vertices > 0 && indices > 0, "the mesh has geometry (\(vertices) verts, \(indices) indices)")
    check(clay_mesh_positions(mesh) != nil, "positions are readable")
    clay_mesh_destroy(mesh)
}

// -- result ------------------------------------------------------------------

if failures == 0 {
    print("swift smoke: OK")
    exit(0)
}
print("swift smoke: \(failures) failure(s)")
exit(1)

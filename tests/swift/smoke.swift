// Swift smoke for the C ABI (c-abi spec: "Swift consumption via SwiftPM").
//
// This is the check that the header is usable from Swift rather than merely
// valid C, and — when run through the simulator — that the engine works on the
// platform ClaySpace actually ships to. It walks the whole surface the app
// needs and asserts results, not just return codes.
//
// Built by tools/check_swift_smoke.sh against the xcframework's macOS slice,
// and against the iOS simulator slice, where it runs inside a booted device.

import Foundation
import claycore

var failures = 0
var checks = 0

func check(_ condition: Bool, _ what: String) {
    checks += 1
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

func evaluate(_ doc: OpaquePointer, _ points: [Float]) -> [Float] {
    var pts = points
    var out = [Float](repeating: 0, count: points.count / 3)
    let rc = clay_eval_points(doc, nil, &pts, out.count, &out, nil)
    check(rc == CLAY_OK, "evaluated \(out.count) point(s)")
    return out
}

// -- version -----------------------------------------------------------------

var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
clay_version(&major, &minor, &patch)
print("claycore ABI \(major).\(minor).\(patch)")
check(major == CLAY_ABI_MAJOR && minor == CLAY_ABI_MINOR,
      "the linked library matches the header it was compiled against")

guard let doc = clay_document_create() else {
    print("FAIL could not create a document")
    exit(1)
}
defer { clay_document_destroy(doc) }

var layer: clay_layer_id = 0
check(clay_add_sdf_layer(doc, "body", &layer) == CLAY_OK, "added an SDF layer")

// -- a spread of primitives --------------------------------------------------

let primParams: [(Int32, [Float])] = [
    (Int32(CLAY_PRIM_SPHERE.rawValue), [0.4]),
    (Int32(CLAY_PRIM_BOX.rawValue), [0.3, 0.3, 0.3]),
    (Int32(CLAY_PRIM_ROUND_BOX.rawValue), [0.3, 0.3, 0.3, 0.05]),
    (Int32(CLAY_PRIM_BOX_FRAME.rawValue), [0.3, 0.3, 0.3, 0.05]),
    (Int32(CLAY_PRIM_TORUS.rawValue), [0.4, 0.12]),
    (Int32(CLAY_PRIM_CAPSULE.rawValue), [-0.2, 0, 0, 0.2, 0, 0, 0.15]),
    (Int32(CLAY_PRIM_CAPPED_CYLINDER.rawValue), [0.3, 0.4]),
    (Int32(CLAY_PRIM_ROUNDED_CYLINDER.rawValue), [0.3, 0.08, 0.4]),
    (Int32(CLAY_PRIM_CAPPED_CONE.rawValue), [0.4, 0.35, 0.1]),
    (Int32(CLAY_PRIM_ROUND_CONE.rawValue), [0.3, 0.1, 0.5]),
    (Int32(CLAY_PRIM_ELLIPSOID.rawValue), [0.5, 0.3, 0.4]),
    (Int32(CLAY_PRIM_OCTAHEDRON.rawValue), [0.5]),
    (Int32(CLAY_PRIM_HEX_PRISM.rawValue), [0.4, 0.2]),
    (Int32(CLAY_PRIM_PYRAMID.rawValue), [0.6]),
]

var primitivesBuilt = 0
for (prim, params) in primParams {
    var p = params
    guard let item = clay_item_create(prim, &p, p.count) else {
        print("  FAIL could not build primitive \(prim): \(lastError())")
        failures += 1
        continue
    }
    var node: clay_node_id = 0
    if clay_layer_add_item(doc, layer, item, &node) == CLAY_OK { primitivesBuilt += 1 }
    clay_item_destroy(item)
}
check(primitivesBuilt == primParams.count,
      "built \(primitivesBuilt) of \(primParams.count) primitives")

// -- every deformer, including the newly reachable opcodes -------------------

var sphereParams: [Float] = [0.5]
guard let deformed = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &sphereParams, 1) else {
    print("FAIL could not create the deformer item")
    exit(1)
}
var twist: [Float] = [1.2]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_TWIST.rawValue), &twist, 1, 0) == CLAY_OK,
      "twist")
var bend: [Float] = [0.5]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_BEND.rawValue), &bend, 1, 0) == CLAY_OK,
      "bend")
var taper: [Float] = [-0.5, 0.5, 1.0, 0.4]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_TAPER.rawValue), &taper, 4, 2) == CLAY_OK,
      "taper with an easing curve")
var displace: [Float] = [0.05, 6.0]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_DISPLACE.rawValue), &displace, 2, 0)
        == CLAY_OK, "displace")
var wrap: [Float] = [-3.14159265, 3.14159265]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_WRAP_AROUND.rawValue), &wrap, 2, 0)
        == CLAY_OK, "wrap_around")
var elongate: [Float] = [0.3, 0.0, 0.0]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_ELONGATE.rawValue), &elongate, 3, 0)
        == CLAY_OK, "elongate")
var bendLinear: [Float] = [0, -1, 0, 0, 1, 0, 0.3, 0, 0]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_BEND_LINEAR.rawValue), &bendLinear, 9, 3)
        == CLAY_OK, "bend_linear")
var bendRadial: [Float] = [0.1, 0.9, 0.2]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_BEND_RADIAL.rawValue), &bendRadial, 3, 5)
        == CLAY_OK, "bend_radial")
var elongateAxis: [Float] = [0.2, 0.0, 0.1]
check(clay_item_add_deformer(deformed, Int32(CLAY_DEFORM_ELONGATE_AXIS.rawValue),
                             &elongateAxis, 3, 0) == CLAY_OK, "elongate_axis")

check(clay_item_set_repeat_radial(deformed, 5, 0.9) == CLAY_OK, "radial repetition")
var tint: [Float] = [0.3, 0.6, 0.9]
check(clay_item_set_color(deformed, &tint) == CLAY_OK, "colour")

var deformedNode: clay_node_id = 0
check(clay_layer_add_item(doc, layer, deformed, &deformedNode) == CLAY_OK,
      "added an item carrying all nine deformers")
clay_item_destroy(deformed)

// -- editing a placed node, and undo -----------------------------------------

check(clay_document_enable_undo(doc) == CLAY_OK, "enabled undo")

// The layer holds many items, so one node's move need not change the union
// minimum at any single point — sample a spread and require some difference.
var probes: [Float] = []
for i in 0..<64 {
    let f = Float(i)
    probes += [sin(f * 1.7) * 1.5, cos(f * 2.3) * 1.5, sin(f * 0.9) * 1.5]
}
let beforeEdit = evaluate(doc, probes)

var moved: [Float] = [0.2, 0.1, 0]
var axis: [Float] = [0, 1, 0]
check(clay_layer_set_transform(doc, layer, deformedNode, &moved, &axis, 0.3, 1.0) == CLAY_OK,
      "retransformed a placed node")
let afterEdit = evaluate(doc, probes)
check(zip(beforeEdit, afterEdit).contains { $0 != $1 }, "the edit changed the field")

var undone: Int32 = 0
check(clay_document_undo(doc, &undone) == CLAY_OK && undone == 1, "undid it")
check(evaluate(doc, probes) == beforeEdit, "undo restored the field exactly")

var enabled: Int32 = 0, undoDepth = 0, redoDepth = 0
check(clay_document_undo_state(doc, &enabled, &undoDepth, &redoDepth) == CLAY_OK
        && enabled == 1 && redoDepth == 1, "undo state reports enabled with a redo available")

// -- voxel sculpting ---------------------------------------------------------

var voxelLayer: clay_layer_id = 0
var grid: OpaquePointer? = nil
check(clay_document_add_voxel_layer(doc, "blocks", 0.1, &voxelLayer, &grid) == CLAY_OK,
      "added a voxel layer")

var index: Int32 = 0
var rgb: [Float] = [0.9, 0.5, 0.2]
check(clay_voxel_palette_add(grid, &rgb, &index) == CLAY_OK && index > 0, "palette entry")

var brush = clay_brush_params()
brush.struct_size = UInt32(MemoryLayout<clay_brush_params>.size)
brush.size = 9
brush.shape = Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue)
brush.falloff = Int32(CLAY_BRUSH_FALLOFF_SMOOTH.rawValue)
brush.strength = 0.8
brush.seed = 7

var cell: [Int32] = [0, 0, 0]
check(clay_voxel_set_brush(grid, &cell, &brush, index) == CLAY_OK, "sphere brush with falloff")

var occupied = 0
check(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied > 0,
      "the brush left \(occupied) cells")

let stamped = occupied
check(clay_voxel_sculpt_smooth(grid, &cell, &brush) == CLAY_OK, "sculpt smooth")
check(clay_voxel_sculpt_inflate(grid, &cell, &brush, 1) == CLAY_OK, "sculpt inflate")
var up: [Float] = [0, 1, 0]
check(clay_voxel_sculpt_flatten(grid, &cell, &brush, &up, 0) == CLAY_OK, "sculpt flatten")
check(clay_voxel_sculpt_pinch(grid, &cell, &brush) == CLAY_OK, "sculpt pinch")
check(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied != stamped,
      "sculpting changed the occupied set (\(stamped) -> \(occupied))")

check(clay_voxel_grid_destroy(grid) != CLAY_OK, "destroying a borrowed layer handle is refused")

// -- meshing -----------------------------------------------------------------

var meshParams = clay_mesh_params()
meshParams.struct_size = UInt32(MemoryLayout<clay_mesh_params>.size)
meshParams.resolution = 48

var mesh: OpaquePointer? = nil
check(clay_document_mesh(doc, &meshParams, &mesh) == CLAY_OK, "meshed the document")
if let mesh = mesh {
    let vertices = clay_mesh_vertex_count(mesh)
    let indices = clay_mesh_index_count(mesh)
    check(vertices > 0 && indices > 0, "mesh has \(vertices) verts / \(indices) indices")
    var watertight: Int32 = 0, manifold: Int32 = 0
    check(clay_mesh_validate(mesh, &watertight, &manifold) == CLAY_OK, "validated the mesh")
    clay_mesh_destroy(mesh)
}

// -- picking and I/O ---------------------------------------------------------

var origin: [Float] = [0, 0, 3]
var direction: [Float] = [0, 0, -1]
var hit: Int32 = 0
var t: Float = 0
var position = [Float](repeating: 0, count: 3)
var normal = [Float](repeating: 0, count: 3)
check(clay_raycast(doc, &origin, &direction, &hit, &t, &position, &normal) == CLAY_OK,
      "raycast returned")
check(hit == 1, "the ray hit the model")

// NSTemporaryDirectory is writable inside the simulator sandbox, unlike cwd.
let outPath = NSTemporaryDirectory() + "clay_swift_smoke.clayspace"
check(clay_document_save(doc, outPath) == CLAY_OK, "saved a .clayspace")
var reloaded: OpaquePointer? = nil
check(clay_document_load(outPath, &reloaded) == CLAY_OK, "loaded it back")
if let reloaded = reloaded {
    let after = evaluate(reloaded, [0, 0, 0])
    check(after[0] == evaluate(doc, [0, 0, 0])[0], "the round trip preserved the field")
    clay_document_destroy(reloaded)
}
try? FileManager.default.removeItem(atPath: outPath)

// -- result ------------------------------------------------------------------

print("\n\(checks - failures)/\(checks) checks passed")
if failures == 0 {
    print("swift smoke: OK")
    exit(0)
}
print("swift smoke: \(failures) failure(s)")
exit(1)

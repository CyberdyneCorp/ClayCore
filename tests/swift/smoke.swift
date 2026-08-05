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

// -- curves ------------------------------------------------------------------

var curveLayer: clay_layer_id = 0
check(clay_add_sdf_layer(doc, "curve", &curveLayer) == CLAY_OK, "added a curve layer")

// Placed well clear of the model the rest of this file builds: the document's
// field is every layer combined, so a probe near the origin would be answering
// a question about the body, not about the curve.
let curveX: Float = 10.0
var curvePoints: [Float] = [curveX - 1, 0, 0, 0.05,
                            curveX + 0, 1, 0, 0.05,
                            curveX + 1, 0, 0, 0.05,
                            curveX + 0, -1, 0, 0.05]
var pointTypes: [Int32] = Array(repeating: Int32(CLAY_POINT_SPLINE.rawValue), count: 4)

guard let curveItem = clay_item_create(Int32(CLAY_PRIM_STROKE.rawValue), nil, 0) else {
    check(false, "created a curve item")
    exit(1)
}
check(clay_item_set_curve_points(curveItem, &curvePoints, 4, &pointTypes, nil, nil) == CLAY_OK,
      "typed control points")
check(clay_item_set_curve(curveItem, 0, 0.01) == CLAY_OK, "curve tolerance")
check(clay_item_set_curve(curveItem, 0, 0) != CLAY_OK, "a tolerance of 0 is refused")
var curveNode: clay_node_id = 0
check(clay_layer_add_item(doc, curveLayer, curveItem, &curveNode) == CLAY_OK && curveNode != 0,
      "placed the curve")
clay_item_destroy(curveItem)

// The Catmull-Rom midpoint of the first span, ~0.088 outside the chord a hard
// chain would draw, so this only reads as inside if the curve really curved.
let bulgeAt: [Float] = [curveX - 0.5625, 0.5625, 0.0]
let bulge = evaluate(doc, bulgeAt)
check(bulge[0] < 0, "the curve bulges outside its control polygon")

// Editing it is an ordinary edit, so it undoes as one.
var hardTypes: [Int32] = Array(repeating: Int32(CLAY_POINT_HARD.rawValue), count: 4)
check(clay_layer_set_stroke_points(doc, curveLayer, curveNode, &curvePoints, 4, &hardTypes,
                                   nil, nil, 0, 0.01) == CLAY_OK, "replaced the points")
check(evaluate(doc, bulgeAt)[0] > 0, "the hard chain no longer reaches the bulge")
check(clay_document_undo(doc, &undone) == CLAY_OK && undone == 1, "undid the curve edit")
check(evaluate(doc, bulgeAt)[0] == bulge[0], "undo restored the curve exactly")

// -- layer protection --------------------------------------------------------

var isGhost: Int32 = 1
var isLocked: Int32 = 1
check(clay_document_layer_protection(doc, layer, &isGhost, &isLocked) == CLAY_OK
      && isGhost == 0 && isLocked == 0, "layers start unprotected")

check(clay_document_set_layer_protection(doc, layer, 0, 1) == CLAY_OK, "locked the layer")
check(clay_layer_add_item(doc, layer, deformed, nil) == CLAY_ERROR_INVALID_ARGUMENT,
      "a locked layer refuses an edit rather than dropping it")
check(clay_document_set_layer_protection(doc, layer, 0, 0) == CLAY_OK,
      "unlocking works, so locking is not permanent")

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

// -- masks -------------------------------------------------------------------

var mask: OpaquePointer? = nil
check(clay_document_add_mask(doc, voxelLayer, 0.1, &mask) == CLAY_OK, "attached a mask")

var maskBrush = brush
maskBrush.mask = nil  // a mask does not gate itself
var maskCentre: [Int32] = [0, 0, 0]
maskBrush.strength = 1.0
maskBrush.falloff = Int32(CLAY_BRUSH_FALLOFF_CONSTANT.rawValue)
maskBrush.size = 21
check(clay_mask_paint_cell(mask, &maskCentre, &maskBrush, 1.0) == CLAY_OK, "painted the mask")

var painted = 0
check(clay_mask_painted_count(mask, &painted) == CLAY_OK && painted > 0,
      "the mask covers \(painted) cells")
var maskValue: Float = -1
var maskPoint: [Float] = [0.05, 0.05, 0.05]
check(clay_mask_sample(mask, &maskPoint, &maskValue) == CLAY_OK && maskValue == 1.0,
      "sampled the mask in world space")

// A fully masked region is frozen: the same brush that changed the grid above
// now leaves it exactly as it is.
let beforeMasked = occupied
var maskedBrush = brush
maskedBrush.mask = mask
check(clay_voxel_erase_brush(grid, &cell, &maskedBrush) == CLAY_OK, "masked erase ran")
check(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied == beforeMasked,
      "the mask froze the region (\(beforeMasked) cells untouched)")
check(clay_voxel_erase_brush(grid, &cell, &brush) == CLAY_OK, "unmasked erase ran")
check(clay_voxel_occupied_count(grid, &occupied) == CLAY_OK && occupied < beforeMasked,
      "without the mask the same brush cuts (\(beforeMasked) -> \(occupied))")

check(clay_mask_expand(mask, 1) == CLAY_OK, "mask expand")
check(clay_mask_contract(mask, 1) == CLAY_OK, "mask contract")
check(clay_mask_smooth(mask, 1) == CLAY_OK, "mask smooth")
check(clay_mask_invert(mask) == CLAY_OK, "mask invert")
check(clay_mask_destroy(mask) != CLAY_OK, "destroying a borrowed mask handle is refused")

// Re-read after the region ops: they change the mask, and the round-trip check
// below compares against whatever it looks like at save time.
check(clay_mask_painted_count(mask, &painted) == CLAY_OK, "read the mask back")

// -- brush strokes -----------------------------------------------------------

var preset = clay_stroke_preset()
check(clay_stroke_preset_defaults(&preset) == CLAY_OK, "preset defaults")
preset.radius = 0.15
preset.spacing = 0.5

// (N, 5) packed: position, pressure, tilt.
var strokeSamples: [Float] = []
for i in 0...30 {
    strokeSamples += [-0.75 + Float(i) * 0.05, 0.6, 0, 1, 0]
}
let strokeCount = strokeSamples.count / 5

var stampCount = 0
check(clay_stroke_resolve(&strokeSamples, strokeCount, &preset, nil, &stampCount) == CLAY_OK
      && stampCount > 3, "resolved \(stampCount) stamps")
var stamps = [clay_stamp](repeating: clay_stamp(), count: stampCount)
var stampCapacity = stampCount
check(clay_stroke_resolve(&strokeSamples, strokeCount, &preset, &stamps, &stampCapacity) == CLAY_OK,
      "read the stamps back")
check(stamps[0].radius > 0 && stamps[stampCount - 1].along > 0.9, "stamps carry radius and along")

var presetBytes = 0
check(clay_stroke_preset_serialize(&preset, nil, &presetBytes) == CLAY_OK && presetBytes > 0,
      "preset serializes to \(presetBytes) bytes")
var presetBuffer = [UInt8](repeating: 0, count: presetBytes)
var presetCapacity = presetBytes
check(clay_stroke_preset_serialize(&preset, &presetBuffer, &presetCapacity) == CLAY_OK,
      "wrote the preset")
var presetBack = clay_stroke_preset()
check(clay_stroke_preset_deserialize(&presetBuffer, presetBytes, &presetBack) == CLAY_OK
      && presetBack.radius == preset.radius, "preset round trip")
presetBuffer[0] = UInt8(clay_stroke_preset_version() + 1)
check(clay_stroke_preset_deserialize(&presetBuffer, presetBytes, &presetBack) != CLAY_OK,
      "a newer preset schema is refused rather than guessed at")

// A fresh template: the builder used earlier was destroyed once its edit was
// added, and a destroyed handle is not a stamp template.
var stampParams: [Float] = [1.0]
guard let stampItem = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &stampParams, 1) else {
    check(false, "created a stamp template")
    exit(1)
}
check(clay_item_set_blend(stampItem, Int32(CLAY_BLEND_QUADRATIC.rawValue), 0.1) == CLAY_OK,
      "stamp template blends")

var strokeNodeCount = 0
let strokeResult = clay_layer_apply_stroke(doc, layer, &strokeSamples, strokeCount, &preset,
                                           stampItem, nil, nil, &strokeNodeCount)
check(strokeResult == CLAY_OK && strokeNodeCount == stampCount,
      "stroked \(strokeNodeCount) of \(stampCount) stamps onto the layer as edits")
clay_item_destroy(stampItem)  // the nodes hold copies, not the builder
var afterStroke: Int32 = 0
var strokeUndo = 0
var strokeRedo = 0
check(clay_document_undo_state(doc, &afterStroke, &strokeUndo, &strokeRedo) == CLAY_OK,
      "read undo state")
check(clay_document_undo(doc, &undone) == CLAY_OK && undone == 1,
      "the whole stroke undid as one step")

var strokeApplied = 0
check(clay_voxel_apply_stroke(grid, &strokeSamples, strokeCount, &preset, index,
                              Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue),
                              Int32(CLAY_BRUSH_FALLOFF_SMOOTH.rawValue), nil,
                              &strokeApplied) == CLAY_OK && strokeApplied == stampCount,
      "stroked \(strokeApplied) stamps into the voxel layer")
// A caller-owned mask painted over the stroke's own path, so the freeze is
// something this check can actually assert rather than hope for.
let freeze = clay_mask_create(0.05)
check(freeze != nil, "created a mask")
for i in 0...30 {
    var point: [Float] = [-0.75 + Float(i) * 0.05, 0.6, 0]
    var stamped: Float = 0
    _ = clay_mask_sample(freeze, &point, &stamped)
    var frozenBrush = clay_brush_params()
    frozenBrush.struct_size = UInt32(MemoryLayout<clay_brush_params>.size)
    frozenBrush.size = 13
    frozenBrush.shape = Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue)
    frozenBrush.falloff = Int32(CLAY_BRUSH_FALLOFF_CONSTANT.rawValue)
    frozenBrush.strength = 1.0
    _ = clay_mask_paint(freeze, &point, &frozenBrush, 1.0)
}
check(clay_voxel_apply_stroke(grid, &strokeSamples, strokeCount, &preset, index,
                              Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue),
                              Int32(CLAY_BRUSH_FALLOFF_SMOOTH.rawValue), freeze,
                              &strokeApplied) == CLAY_OK && strokeApplied == 0,
      "a mask over the path froze the whole stroke (\(stampCount) stamps dropped)")
check(clay_mask_destroy(freeze) == CLAY_OK, "destroyed the caller-owned mask")

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

    var reloadedMask: OpaquePointer? = nil
    check(clay_document_mask(reloaded, voxelLayer, &reloadedMask) == CLAY_OK,
          "the mask came back with the document")
    var reloadedPainted = 0
    check(clay_mask_painted_count(reloadedMask, &reloadedPainted) == CLAY_OK
          && reloadedPainted == painted,
          "the round trip preserved the mask (\(reloadedPainted) cells)")

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

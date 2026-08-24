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
import Metal
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

// -- backends ----------------------------------------------------------------
//
// Which backends the SHIPPED artifact carries, asserted rather than assumed.
// The xcframework went out CPU-only for several releases: CLAY_BACKEND_METAL
// defaults off and the build script never passed it, so every Apple host was
// pinned to CPU field evaluation and nothing here said so. A consumer of a
// prebuilt static library cannot add the backend afterwards, so if this check
// fails the artifact is wrong, not the app.

func registeredBackends() -> [String] {
    var size = 0
    guard clay_list_backends(nil, &size) == CLAY_OK, size > 0 else { return [] }
    var buffer = [CChar](repeating: 0, count: size)
    guard clay_list_backends(&buffer, &size) == CLAY_OK else { return [] }
    // Comma-separated with no spaces, per clay_list_backends.
    let names = buffer.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    return names.split(separator: ",").map(String.init)
}

let backends = registeredBackends()
print("backends: \(backends.joined(separator: ", "))")
check(backends.contains("cpu"), "the CPU backend is registered")
// Metal registers for one reason and fails to register for three, and
// "backends: cpu" does not say which. MetalBackend::init() returns false when
// CreateSystemDefaultDevice() is null (no GPU — environmental), when the
// embedded metallib does not load (a real defect in the artifact we ship), or
// when the command queue cannot be made. Collapsing those into one assertion
// is what made this smoke fail the v0.27.0 and v0.27.1 releases on a runner
// with no GPU, and it would equally have hidden a broken metallib behind the
// same message.
//
// So: ask whether a DEVICE exists, and report which case this is. Where one
// exists, the backend must register — if it does not, the metallib is broken
// and that is a failure, not an environment. Where none exists there is
// nothing to register against, and what the ARTIFACT ships is gated where it
// is decidable: tools/build_xcframework.sh fails the build outright if a
// slice's merged archive carries no clay_metallib symbol.
if let device = MTLCreateSystemDefaultDevice() {
    print("  Metal device: \(device.name)")
    check(backends.contains("metal"),
          "the Metal backend registered — this machine HAS a Metal device, so a "
          + "missing backend would mean the embedded metallib failed to load")
} else {
    print("  SKIP no Metal device on this machine, so there is nothing for the "
          + "backend to register against. This is the environment, not the "
          + "artifact: whether the xcframework SHIPS the metallib is gated by "
          + "tools/build_xcframework.sh, which fails the build if it does not.")
}

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

// -- loft --------------------------------------------------------------------

var loftLayer: clay_layer_id = 0
check(clay_add_sdf_layer(doc, "loft", &loftLayer) == CLAY_OK, "added a loft layer")

var loftParams: [Float] = [1.0, 0.0]   // half-depth, ease
guard let loftItem = clay_item_create(Int32(CLAY_PRIM_LOFT.rawValue), &loftParams, 2) else {
    check(false, "created a loft item")
    exit(1)
}
// Placed clear of everything else: the document's field is every layer
// combined, so a probe near the origin would answer about the body.
let loftY: Float = 20.0
var loftPos: [Float] = [0, loftY, 0]
check(clay_item_set_position(loftItem, &loftPos) == CLAY_OK, "placed the loft")

check(clay_layer_add_item(doc, loftLayer, loftItem, nil) != CLAY_OK,
      "a loft with no profiles is refused")
var wideCircle: [Float] = [1.0]
check(clay_item_add_loft_profile(loftItem, Int32(CLAY_PROFILE_CIRCLE.rawValue),
                                 &wideCircle, 1, nil, 0) == CLAY_OK, "bottom profile")
var square: [Float] = [-0.3, -0.3, 0.3, -0.3, 0.3, 0.3, -0.3, 0.3]
check(clay_item_add_loft_profile(loftItem, Int32(CLAY_PROFILE_POLYGON.rawValue),
                                 nil, 0, &square, 4) == CLAY_OK, "top profile")
check(clay_layer_add_item(doc, loftLayer, loftItem, nil) == CLAY_OK, "placed it with two")
clay_item_destroy(loftItem)

// The loft is a circle at the bottom and a small square at the top: a point
// well outside the square but inside the circle separates the two ends.
// The lift axis is Z, not Y — the profile lives in (x, y) and cop_extrude
// caps along z. The item sits at y = loftY only to keep it clear of the rest
// of the model.
check(evaluate(doc, [0.85, loftY, -0.99])[0] < 0, "the loft's near end is the circle")
check(evaluate(doc, [0.85, loftY, 0.99])[0] > 0, "and its far end is the smaller square")

var loftScale: Float = 0
check(clay_layer_safe_step_scale(doc, loftLayer, &loftScale) == CLAY_OK && loftScale < 1.0,
      "a loft drops the safe step scale to \(loftScale) — it is a bound, not a distance")

// -- swept along a guide -----------------------------------------------------

var sweptLayer: clay_layer_id = 0
check(clay_add_sdf_layer(doc, "swept", &sweptLayer) == CLAY_OK, "added a swept layer")

var sweptEase: [Float] = [0]
guard let sweptItem = clay_item_create(Int32(CLAY_PRIM_SWEPT.rawValue), &sweptEase, 1) else {
    check(false, "created a swept item")
    exit(1)
}
// Clear of everything else, as with the loft above.
let sweptY: Float = 30.0
var sweptGuide: [Float] = [-1, sweptY, 0, 0,
                            0, sweptY + 0.7, 0, 0,
                            1, sweptY, 0, 0]
var sweptTypes: [Int32] = Array(repeating: Int32(CLAY_POINT_SPLINE.rawValue), count: 3)
check(clay_item_set_curve_points(sweptItem, &sweptGuide, 3, &sweptTypes, nil, nil) == CLAY_OK,
      "guide points")
check(clay_item_set_curve(sweptItem, 0, 0.02) == CLAY_OK, "guide tolerance")
check(clay_item_set_curve(sweptItem, 1, 0.02) != CLAY_OK,
      "a closed guide is refused rather than ignored")

check(clay_layer_add_item(doc, sweptLayer, sweptItem, nil) != CLAY_OK,
      "a sweep with no profiles is refused")
var sweptWide: [Float] = [0.3]
var sweptNarrow: [Float] = [0.1]
check(clay_item_add_loft_profile(sweptItem, Int32(CLAY_PROFILE_CIRCLE.rawValue),
                                 &sweptWide, 1, nil, 0) == CLAY_OK, "start profile")
check(clay_item_add_loft_profile(sweptItem, Int32(CLAY_PROFILE_CIRCLE.rawValue),
                                 &sweptNarrow, 1, nil, 0) == CLAY_OK, "end profile")
check(clay_layer_add_item(doc, sweptLayer, sweptItem, nil) == CLAY_OK, "placed the sweep")
clay_item_destroy(sweptItem)

// On the guide there is material; the sweep tapers, so a point that clears the
// narrow end does not.
check(evaluate(doc, [0, sweptY + 0.7, 0])[0] < 0, "the sweep follows its guide")
check(evaluate(doc, [0.95, sweptY + 0.2, 0])[0] > 0, "and it tapers toward the end")

var sweptScale: Float = 0
check(clay_layer_safe_step_scale(doc, sweptLayer, &sweptScale) == CLAY_OK && sweptScale < 1.0,
      "a sweep drops the safe step scale to \(sweptScale) — curvature compresses space")

// -- the cut tool ------------------------------------------------------------

// A block of its own, well clear of the rest of the model: the document's
// field is every layer combined, so a probe near the origin would be answering
// a question about the body rather than about the cut.
let cutX: Float = -10.0
var cutLayer: clay_layer_id = 0
check(clay_add_sdf_layer(doc, "cut", &cutLayer) == CLAY_OK, "added a cut layer")

var blockSize: [Float] = [1, 1, 1]
guard let blockItem = clay_item_create(Int32(CLAY_PRIM_BOX.rawValue), &blockSize, 3) else {
    check(false, "created the block")
    exit(1)
}
var blockPos: [Float] = [cutX, 0, 0]
check(clay_item_set_position(blockItem, &blockPos) == CLAY_OK, "placed it")
check(clay_layer_add_item(doc, cutLayer, blockItem, nil) == CLAY_OK, "added the block")
clay_item_destroy(blockItem)
check(evaluate(doc, [cutX, 0, 0])[0] < 0, "the block is solid before the cut")

var cutDesc = clay_cut_desc()
cutDesc.struct_size = UInt32(MemoryLayout<clay_cut_desc>.size)
cutDesc.origin = (cutX, 0, -4)
cutDesc.right = (1, 0, 0)
cutDesc.up = (0, 1, 0)
cutDesc.forward = (0, 0, 1)
cutDesc.shape = Int32(CLAY_CUT_CIRCLE.rawValue)
cutDesc.radius = 0.4
cutDesc.region_min = (cutX - 1, -1, -1)
cutDesc.region_max = (cutX + 1, 1, 1)

guard let cutItem = clay_cut_create(&cutDesc, nil, 0) else {
    check(false, "resolved the cut")
    exit(1)
}
check(clay_item_set_op(cutItem, Int32(CLAY_OP_SUBTRACT.rawValue)) == CLAY_OK, "cut subtracts")
check(clay_layer_add_item(doc, cutLayer, cutItem, nil) == CLAY_OK, "placed the cut")
clay_item_destroy(cutItem)

check(evaluate(doc, [cutX, 0, 0])[0] > 0, "the cut went through the middle")
check(evaluate(doc, [cutX, 0, 0.9])[0] > 0, "...and out the far face")
check(evaluate(doc, [cutX + 0.8, 0.8, 0])[0] < 0, "and left the corners alone")

// A frame that is not orthonormal is refused rather than squared up.
var skewed = cutDesc
skewed.up = (0.5, 0.5, 0)
check(clay_cut_create(&skewed, nil, 0) == nil, "a skewed frame is refused")

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

// And reading it back: what a host that reloaded this document would have to do
// before it could edit a curve it did not author. Size query, then fill.
var readCount = 0
var readClosed: Int32 = -1
var readTolerance: Float = -1
check(clay_layer_stroke_points(doc, curveLayer, curveNode, nil, &readCount, nil, nil, nil,
                               &readClosed, &readTolerance) == CLAY_OK
      && readCount == 4 && readClosed == 0 && readTolerance == 0.01,
      "sized the placed curve, and learned it is open")
var readPoints = [Float](repeating: 0, count: readCount * 4)
var readTypes = [Int32](repeating: -1, count: readCount)
var readCapacity = readCount
let filled = clay_layer_stroke_points(doc, curveLayer, curveNode, &readPoints, &readCapacity,
                                      &readTypes, nil, nil, nil, nil)
let allSpline = readTypes == pointTypes
check(filled == CLAY_OK && readCapacity == 4 && readPoints == curvePoints && allSpline,
      "read the placed curve back exactly as it was authored")
var tooSmall = 1
check(clay_layer_stroke_points(doc, curveLayer, curveNode, &readPoints, &tooSmall, nil, nil, nil,
                               nil, nil) == CLAY_ERROR_BUFFER_TOO_SMALL && tooSmall == 4,
      "a short buffer reports what it needed")

// -- layer protection --------------------------------------------------------

var isGhost: Int32 = 1
var isLocked: Int32 = 1
check(clay_document_layer_protection(doc, layer, &isGhost, &isLocked) == CLAY_OK
      && isGhost == 0 && isLocked == 0, "layers start unprotected")

check(clay_document_set_layer_protection(doc, layer, 0, 1) == CLAY_OK, "locked the layer")
// A fresh item: `deformed` was destroyed once its edit was added, and reading
// a destroyed handle is what made this file abort intermittently — freed
// memory is sometimes still readable and sometimes not.
var lockProbeParams: [Float] = [0.3]
guard let lockProbe = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &lockProbeParams, 1)
else {
    check(false, "created a probe item")
    exit(1)
}
check(clay_layer_add_item(doc, layer, lockProbe, nil) == CLAY_ERROR_INVALID_ARGUMENT,
      "a locked layer refuses an edit rather than dropping it")
clay_item_destroy(lockProbe)
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

// -- the remaining verbs, and repair -----------------------------------------

// On a grid of their own: the checks after this one assert counts on `grid`,
// and sculpting it here would move the ground under them.
guard let verbGrid = clay_voxel_grid_create(0.1) else {
    check(false, "created a grid for the verbs")
    exit(1)
}
var slabRGB: [Float] = [0.6, 0.6, 0.65]
var slabIndex: Int32 = 0
check(clay_voxel_palette_add(verbGrid, &slabRGB, &slabIndex) == CLAY_OK, "slab colour")
var slabLo: [Int32] = [-8, 0, -8]
var slabHi: [Int32] = [8, 3, 8]
check(clay_voxel_fill_box(verbGrid, &slabLo, &slabHi, slabIndex) == CLAY_OK, "filled a slab")

var verbAt: [Int32] = [0, 3, 0]
check(clay_voxel_sculpt_fill_cavities(verbGrid, &verbAt, &brush, 1) == CLAY_OK, "fill cavities")
check(clay_voxel_sculpt_fill_cavities(verbGrid, &verbAt, &brush, 0) != CLAY_OK,
      "zero passes is refused")
check(clay_voxel_sculpt_scrape(verbGrid, &verbAt, &brush, &up, 0) == CLAY_OK, "scrape")
var smudgeBy: [Float] = [0.2, 0, 0]
check(clay_voxel_sculpt_smudge(verbGrid, &verbAt, &brush, &smudgeBy) == CLAY_OK, "smudge")
var alphaStamp = [Float](repeating: 1.0, count: 16)
check(clay_voxel_sculpt_carve_alpha(verbGrid, &verbAt, &brush, &alphaStamp, 4, 4, &up, 0)
      == CLAY_OK, "carve with an alpha")
var verbCells = 0
check(clay_voxel_occupied_count(verbGrid, &verbCells) == CLAY_OK && verbCells > 0,
      "the verbs left \(verbCells) cells")
check(clay_voxel_grid_destroy(verbGrid) == CLAY_OK, "destroyed the verb grid")

// A hollow shell of its own, so the report is about a known geometry rather
// than about whatever the sculpting above left behind.
guard let shellGrid = clay_voxel_grid_create(0.1) else {
    check(false, "created a shell grid")
    exit(1)
}
var shellRGB: [Float] = [0.8, 0.4, 0.2]
var shellIndex: Int32 = 0
check(clay_voxel_palette_add(shellGrid, &shellRGB, &shellIndex) == CLAY_OK, "shell colour")
var shellLo: [Int32] = [-5, -5, -5]
var shellHi: [Int32] = [5, 5, 5]
check(clay_voxel_fill_box(shellGrid, &shellLo, &shellHi, shellIndex) == CLAY_OK, "filled a box")
var holeLo: [Int32] = [-4, -4, -4]
var holeHi: [Int32] = [4, 4, 4]
check(clay_voxel_fill_box(shellGrid, &holeLo, &holeHi, 0) == CLAY_OK, "hollowed it")

var repairReport = clay_repair_report()
repairReport.struct_size = UInt32(MemoryLayout<clay_repair_report>.size)
check(clay_voxel_repair_report(shellGrid, &repairReport) == CLAY_OK
      && repairReport.enclosed_voids == 1 && repairReport.airtight == 0,
      "the shell reports one enclosed void of \(repairReport.void_cells) cells")

check(clay_voxel_repair_close_holes(shellGrid, 1, nil) == CLAY_OK, "close holes")
check(clay_voxel_repair_fill_voids(shellGrid, nil) == CLAY_OK, "fill voids")
check(clay_voxel_repair_report(shellGrid, &repairReport) == CLAY_OK
      && repairReport.airtight != 0, "the shell is airtight now")
check(clay_voxel_grid_destroy(shellGrid) == CLAY_OK, "destroyed the shell grid")

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
preset.struct_size = UInt32(MemoryLayout<clay_stroke_preset>.size)
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
// Required since ABI 0.35.0: deserialize fills a descriptor bounded by the size
// the CALLER declares, and refuses rather than writing past a struct it cannot
// measure. A zeroed clay_stroke_preset declares nothing.
presetBack.struct_size = UInt32(MemoryLayout<clay_stroke_preset>.size)
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

// -- the mask brush ----------------------------------------------------------
// Painting a mask with the drag a host already resolved, rather than by looping
// single stamps and re-deriving the spacing itself.

let strokeMask = clay_mask_create(0.05)
check(strokeMask != nil, "created a mask to stroke into")
var maskApplied = 0
check(clay_mask_apply_stroke(strokeMask, &strokeSamples, strokeCount, &preset, 1.0,
                             Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue),
                             Int32(CLAY_BRUSH_FALLOFF_SMOOTH.rawValue),
                             &maskApplied) == CLAY_OK && maskApplied == stampCount,
      "strokeMask a mask along a drag (\(maskApplied) stamps)")
var strokedPainted = 0
check(clay_mask_painted_count(strokeMask, &strokedPainted) == CLAY_OK && strokedPainted > 0,
      "the stroke left \(strokedPainted) masked cells")

// The bounded complement: what clay_mask_invert structurally cannot do.
var boxLo: [Float] = [-1, -1, -1]
var boxHi: [Float] = [1, 1, 1]
check(clay_mask_invert_within(strokeMask, &boxLo, &boxHi) == CLAY_OK,
      "took the complement over a box")
var farPoint: [Float] = [0.9, 0.9, 0.9]
var farValue: Float = 0
check(clay_mask_sample(strokeMask, &farPoint, &farValue) == CLAY_OK && farValue > 0.9,
      "everything else in the box is frozen now")

var fillValue: Float = 0
check(clay_mask_fill(strokeMask, &boxLo, &boxHi, 0.0) == CLAY_OK, "released the box")
check(clay_mask_sample(strokeMask, &farPoint, &fillValue) == CLAY_OK && fillValue < 0.1,
      "filling with zero released it")
check(clay_mask_destroy(strokeMask) == CLAY_OK, "destroyed the stroked mask")

// -- mask extrude ------------------------------------------------------------
// A plate pulled off a masked patch, on both representations.

let extrudeMask = clay_mask_create(0.03)
check(extrudeMask != nil, "created a mask to extrude from")
var capLo: [Float] = [-0.3, 0.35, -0.3]
var capHi: [Float] = [0.3, 1.2, 0.3]
check(clay_mask_fill(extrudeMask, &capLo, &capHi, 1.0) == CLAY_OK, "masked a cap")

var extrudeParams = clay_mask_extrude_params()
extrudeParams.struct_size = UInt32(MemoryLayout<clay_mask_extrude_params>.size)
extrudeParams.thickness = 0.12
extrudeParams.side = Int32(CLAY_EXTRUDE_OUTWARD.rawValue)

var measured: OpaquePointer? = nil
check(clay_mask_to_field(extrudeMask, 0.5, 0.2, 0.3, 0.0, &measured) == CLAY_OK
        && measured != nil,
      "measured the mask as a distance field")
if let measured = measured { clay_item_destroy(measured) }

var plate: OpaquePointer? = nil
check(clay_document_mask_extrude(doc, layer, extrudeMask, &extrudeParams, &plate) == CLAY_OK
        && plate != nil,
      "extruded a plate off the sdf layer")
if let plate = plate { clay_item_destroy(plate) }

var extract: OpaquePointer? = nil
check(clay_voxel_mask_extrude(grid, extrudeMask, &extrudeParams, &extract) == CLAY_OK
        && extract != nil,
      "extruded a plate off the voxel grid")
if let extract = extract {
    var extractCells = 0
    check(clay_voxel_occupied_count(extract, &extractCells) == CLAY_OK && extractCells > 0,
          "the voxel extract holds \(extractCells) cells")
    check(clay_voxel_grid_destroy(extract) == CLAY_OK, "destroyed the caller-owned extract")
}

// A refusal is typed rather than an empty handle.
let emptyMask = clay_mask_create(0.03)
var refused: OpaquePointer? = nil
check(clay_document_mask_extrude(doc, layer, emptyMask, &extrudeParams, &refused)
        == CLAY_ERROR_INVALID_ARGUMENT,
      "an empty mask is refused with a typed error")
check(clay_mask_destroy(emptyMask) == CLAY_OK, "destroyed the empty mask")
check(clay_mask_destroy(extrudeMask) == CLAY_OK, "destroyed the extrude mask")

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

    // The full report, not two bits of it: a host that is told an export is bad
    // needs the counts that say why.
    var report = clay_validation_report()
    report.struct_size = UInt32(MemoryLayout<clay_validation_report>.size)
    check(clay_mesh_validation_report(mesh, 0, &report) == CLAY_OK, "read the validation report")
    check(report.watertight == watertight && report.manifold == manifold,
          "the report agrees with the two-boolean call")
    check(report.triangles * 3 == indices, "the report counts \(report.triangles) triangles")
    // The pass did not run, so the zero above means "not looked for".
    check(report.intersection_budget == 0, "the self-intersection pass was skipped")

    var volume: Double = 0, area: Double = 0
    check(clay_mesh_measure(mesh, &volume, &area) == CLAY_OK, "measured the mesh")
    check(volume > 0 && area > 0, "volume \(volume), area \(area) — positive means outward")

    // Bytes rather than a path: an iOS host receives documents from a document
    // provider behind a security-scoped URL, not as a stable path it owns.
    var blob: OpaquePointer? = nil
    check(clay_mesh_save_memory(mesh, "ply", &blob) == CLAY_OK, "serialized the mesh to memory")
    if let blob = blob {
        let size = clay_blob_size(blob)
        check(size > 0 && clay_blob_data(blob) != nil, "the blob holds \(size) bytes")
        var reloaded: OpaquePointer? = nil
        check(clay_mesh_load_memory(clay_blob_data(blob), size, "ply", nil, &reloaded) == CLAY_OK,
              "loaded the mesh back from those bytes")
        check(clay_mesh_index_count(reloaded) == indices, "the round trip kept every index")
        clay_mesh_destroy(reloaded)
        clay_blob_destroy(blob)
    }
    clay_mesh_destroy(mesh)
}

// -- surviving a crash -------------------------------------------------------
// A recovery is a snapshot plus the steps since it. The host owns the file; the
// library owns the bytes.
do {
    let session = clay_document_create()
    var sdf: clay_layer_id = 0
    check(clay_add_sdf_layer(session, "body", &sdf) == CLAY_OK, "session layer")
    check(clay_document_enable_undo(session) == CLAY_OK, "undo on")

    var snapshot: OpaquePointer? = nil
    check(clay_document_save_memory(session, &snapshot) == CLAY_OK, "snapshot taken")

    var radius: Float = 0.5
    if let item = clay_item_create(CLAY_PRIM_SPHERE, &radius, 1) {
        var node: clay_node_id = 0
        check(clay_layer_add_item(session, sdf, item, &node) == CLAY_OK, "an edit to recover")
        clay_item_destroy(item)
    }

    var journal: OpaquePointer? = nil
    var nowAt: Int = 0
    check(clay_document_journal_since(session, 0, &journal, &nowAt) == CLAY_OK, "journal taken")
    check(nowAt > 0, "the journal advanced to \(nowAt)")

    if let snapshot = snapshot, let journal = journal {
        var recovered: OpaquePointer? = nil
        check(clay_document_load_memory(clay_blob_data(snapshot), clay_blob_size(snapshot),
                                        &recovered) == CLAY_OK, "snapshot reloaded")
        check(clay_document_enable_undo(recovered) == CLAY_OK, "undo on the recovered document")
        var applied: Int = 0
        var stopped: Int32 = 0
        check(clay_document_replay_journal(recovered, clay_blob_data(journal),
                                           clay_blob_size(journal), &applied, &stopped) == CLAY_OK,
              "journal replayed: \(applied) events")
        check(stopped == 0, "no barrier in the way")
        var nodes: Int = 0
        check(clay_layer_node_count(recovered, sdf, &nodes) == CLAY_OK && nodes == 1,
              "the edit came back")
        clay_document_destroy(recovered)
        clay_blob_destroy(journal)
        clay_blob_destroy(snapshot)
    }
    clay_document_destroy(session)
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

// -- importing a mesh as a field ---------------------------------------------
// The capability an app actually reaches for: a downloaded or scanned model
// turned into something that can be combined, cut and sculpted. The sign comes
// from a generalized winding number, so a mesh that is not watertight — which
// is most of them — still imports sensibly.

do {
    // A closed box as raw triangles, wound outward.
    let h: Float = 0.6
    var positions: [Float] = []
    for i in 0..<8 {
        positions.append((i & 1) != 0 ? h : -h)
        positions.append((i & 2) != 0 ? h : -h)
        positions.append((i & 4) != 0 ? h : -h)
    }
    let faces: [[UInt32]] = [[0, 2, 3, 1], [4, 5, 7, 6], [0, 1, 5, 4],
                             [2, 6, 7, 3], [0, 4, 6, 2], [1, 3, 7, 5]]
    var indices: [UInt32] = []
    for f in faces {
        indices.append(contentsOf: [f[0], f[1], f[2]])
        indices.append(contentsOf: [f[0], f[2], f[3]])
    }

    var boxMesh: OpaquePointer? = nil
    check(clay_mesh_from_triangles(positions, positions.count / 3, indices, indices.count,
                                   &boxMesh) == CLAY_OK,
          "built a mesh from triangles")

    var volumeParams = clay_volume_params()
    volumeParams.struct_size = UInt32(MemoryLayout<clay_volume_params>.size)
    volumeParams.cell_size = 0.05

    var volumeItem: OpaquePointer? = nil
    check(clay_item_volume_from_mesh(boxMesh, &volumeParams, &volumeItem) == CLAY_OK
          && volumeItem != nil,
          "sampled the mesh into an item")

    guard let importDoc = clay_document_create() else {
        check(false, "created a document for the import")
        exit(1)
    }
    var importLayer: clay_layer_id = 0
    check(clay_add_sdf_layer(importDoc, "imported", &importLayer) == CLAY_OK,
          "added a layer for the import")
    check(clay_layer_add_item(importDoc, importLayer, volumeItem, nil) == CLAY_OK,
          "placed the imported volume")

    let inside = evaluate(importDoc, [0, 0, 0])
    let outside = evaluate(importDoc, [2, 0, 0])
    check(inside[0] < 0, "the imported field is negative inside the box (\(inside[0]))")
    check(outside[0] > 0, "the imported field is positive outside it (\(outside[0]))")

    // clay_item_create still refuses a volume: it has no samples to give.
    var ignored: Float = 0
    check(clay_item_create(Int32(CLAY_PRIM_VOLUME.rawValue), &ignored, 1) == nil,
          "clay_item_create will not build a volume")

    // A mesh with no triangles has no surface to measure from.
    var emptyItem: OpaquePointer? = nil
    var emptyMesh: OpaquePointer? = nil
    check(clay_mesh_from_triangles(positions, positions.count / 3, indices, 0, &emptyMesh)
          == CLAY_ERROR_INVALID_ARGUMENT,
          "a mesh with no triangles is refused")
    check(clay_item_volume_from_mesh(nil, &volumeParams, &emptyItem)
          == CLAY_ERROR_INVALID_ARGUMENT,
          "a null mesh is refused")

    // And it survives a save, which is what makes an import worth doing.
    let importPath = NSTemporaryDirectory() + "clay_swift_import.clayspace"
    check(clay_document_save(importDoc, importPath) == CLAY_OK, "saved the imported document")
    var importBack: OpaquePointer? = nil
    check(clay_document_load(importPath, &importBack) == CLAY_OK, "loaded it back")
    if let importBack = importBack {
        check(evaluate(importBack, [0, 0, 0])[0] == inside[0],
              "the round trip preserved the imported field")
        clay_document_destroy(importBack)
    }
    try? FileManager.default.removeItem(atPath: importPath)

    // Relax: the reachable workflow from an app is import-then-smooth, since
    // this ABI builds a volume from a mesh.
    var relaxParams = clay_relax_params()
    relaxParams.struct_size = UInt32(MemoryLayout<clay_relax_params>.size)
    relaxParams.strength = 1.0
    relaxParams.radius_cells = 2
    relaxParams.iterations = 2
    check(clay_item_volume_relax(volumeItem, &relaxParams) == CLAY_OK,
          "relaxed the imported volume")

    // ...and asking to relax something that is not a volume is refused rather
    // than quietly doing nothing.
    var sphereRadius: Float = 1.0
    let plainItem = clay_item_create(Int32(CLAY_PRIM_SPHERE.rawValue), &sphereRadius, 1)
    check(clay_item_volume_relax(plainItem, &relaxParams) == CLAY_ERROR_INVALID_ARGUMENT,
          "relaxing a non-volume item is refused")
    clay_item_destroy(plainItem)

    clay_item_destroy(volumeItem)
    clay_document_destroy(importDoc)
    clay_mesh_destroy(boxMesh)
}

// -- result ------------------------------------------------------------------

print("\n\(checks - failures)/\(checks) checks passed")
if failures == 0 {
    print("swift smoke: OK")
    exit(0)
}
print("swift smoke: \(failures) failure(s)")
exit(1)

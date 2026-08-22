// Shared by both device test bundles: the gallery builds the same voxel
// and volume fixtures the verb cases do.

import Foundation
import XCTest
import claycore

enum Fixture {

    static func brush(size: Int32 = 8,
                      falloff: clay_brush_falloff = CLAY_BRUSH_FALLOFF_SMOOTH)
        -> clay_brush_params {
        var b = clay_brush_params()
        b.struct_size = UInt32(MemoryLayout<clay_brush_params>.size)
        b.size = size
        b.shape = Int32(CLAY_BRUSH_SHAPE_SPHERE.rawValue)
        b.falloff = Int32(falloff.rawValue)
        b.strength = 1.0
        b.seed = 1
        return b
    }

    /// Cell coordinates for stamp `i`, matching SceneBuilder's world spread so
    /// the voxel and SDF cases traverse the same shape.
    static func cell(_ i: Int, scale: Float = 40) -> [Int32] {
        let (x, y, z) = SceneBuilder.stampPosition(i)
        return [Int32(x * scale), Int32(y * scale), Int32(z * scale)]
    }

    /// A document with a voxel layer carrying `stamps` stamps of material, so
    /// the sculpt verbs have something to reshape. Returns the borrowed grid;
    /// destroying the document frees it.
    static func voxelGrid(stamps: Int, voxelSize: Float = 0.02)
        -> (doc: OpaquePointer, grid: OpaquePointer)? {
        guard let doc = clay_document_create() else { return nil }
        var layer: clay_layer_id = 0
        var grid: OpaquePointer?
        guard clay_document_add_voxel_layer(doc, "bench", voxelSize, &layer, &grid) == CLAY_OK,
              let grid else {
            clay_document_destroy(doc); return nil
        }
        var b = brush()
        for i in 0..<max(stamps, 1) {
            var c = cell(i)
            _ = clay_voxel_set_brush(grid, &c, &b, 1)
        }
        return (doc, grid)
    }

    /// Where perforation `i` goes, on SceneBuilder's own walk so the holes are
    /// spread rather than piled. Scaled to 18 rather than 40 so every hole
    /// lands INSIDE the block below with a margin: a hole on the surface is a
    /// dent, not a pocket, and this verb correctly ignores it.
    static func holeCell(_ i: Int) -> [Int32] {
        let (x, y, z) = SceneBuilder.stampPosition(i)
        return [Int32(x * 18), Int32(y * 18), Int32(z * 18)]
    }

    /// A solid block with `holes` single-cell perforations.
    ///
    /// The only shape `fill_cavities` acts on. The verb fills what is NARROW:
    /// a cell with five of its six face neighbours occupied, which is the
    /// pepper a dithered soft stamp leaves through material it just deposited.
    /// `voxelGrid`'s scattered spheres contain no such cell, so the case built
    /// on it measured NOTHING — 0 of 200 dabs changed a single cell, at every
    /// axis point and at brush strengths from 1.0 down to 0.3, confirmed
    /// through `clay_voxel_change_count` as the header prescribes.
    ///
    /// That was true before the stamp spread was corrected, not because of it.
    /// The old numbers were non-monotonic — 10:0.256, 100:0.482, 1000:0.019 ms
    /// — which is the collapsed spread piling blobs into accidental pockets at
    /// small counts and a solid mass at large. It measured the accident.
    ///
    /// `holes` is the axis: the block is a fixed 33^3 either way, so a bigger
    /// number is more pockets to find rather than more material to walk.
    static func perforatedBlock(holes: Int, voxelSize: Float = 0.02)
        -> (doc: OpaquePointer, grid: OpaquePointer)? {
        guard let doc = clay_document_create() else { return nil }
        var layer: clay_layer_id = 0
        var grid: OpaquePointer?
        guard clay_document_add_voxel_layer(doc, "bench", voxelSize, &layer, &grid) == CLAY_OK,
              let grid else {
            clay_document_destroy(doc); return nil
        }
        var lo: [Int32] = [-16, -16, -16]
        var hi: [Int32] = [16, 16, 16]
        guard clay_voxel_fill_box(grid, &lo, &hi, 1) == CLAY_OK else {
            clay_document_destroy(doc); return nil
        }
        punch(grid, holes: holes)
        return (doc, grid)
    }

    /// Re-open every perforation. Used as the fixture's RESET, because the verb
    /// under test fills them: without this the first pass over the block would
    /// close the pockets and every later iteration would measure an empty
    /// search. Untimed, and cheap — a set() per hole.
    static func punch(_ grid: OpaquePointer, holes: Int) {
        for i in 0..<max(holes, 1) {
            var c = holeCell(i)
            _ = clay_voxel_set(grid, &c, 0)
        }
    }

    /// A volume item baked from a document of `stamps` stamps — what relax and
    /// flatten act on. Free with clay_item_destroy.
    static func volumeItem(stamps: Int, cellSize: Float = 0.05) -> OpaquePointer? {
        guard let (doc, _) = SceneBuilder.sdfDocument(stamps: stamps) else { return nil }
        defer { clay_document_destroy(doc) }
        var params = clay_volume_params()
        params.struct_size = UInt32(MemoryLayout<clay_volume_params>.size)
        params.cell_size = cellSize
        params.band = 0
        params.padding = 0
        params.beta = 0
        var item: OpaquePointer?
        guard clay_item_volume_from_document(doc, &params, nil, nil, &item) == CLAY_OK else {
            return nil
        }
        return item
    }

    static func strokePreset(radius: Float = 0.15) -> clay_stroke_preset {
        var preset = clay_stroke_preset()
        preset.struct_size = UInt32(MemoryLayout<clay_stroke_preset>.size)
        _ = clay_stroke_preset_defaults(&preset)
        preset.radius = radius
        return preset
    }

    /// A short open path, as x y z r control points — what a Trim Curve drag
    /// leaves and what a tube is swept along. Deterministic, so two runs
    /// tessellate the same curve.
    static func curvePoints(count: Int = 12) -> [Float] {
        var points: [Float] = []
        points.reserveCapacity(count * 4)
        for i in 0..<count {
            let t = Float(i) / Float(max(count - 1, 1))
            points.append(contentsOf: [t * 1.6 - 0.8, sin(t * 4) * 0.3, 0, 0.1])
        }
        return points
    }

    /// The same path as x y z triples, which is what clay_tube_create takes.
    static func tubePath(count: Int = 12) -> [Float] {
        let xyzr = curvePoints(count: count)
        var path: [Float] = []
        path.reserveCapacity(count * 3)
        for i in 0..<count {
            path.append(contentsOf: [xyzr[i * 4], xyzr[i * 4 + 1], xyzr[i * 4 + 2]])
        }
        return path
    }

    /// A document of `stamps` stamps carrying a placed armature, plus the node
    /// it became and a target inside it to drag. The rig is the four-node tree
    /// tests/unit/test_c_armature.cpp uses: node 3 hangs off node 1, not off
    /// node 2, so a subtree move actually moves a subtree.
    static func armatureLayer(stamps: Int)
        -> (doc: OpaquePointer, layer: clay_layer_id, node: clay_node_id, target: UInt32)? {
        guard let (doc, layer) = SceneBuilder.sdfDocument(stamps: stamps) else { return nil }
        var rig: [Float] = [
            0.0, 0.0, 0.0, 0.30,   // 0, the root
            0.5, 0.0, 0.0, 0.20,   // 1, off the root
            1.0, 0.0, 0.0, 0.15,   // 2, off 1
            0.5, 0.6, 0.0, 0.15,   // 3, off 1 as well
        ]
        var parents: [UInt32] = [0, 0, 1, 1]
        guard let item = clay_item_create(Int32(CLAY_PRIM_ARMATURE.rawValue), nil, 0) else {
            clay_document_destroy(doc); return nil
        }
        defer { clay_item_destroy(item) }
        var node: clay_node_id = 0
        guard clay_item_set_stroke_points(item, &rig, 4) == CLAY_OK,
              clay_item_set_armature_parents(item, &parents, 4) == CLAY_OK,
              clay_layer_add_item(doc, layer, item, &node) == CLAY_OK else {
            clay_document_destroy(doc); return nil
        }
        // Node 1 — the one carrying a subtree, so the move is not a leaf move.
        return (doc, layer, node, 1)
    }

    /// Pointer-pressure-time samples along a short drag.
    static func strokeSamples(count: Int = 32) -> [Float] {
        var samples: [Float] = []
        samples.reserveCapacity(count * 5)
        for i in 0..<count {
            let t = Float(i) / Float(max(count - 1, 1))
            // x y z pressure time
            samples.append(contentsOf: [t * 1.2 - 0.6, sin(t * 3) * 0.2, 0, 1.0, t])
        }
        return samples
    }
}

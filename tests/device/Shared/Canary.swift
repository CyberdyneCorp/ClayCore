// A fixed workload whose only job is to say whether the machine underneath the
// suite changed while the suite ran.
//
// THE PROBLEM IT EXISTS FOR. A case's number depends on where in the run it was
// taken. Moving one bundle from third to first took sdf_stamp_cpu from
// 14.988 ms to 5.618 and sdf_stamp_bricks from 6.506 to 3.065 — same commit,
// same fixtures, same device, only the position. That is 2.7x against a
// regression tolerance of 1.4x, so ordering alone can report a regression twice
// the size of the one the gate exists to catch.
//
// And the OS signal cannot express it: both of those runs recorded
// thermalState `nominal` at every point. ProcessInfo.thermalState has four
// levels and the device throttles well inside the first one.

import Foundation

enum Canary {
    /// Deliberately NOT any verb, document, grid or tape.
    ///
    /// If the canary shared a path with something under test, an engine change
    /// would move it and the gate would report drift that is really a code
    /// change. `voxel_add_level` was tempting — it read 0.53 ms against 0.53 ms
    /// across two runs — and rejected for exactly that reason: it is budgeted.
    ///
    /// A dense walk over a fixed buffer with float arithmetic depends on the
    /// core it lands on and the clock it runs at, and on nothing this
    /// repository ships.
    private static let count = 1 << 16          // 64k floats, 256 KB — L2-sized
    private static let passes = 24

    /// Allocated once. A canary that also measured its own allocation would be
    /// reporting the allocator, which moves for reasons of its own.
    private static var buffer: [Float] = {
        (0..<count).map { Float($0 & 1023) * 0.001 }
    }()

    /// Cost of the fixed workload, in milliseconds.
    ///
    /// `blackHole` keeps the optimiser from deleting the arithmetic. Without
    /// it the canary measures nothing and reports beautifully stable zeros.
    static func sampleMs() -> Double {
        let start = DispatchTime.now().uptimeNanoseconds
        var acc: Float = 0
        buffer.withUnsafeBufferPointer { b in
            for _ in 0..<passes {
                var local: Float = 0
                for i in 0..<count { local = local * 1.0000001 + b[i] }
                acc += local
            }
        }
        blackHole(acc)
        return Double(DispatchTime.now().uptimeNanoseconds - start) / 1_000_000.0
    }

    @inline(never)
    private static func blackHole(_ value: Float) {
        if value == .infinity { print("unreachable \(value)") }
    }
}

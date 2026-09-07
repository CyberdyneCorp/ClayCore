// THE AXIS THE GATE DID NOT HAVE: how long the session has been running
// (gate-sustained-device-sculpting).
//
// `measureAxis` measures growth over DOCUMENT SIZE — `GrowthAxis.standard` is
// [10, 100, 1000] items, and a case fails when its cost scales faster than
// N^1.25. That catches an algorithm that is not local. It says nothing about an
// artist's actual session, which is hours of dabs on ONE model that is not
// growing.
//
// The failures on this axis are different ones: a leak, an unbounded cache, a
// history outgrowing its budget, an arena that never converges, a fragmenting
// allocator. None of them scale with the document, so none of them can fail a
// case on the other axis however large it is made.
//
// THREE WINDOWS OVER ONE UNINTERRUPTED RUN, with no reset between them — which
// is the opposite of what `measureAxis` does, and the reason this is its own
// harness rather than a flag on that one. A reset is exactly what would hide
// what this is looking for.
//
// WHAT IS RECORDED PER WINDOW: p50, p95 and p99, the memory footprint at the
// window's end, and the thermal state. On a device the last two are not
// optional context — a p95 that rose while the thermal state moved from nominal
// to serious is a thermal reading, not a leak, and a record without the state
// cannot tell the two apart. That is the same discipline the run-level thermal
// fields already carry.

import Foundation
import XCTest
#if canImport(Darwin)
import Darwin
#endif

/// One window of a sustained run.
struct SustainedWindow: Codable {
    /// 0 early, 1 middle, 2 late.
    let index: Int
    let dabs: Int
    let p50Ms: Double
    let p95Ms: Double
    let p99Ms: Double
    /// The process footprint in bytes at the END of this window. A leak shows
    /// as this rising window on window while the work does not.
    let footprintBytes: UInt64
    /// What the engine says it touched in this window. THE COUNTS ARE THE
    /// PORTABLE HALF: a duration is a claim about the machine and the moment,
    /// and a count of vertices reached is the same integer everywhere. A window
    /// whose p95 rose while its counts held is a thermal or an allocator
    /// reading; one whose counts rose is the engine doing more work.
    let verticesConsidered: UInt64
    let verticesAffected: UInt64
    let scratchHighWaterBytes: UInt64
    let thermalState: String
}

enum Footprint {
    /// The process's physical footprint, which is what jetsam scores. Zero when
    /// the platform does not offer it, and a zero is reported rather than
    /// guessed at: a fabricated figure here would read as a measurement.
    static func bytes() -> UInt64 {
        #if canImport(Darwin)
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size /
                                           MemoryLayout<natural_t>.size)
        let ok = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return ok == KERN_SUCCESS ? UInt64(info.phys_footprint) : 0
        #else
        return 0
        #endif
    }
}

extension VerbCaseGroup {
    /// Run `dab` `windows * perWindow` times with NO RESET, and report each
    /// window.
    ///
    /// `dab` takes the index so the caller can walk a path; `counters` is read
    /// after every window so the counts can be attributed to it. The caller
    /// owns the fixture and its lifetime, because a sustained case is exactly
    /// the case that must not rebuild anything between windows.
    func measureSustained(
        name: String, verb: String, _ cls: BudgetClass, backend: String = "cpu",
        perWindow: Int, windows: Int = 3,
        counters: () -> (considered: UInt64, affected: UInt64, scratch: UInt64),
        dab: (Int) -> Void
    ) {
        let canaryBefore = collector.sampleCanaryNow()
        let caseStartedAtMs = collector.elapsedMs
        let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)

        var rows: [SustainedWindow] = []
        var previous = counters()
        for w in 0..<windows {
            var samples: [Double] = []
            samples.reserveCapacity(perWindow)
            for i in 0..<perWindow {
                let t0 = DispatchTime.now().uptimeNanoseconds
                dab(w * perWindow + i)
                let t1 = DispatchTime.now().uptimeNanoseconds
                samples.append(Double(t1 - t0) / 1_000_000.0)
            }
            samples.sort()
            let now = counters()
            rows.append(SustainedWindow(
                index: w,
                dabs: perWindow,
                p50Ms: Timing.percentile(samples, 0.50),
                p95Ms: Timing.percentile(samples, 0.95),
                p99Ms: Timing.percentile(samples, 0.99),
                footprintBytes: Footprint.bytes(),
                verticesConsidered: now.considered - previous.considered,
                verticesAffected: now.affected - previous.affected,
                scratchHighWaterBytes: now.scratch,
                thermalState: DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)))
            previous = now
        }

        // The case is recorded through the SAME CaseResult every other case
        // uses, with the windows attached: one record, one reader, and a
        // sustained case that a baseline diff can see rather than a second
        // format nothing compares.
        //
        // `measurements` carries the late window so a case that has never been
        // measured on this axis still gets a latency row a budget can be
        // stated against; `growthExponent` is nil because there is no document
        // axis here and reporting one would be reporting a number about
        // something this case did not vary.
        let last = rows.last
        collector.add(CaseResult(
            name: name, verb: verb, budgetClass: cls,
            backend: backend, servedBy: backend,
            measurements: last.map {
                [Measurement(stamps: $0.dabs, p50Ms: $0.p50Ms, p95Ms: $0.p95Ms,
                             samples: $0.dabs, repeats: 1, p95SpreadMs: 0, batch: 1)]
            } ?? [],
            growthExponent: nil,
            startedAtMs: caseStartedAtMs,
            thermalStateStart: caseThermalStart,
            thermalStateEnd: DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState),
            canaryBeforeMs: canaryBefore,
            canaryAfterMs: collector.sampleCanaryNow(),
            windows: rows))
    }
}

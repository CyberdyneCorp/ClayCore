// The scaffolding a verb-case bundle needs, so there can be more than one.
//
// There is more than one because `testEveryVerbOnDevice` sat right on this
// iPad's jetsam limit. Split off into its own PROCESS it still died at the
// heavy tail — mask_extrude, sdf_consolidate, sdf_relax, sdf_move and the four
// authoring verbs — and split off into its own SESSION it still died there,
// after 25 and after 40 minutes of idle, having passed twice at 30. A run that
// depends on how long the tablet rested is not a gate; it is a coin flip that
// costs an hour to toss.
//
// So the peak is what got smaller, not the schedule. The light cases and the
// heavy ones are two bundles, which is two processes, and a process is the
// only boundary that gives the high-water mark back. The split is at
// mask_extrude because that is where the kills landed: 21 cases in on one run,
// 23 on another.
//
// Cases are matched to baselines BY NAME — check_device_bench.py keys on
// `name` and nothing else — so moving a case between bundles changes which
// process measures it and not what it is compared against. The `bundle` field
// each record carries is used only to group canary samples, which is exactly
// right: a new bundle times from its own start.

import Foundation
import XCTest
import claycore

/// Shared by the verb bundles. Holds no test methods of its own, so XCTest
/// does not collect it.
class VerbCaseGroup: XCTestCase {


    var collector = RunCollector()

    func abiVersion() -> String {
        var major: Int32 = 0, minor: Int32 = 0, patch: Int32 = 0
        clay_version(&major, &minor, &patch)
        return "\(major).\(minor).\(patch)"
    }

    /// Measure one verb across the growth axis.
    ///
    /// `prepare` builds the fixture for a document size and returns the timed
    /// body plus its teardown. Returning nil skips that size, which is a
    /// failure rather than a silent gap.
    /// `axis` overrides the shared growth axis for a case whose smallest point
    /// its fixture cannot feed. Only `voxel_mesh_dirty` uses it; the reason is
    /// recorded there.
    /// How many distinct positions a walking case visits before it wraps.
    ///
    /// The walking cases take no reset — a dab lands wherever the walk has
    /// reached — and un-batched they made ~203 calls per axis point, which
    /// sparsely sampled the working volume. Batched at 128 they would make
    /// ~26,000 and SATURATE it: `stampPosition` is bounded to +/-0.8, so at a
    /// 0.02 cell that is an 80^3 mask and a 64^3 grid fully written rather than
    /// dotted. That is a fixture several times larger than the one the case was
    /// calibrated on, and it took the measure bundle over the jetsam limit —
    /// `testEveryVerbOnDevice` died with signal kill at `mask_extrude`, the
    /// heaviest case in the suite, on two runs out of three.
    ///
    /// Wrapping keeps the fixture the size it was and is the more faithful
    /// gesture besides: a stroke passes over ground it has already touched.
    static let walkWindow = 256

    /// `batch` applications of the verb per timed body, recorded alongside the
    /// figure so it stays a statement about the verb.
    ///
    /// A case can only fail the gate when its growth clears BOTH the 1.4x
    /// tolerance and the 0.05 ms floor, so a figure under 0.125 ms cannot be
    /// objected to at any ratio (#337). The verbs below are genuinely that
    /// cheap: one `clay_cut_create` is 0.2 us and would need a THREE-HUNDRED-
    /// fold regression before this suite noticed. Batching puts the figure
    /// where the tolerance means something without changing what is timed —
    /// the same call, on the same fixture, N times.
    ///
    /// Growing the per-application workload instead was the other option, and
    /// is right where a knob exists (`voxel_smooth_r32` is exactly that). It
    /// does not exist for most of these: `cut_create` resolves a fixed rect,
    /// and reaching 300x through its only size input would mean a 3,000-vertex
    /// lasso, which is not the gesture the verb names.
    func measureAxis(
        name: String, verb: String, _ cls: BudgetClass, backend: String = "cpu",
        axis: [Int] = GrowthAxis.standard, batch: Int = 1,
        prepare: (Int) -> (body: () -> Void, reset: (() -> Void)?,
                           cleanup: () -> Void)?
    ) {
        var measurements: [Measurement] = []
        // Bracket the case rather than sampling on a timer: what the gate
        // needs is the machine THIS case ran on. See CaseResult.canaryBeforeMs.
        let canaryBefore = collector.sampleCanaryNow()
        let caseStartedAtMs = collector.elapsedMs
        let caseThermalStart = DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState)
        for stamps in axis {
            guard let fixture = prepare(stamps) else {
                XCTFail("\(name): could not build a fixture at \(stamps) stamps")
                continue
            }
            let one = fixture.body
            let body: () -> Void = batch <= 1 ? one : { for _ in 0..<batch { one() } }
            let r = Timing.measureStable(reset: fixture.reset, body)
            fixture.cleanup()
            measurements.append(Measurement(stamps: stamps, p50Ms: r.p50,
                                            p95Ms: r.p95, samples: r.n,
                                            repeats: r.repeats,
                                            p95SpreadMs: r.spread,
                                            batch: batch))
        }
        let canaryAfter = collector.sampleCanaryNow()
        collector.add(CaseResult(
            name: name, verb: verb, budgetClass: cls,
            backend: backend, servedBy: backend,
            measurements: measurements,
            growthExponent: Timing.growthExponent(measurements),
            startedAtMs: caseStartedAtMs,
            thermalStateStart: caseThermalStart,
            thermalStateEnd: DeviceInfo.thermalName(ProcessInfo.processInfo.thermalState),
            canaryBeforeMs: canaryBefore,
            canaryAfterMs: canaryAfter))
    }

    /// Attach the record and make the two assertions every verb bundle makes.
    func finishAndCheck() {
        // -- the coverage guard ------------------------------------------------------

        let record = collector.finish(abiVersion: abiVersion(), attachTo: self)

        // A case that ran but names no verb in the table is checkable here.
        // The converse — every non-exempt entry produced a measurement — is
        // NOT: `sdf_stamp_cpu` runs in LatencyTests, and each test emits its
        // own attachment, so no single test sees the whole run. That check
        // belongs to tools/check_device_coverage.py, which reads the merged
        // record and clay.h together.
        let known = Set(Coverage.table.compactMap(\.caseName))
        for result in record.cases {
            XCTAssertTrue(known.contains(result.name),
                          "coverage: case '\(result.name)' is in no table entry")
        }
        XCTAssertTrue(record.valid,
                      "thermal state \(record.thermalStateStart) -> "
                      + "\(record.thermalStateEnd); a throttled run is a different "
                      + "experiment, not a slower result")
    }
}

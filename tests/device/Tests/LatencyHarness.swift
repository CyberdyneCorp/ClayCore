// The measurement core for the on-device harness.
//
// Three decisions are load-bearing here and are worth stating where they are
// implemented rather than only in the spec:
//
// 1. PERCENTILES, NOT MEANS. Occasional samples land on an efficiency core.
//    A mean lets one stall hide inside a passing average; a p95 does not, and
//    a stroke is hundreds of stamps so the tail is what an artist feels.
//
// 2. WARM-UP IS EXCLUDED. Compiling the Metal pipeline the first time cost
//    14.172 s against 0.010 s warm on the first device run of this harness.
//    Timed without a warm-up, the first case measures the shader cache.
//
// 3. LATENCY IS PER STAMP, ACROSS A DOCUMENT-GROWTH AXIS. The tape recompiles
//    on every edit, so per-stamp cost is a function of accumulated document
//    size. A single-size measurement pins the least interesting point on that
//    curve.

import Foundation
import XCTest

// MARK: - What a case declares

/// What kind of interaction a case represents. A single frame budget would be
/// wrong for most of the surface: `consolidate` is not per-stamp work and,
/// held to a stamp budget, would fail forever until the gate was ignored.
enum BudgetClass: String, Codable {
    /// One brush stamp on a stroke. Must fit inside a fraction of a frame,
    /// because the host still has to draw with what is left.
    case interactive
    /// One drag resolved as a unit — a Move drag, a cut preview.
    case gesture
    /// An explicit user action: consolidate, mask extrude, an export.
    case operation
}

// MARK: - What a run records

struct Measurement: Codable {
    /// Where on the growth axis this was measured.
    let stamps: Int
    let p50Ms: Double
    let p95Ms: Double
    let samples: Int
}

struct CaseResult: Codable {
    let name: String
    /// Key into the coverage table — the brush or verb this exercises.
    let verb: String
    let budgetClass: BudgetClass
    /// The backend the case asked for.
    let backend: String
    /// The backend that actually served it. A case whose request was not
    /// honoured is a failure, not a result: recording the substitute's number
    /// would let a Metal regression hide behind a CPU measurement.
    let servedBy: String
    let measurements: [Measurement]
    /// d(log cost)/d(log stamps) across the axis. 0 is flat, 1 is linear in
    /// document size, 2 is quadratic. Reported even when it passes, so the
    /// curve is inspectable without re-running.
    let growthExponent: Double?
}

struct RunRecord: Codable {
    /// "iPad15,5" — the specific model, not UIDevice's generic "iPad".
    /// Baselines are only comparable against the hardware that produced them.
    let deviceModel: String
    let osVersion: String
    let abiVersion: String
    let thermalStateStart: String
    let thermalStateEnd: String
    /// False when thermal state was anything but nominal at either end. A
    /// throttled run is not a slower result, it is a different experiment.
    let valid: Bool
    let cases: [CaseResult]
    /// The coverage table as the harness declared it, so the checker can hold
    /// it against the entry points clay.h actually exposes.
    let coverage: [CoverageEntry]
}

// MARK: - Timing

enum Timing {
    /// Nearest-rank percentile over already-sorted samples.
    static func percentile(_ sorted: [Double], _ p: Double) -> Double {
        guard !sorted.isEmpty else { return .nan }
        let rank = Int((p / 100.0 * Double(sorted.count)).rounded(.up)) - 1
        return sorted[min(max(rank, 0), sorted.count - 1)]
    }

    /// Timed work to aim for per measurement. Sample count is derived from
    /// this rather than fixed, for two reasons that pull the same way:
    ///
    /// A FIXED 30 SAMPLES MAKES p95 MEANINGLESS. The 95th percentile of 30
    /// values is the second-worst one, so a single scheduling hiccup IS the
    /// result — measured: the brick-cache case reported 5.7 ms and 16.0 ms on
    /// consecutive runs with identical code. A gate on that would fail for
    /// reasons unrelated to the change under test.
    ///
    /// And it wastes minutes on the slow cases: `sdf_consolidate` at ~5 s an
    /// iteration spent 150 s per document size to produce a number that a
    /// tenth of the samples pins just as well.
    static let targetMeasureMs = 1500.0
    static let minSamples = 10
    static let maxSamples = 200

    /// Run `body` untimed to warm up, then timed enough times for the
    /// percentiles to mean something. Returns milliseconds.
    ///
    /// `reset` runs after every iteration and is NOT timed. Any verb that
    /// mutates what it measures needs one, or iteration 2 measures something
    /// iteration 1 already changed. Two ways that bit, both caught by raising
    /// the sample count:
    ///
    ///  - a stamp case with no reset grows the document by one per iteration,
    ///    so "10 stamps" silently measured the average over 10..210 of them;
    ///  - `consolidate` collapses a layer, so every iteration after the first
    ///    re-consolidated an already-consolidated layer — a different and much
    ///    cheaper operation (measured: 4975 ms became 514 ms purely by
    ///    changing the sample count, which is the tell).
    static func measure(warmups: Int = 3, reset: (() -> Void)? = nil,
                        _ body: () -> Void)
        -> (p50: Double, p95: Double, n: Int) {
        for _ in 0..<warmups { body(); reset?() }

        // Probe once to size the run. The warm-ups already paid any one-time
        // cost, so this is representative.
        //
        // Size by body PLUS reset, even though only the body is reported. A
        // reset can cost far more than the verb it restores — `relax` runs in
        // under a millisecond but its reset rebuilds a thousand-stamp document
        // and re-bakes a volume — and sizing on the body alone asked for 200
        // samples of that, which took the device out of memory and killed the
        // test process mid-run.
        let probeStart = DispatchTime.now().uptimeNanoseconds
        body()
        let bodyEnd = DispatchTime.now().uptimeNanoseconds
        reset?()
        let resetEnd = DispatchTime.now().uptimeNanoseconds

        _ = bodyEnd  // only the full iteration decides how many we can afford
        let iterationMs = Double(resetEnd - probeStart) / 1_000_000.0
        let wanted = iterationMs > 0 ? Int(targetMeasureMs / iterationMs) : maxSamples
        let samples = min(max(wanted, minSamples), maxSamples)

        var times: [Double] = []
        times.reserveCapacity(samples)
        for _ in 0..<samples {
            let t0 = DispatchTime.now().uptimeNanoseconds
            body()
            let t1 = DispatchTime.now().uptimeNanoseconds
            times.append(Double(t1 - t0) / 1_000_000.0)
            reset?()
        }
        times.sort()
        return (percentile(times, 50), percentile(times, 95), samples)
    }

    /// Fitted exponent of cost against document size: log-log slope between
    /// the smallest and largest point on the axis.
    static func growthExponent(_ measurements: [Measurement]) -> Double? {
        guard let first = measurements.first, let last = measurements.last,
              measurements.count >= 2,
              first.stamps > 0, last.stamps > first.stamps,
              first.p95Ms > 0, last.p95Ms > 0 else { return nil }
        let sizeRatio = log(Double(last.stamps) / Double(first.stamps))
        guard sizeRatio > 0 else { return nil }
        return log(last.p95Ms / first.p95Ms) / sizeRatio
    }
}

// MARK: - Environment

enum DeviceInfo {
    /// The hardware identifier ("iPad15,5"). `UIDevice.model` says only
    /// "iPad", which is useless for deciding whether two runs are comparable.
    static var model: String {
        var info = utsname()
        uname(&info)
        // Read the tuple through a Mirror rather than rebinding a pointer into
        // it: taking an unsafe pointer to `info.machine` while `info` is still
        // being written is an exclusivity violation the compiler rejects.
        let name = Mirror(reflecting: info.machine).children
            .compactMap { $0.value as? CChar }
            .prefix { $0 != 0 }
            .map { String(UnicodeScalar(UInt8(bitPattern: $0))) }
            .joined()
        return name.isEmpty ? "unknown" : name
    }

    static func thermalName(_ state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal:  return "nominal"
        case .fair:     return "fair"
        case .serious:  return "serious"
        case .critical: return "critical"
        @unknown default: return "unknown"
        }
    }
}

// MARK: - Collecting a run

/// Accumulates case results and attaches the run as JSON.
final class RunCollector {
    private var cases: [CaseResult] = []
    private let thermalStart: ProcessInfo.ThermalState

    init() {
        thermalStart = ProcessInfo.processInfo.thermalState
    }

    func add(_ result: CaseResult) {
        cases.append(result)
        let axis = result.measurements
            .map { "\($0.stamps): p50 \(String(format: "%.3f", $0.p50Ms)) ms / "
                 + "p95 \(String(format: "%.3f", $0.p95Ms)) ms" }
            .joined(separator: ", ")
        let growth = result.growthExponent
            .map { String(format: "%.2f", $0) } ?? "n/a"
        print("bench \(result.name) [\(result.servedBy)] "
              + "\(result.budgetClass.rawValue): \(axis); growth^\(growth)")
    }

    /// Builds the record and attaches it to the running test.
    func finish(abiVersion: String, attachTo testCase: XCTestCase) -> RunRecord {
        let thermalEnd = ProcessInfo.processInfo.thermalState
        let record = RunRecord(
            deviceModel: DeviceInfo.model,
            osVersion: ProcessInfo.processInfo.operatingSystemVersionString,
            abiVersion: abiVersion,
            thermalStateStart: DeviceInfo.thermalName(thermalStart),
            thermalStateEnd: DeviceInfo.thermalName(thermalEnd),
            valid: thermalStart == .nominal && thermalEnd == .nominal,
            cases: cases,
            coverage: Coverage.table)

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let data = try? encoder.encode(record) {
            let attachment = XCTAttachment(data: data, uniformTypeIdentifier: "public.json")
            attachment.name = "device-bench.json"
            attachment.lifetime = .keepAlways
            testCase.add(attachment)
            // Also on stdout: an attachment needs xcresulttool to read, and a
            // developer watching a local run should not need it.
            print("=== device-bench.json ===")
            print(String(data: data, encoding: .utf8) ?? "<unencodable>")
            print("=== end device-bench.json ===")
        }
        return record
    }
}

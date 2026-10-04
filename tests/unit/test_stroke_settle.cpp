// The settle rule of brush::StrokeTransaction (#670): which stamps of a stroke
// still under the pen are FINAL, so a consumer may apply them as they settle
// and still apply exactly the stamps of the whole path.
//
// The property, checked for every preset field and several batchings: at every
// append, each stamp the transaction calls settled equals — bit for bit, in
// everything a consumer reads — the stamp at that index of the finished
// stroke. And the rule is not vacuous: without a start taper, stamps settle
// while the stroke is still moving.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "clay/brush/stroke.h"

using namespace clay;

namespace {

// A wandering path with every channel moving: uneven sample spacing, pressure,
// tilt, azimuth and velocity, so every field the resolver interpolates has
// something to interpolate.
std::vector<brush::StrokeSample> wandering_path(int n) {
    std::vector<brush::StrokeSample> out;
    float x = -1.0f;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i);
        x += 0.02f + 0.015f * static_cast<float>(i % 3);
        brush::StrokeSample s;
        s.position = kernel::cf3(x, 0.15f * std::sin(t * 0.3f), 0.05f * std::cos(t * 0.17f));
        s.pressure = 0.35f + 0.6f * static_cast<float>((i * 7) % 11) / 10.0f;
        s.tilt = 0.1f * static_cast<float>(i % 5);
        s.azimuth = 0.2f * t;
        s.velocity = 0.5f + 0.25f * static_cast<float>(i % 4);
        s.timestamp = 0.004 * t;
        out.push_back(s);
    }
    return out;
}

struct NamedPreset {
    std::string name;
    brush::StrokePreset preset;
};

std::vector<NamedPreset> presets() {
    brush::StrokePreset base;
    base.radius = 0.08f;
    base.spacing = 0.25f;
    std::vector<NamedPreset> out;
    out.push_back({"plain", base});
    brush::StrokePreset p = base;
    p.taper_end = 0.3f;
    out.push_back({"taper_end", p});
    p = base;
    p.taper_start = 0.2f;
    p.taper_end = 0.2f;
    out.push_back({"taper_start", p});
    p = base;
    p.steady = 0.6f;
    out.push_back({"steady", p});
    p = base;
    p.jitter_position = 0.3f;
    p.jitter_size = 0.4f;
    p.jitter_rotation = 0.8f;
    p.seed = 11;
    out.push_back({"jitter", p});
    p = base;
    p.pressure.size = 0.8f;
    p.pressure.strength = 0.7f;
    p.pressure.curve = 1.7f;
    out.push_back({"pressure", p});
    p = base;
    p.velocity_response.size = -0.5f;
    p.velocity_response.strength = 0.4f;
    p.velocity_response.reference = 1.0f;
    out.push_back({"velocity", p});
    p = base;
    p.rotate_along_stroke = true;
    out.push_back({"rotate_along", p});
    p = base;
    p.rotate_to_azimuth = true;
    out.push_back({"azimuth", p});
    p = base;
    p.accumulation = brush::Accumulation::Clamped;
    p.taper_end = 0.15f;
    out.push_back({"clamped", p});
    return out;
}

// Batch sizes: one batch, singles, fives, forties, and an uneven schedule.
std::vector<std::vector<std::size_t>> schedules(std::size_t n) {
    std::vector<std::vector<std::size_t>> out;
    out.push_back({n});
    for (std::size_t size : {std::size_t{1}, std::size_t{5}, std::size_t{40}}) {
        std::vector<std::size_t> s;
        for (std::size_t done = 0; done < n; done += size) s.push_back(std::min(size, n - done));
        out.push_back(s);
    }
    std::vector<std::size_t> uneven;
    std::uint32_t h = 2166136261u;
    for (std::size_t done = 0; done < n;) {
        h = (h ^ static_cast<std::uint32_t>(done)) * 16777619u;
        const std::size_t size = std::min<std::size_t>(1 + h % 13, n - done);
        uneven.push_back(size);
        done += size;
    }
    out.push_back(uneven);
    return out;
}

bool same_for_a_consumer(const brush::Stamp& a, const brush::Stamp& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.radius == b.radius && a.strength == b.strength &&
           a.deposit == b.deposit && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w;
}

}  // namespace

TEST_CASE("stroke settle: a settled stamp is the finished stroke's stamp, for every preset") {
    const std::vector<brush::StrokeSample> path = wandering_path(80);
    for (const NamedPreset& np : presets()) {
        const std::vector<brush::Stamp> whole = brush::resolve_stroke(path, np.preset);
        REQUIRE(whole.size() > 10);
        for (const std::vector<std::size_t>& schedule : schedules(path.size())) {
            CAPTURE(np.name);
            CAPTURE(schedule.size());
            brush::StrokeTransaction tx(np.preset);
            std::size_t fed = 0, settled_before = 0, mid_settled = 0;
            bool settled_grew = true, revisions_respect_settled = true, settled_matches = true;
            for (std::size_t size : schedule) {
                tx.append({path.begin() + static_cast<std::ptrdiff_t>(fed),
                           path.begin() + static_cast<std::ptrdiff_t>(fed + size)});
                fed += size;
                const std::size_t settled = tx.settled();
                settled_grew = settled_grew && settled >= settled_before;
                revisions_respect_settled =
                    revisions_respect_settled && tx.revised_from() >= settled_before;
                for (std::size_t i = 0; i < settled; ++i)
                    settled_matches = settled_matches && i < whole.size() &&
                                      same_for_a_consumer(tx.stamps()[i], whole[i]);
                settled_before = settled;
                if (fed <= path.size() / 2) mid_settled = settled;
            }
            CHECK(settled_grew);
            CHECK(revisions_respect_settled);
            CHECK(settled_matches);
            CHECK_FALSE(tx.finished());

            tx.finish();
            CHECK(tx.settled() == whole.size());
            REQUIRE(tx.stamps().size() == whole.size());
            bool exact = true;
            for (std::size_t i = 0; i < whole.size(); ++i)
                exact = exact && same_for_a_consumer(tx.stamps()[i], whole[i]) &&
                        tx.stamps()[i].along == whole[i].along;
            CHECK(exact);

            // NOT VACUOUS. A stroke with no start taper settles while it is
            // still moving, so "nothing settles until the end" would fail here
            // and a consumer applies ink under the pen. A start taper is the
            // one field that holds everything: it is a fraction of the whole
            // stroke, so every station eventually falls inside it.
            if (schedule.size() > 1) {
                if (np.preset.taper_start > 0.0f)
                    CHECK(mid_settled == 0);
                else
                    CHECK(mid_settled >= whole.size() / 4);
            }
        }
    }
}

TEST_CASE("stroke settle: the end taper is held back and settles as the stroke moves on") {
    brush::StrokePreset p;
    p.radius = 0.1f;
    p.spacing = 0.25f;
    p.taper_end = 0.3f;
    std::vector<brush::StrokeSample> path;
    for (int i = 0; i <= 100; ++i) {
        brush::StrokeSample s;
        s.position = kernel::cf3(0.02f * static_cast<float>(i), 0.0f, 0.0f);
        path.push_back(s);
    }
    brush::StrokeTransaction tx(p);
    tx.append({path.begin(), path.begin() + 51});
    const std::size_t half_stamps = tx.stamps().size();
    const std::size_t half_settled = tx.settled();
    // The last 30% of what has arrived is still tapering, so it waits.
    CHECK(half_settled < half_stamps);
    CHECK(half_settled >= half_stamps * 6 / 10);
    // The radius at the tail is the TAPERED one now, which is exactly what a
    // consumer must not apply: the stroke goes on past it.
    CHECK(tx.stamps().back().radius < p.radius);

    tx.append({path.begin() + 51, path.end()});
    // Stamps that were tapering at the half are full-width now.
    CHECK(tx.revised_from() < half_stamps);
    CHECK(tx.revised_from() >= half_settled);
    CHECK(tx.stamps()[half_stamps - 1].radius == p.radius);
    CHECK(tx.settled() > half_settled);

    // A finished transaction takes no more samples.
    tx.finish();
    const std::size_t final_count = tx.stamps().size();
    CHECK(tx.append(path).empty());
    CHECK(tx.stamps().size() == final_count);
    CHECK(tx.samples().size() == path.size());
}

TEST_CASE("stroke settle: a lone sample has no settled stamp until a second arrives") {
    brush::StrokePreset p;
    p.radius = 0.1f;
    p.rotate_to_azimuth = true;
    brush::StrokeSample a;
    a.position = kernel::cf3(0, 0, 0);
    a.azimuth = 3.0f;
    brush::StrokeTransaction tx(p);
    tx.append({a});
    REQUIRE(tx.stamps().size() == 1);
    CHECK(tx.settled() == 0);
    brush::StrokeSample b = a;
    b.position = kernel::cf3(0.01f, 0, 0);
    tx.append({b});
    CHECK(tx.settled() == 1);
}

TEST_CASE("stroke settle: a cursor hands each settled stamp out once, in order") {
    brush::StrokePreset p;
    p.radius = 0.05f;
    p.spacing = 0.3f;
    p.taper_end = 0.2f;
    const std::vector<brush::StrokeSample> path = wandering_path(60);
    const std::vector<brush::Stamp> whole = brush::resolve_stroke(path, p);

    brush::StrokeTransaction tx(p);
    brush::StampCursor cursor;
    std::vector<brush::Stamp> taken;
    for (std::size_t i = 0; i < path.size(); i += 7) {
        tx.append({path.begin() + static_cast<std::ptrdiff_t>(i),
                   path.begin() + static_cast<std::ptrdiff_t>(std::min(i + 7, path.size()))});
        CHECK(cursor.taken() == taken.size());
        const std::vector<brush::Stamp> next = cursor.take(tx);
        taken.insert(taken.end(), next.begin(), next.end());
        CHECK_FALSE(cursor.drained(tx));
    }
    CHECK(taken.size() < whole.size());  // the taper tail is still held back
    tx.finish();
    const std::vector<brush::Stamp> rest = cursor.take(tx);
    taken.insert(taken.end(), rest.begin(), rest.end());
    CHECK(cursor.drained(tx));
    CHECK(cursor.take(tx).empty());
    REQUIRE(taken.size() == whole.size());
    bool same = true;
    for (std::size_t i = 0; i < whole.size(); ++i)
        same = same && same_for_a_consumer(taken[i], whole[i]);
    CHECK(same);
}

TEST_CASE("stroke settle: the station the count's epsilon admits past the end waits") {
    // length / step sits a hair under 2, and the count's epsilon admits a
    // third station at d = 2 * step — just PAST the path received. Clamped to
    // the end of the path for now, it moves as soon as the path goes on, so it
    // is not settled; the first two are.
    brush::StrokePreset p;
    p.radius = 0.1f;
    p.spacing = 0.25f;  // step 0.05
    brush::StrokeSample a, b, c;
    a.position = kernel::cf3(0.0f, 0.0f, 0.0f);
    b.position = kernel::cf3(0.09999f, 0.0f, 0.0f);
    c.position = kernel::cf3(0.09999f, 0.05f, 0.0f);  // the path turns
    brush::StrokeTransaction tx(p);
    tx.append({a, b});
    REQUIRE(tx.stamps().size() == 3);
    CHECK(tx.settled() == 2);
    const brush::Stamp provisional = tx.stamps()[2];
    tx.append({c});
    REQUIRE(tx.stamps().size() >= 3);
    // It did move: the rule is not caution, it is the difference.
    CHECK(tx.stamps()[2].position.y != provisional.position.y);
    CHECK(tx.settled() >= 3);
}

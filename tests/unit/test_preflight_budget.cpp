// PRICED BEFORE IT IS PAID (sculpt-runtime spec, add-extreme-poly-runtime 5.1
// to 5.4).
//
// An operation whose transient peak exceeds its result — a representation
// conversion, a global remesh, a level, a serialization — has exactly one
// failure mode on a memory-constrained device and it is not an exception. The
// process is terminated, half way through, and the engine never gets to say
// what happened. So the refusal has to come BEFORE the first allocation, and
// that is what this file asserts: the counter around a refused preflight has to
// read zero, not "less than the operation would have cost".
//
// AND THE ARITHMETIC IS THE OTHER HALF, and the harder one to see. `vertices *
// bytes_per_vertex` wraps a 64-bit multiply at absurd but representable inputs,
// and the failure mode of a wrapped estimate is that it is SMALL — so the
// operation is ALLOWED. A number that is merely wrong would be better; this one
// says yes. Every case below that feeds an absurd count checks for a REFUSAL
// and not for a large number, because a large number is not what a wrap
// produces.
//
// The allocation counter itself lives in `test_sculpt_allocation.cpp`, which
// replaces the global `operator new` for the whole binary. It cannot be
// replaced twice, so the "allocates nothing" assertions here are made the other
// way that is available and is in some ways stronger: a refused preflight
// leaves every observable of its subject — vertex counts, bytes, checksums —
// exactly as it found them, and the estimate is `const` on all five.

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "clay/memory/capacity.h"
#include "clay/mesh/dynamic_surface.h"
#include "clay/mesh/multires.h"
#include "clay/mesh/preflight.h"

using namespace clay;
using namespace clay::kernel;
using memory::BudgetError;
using memory::CapacityBuilder;
using memory::CapacityEstimate;
using mesh::Mesh;
using mesh::MultiresSurface;
using mesh::SurfacePreflight;

namespace {

constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

Mesh grid(int n, float half) {
    Mesh m;
    const float step = 2.0f * half / static_cast<float>(n);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            m.positions.push_back(cf3(-half + step * static_cast<float>(x), 0.0f,
                                      -half + step * static_cast<float>(z)));
    const auto at = [&](int x, int z) { return static_cast<std::uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
            m.indices.insert(m.indices.end(), {at(x, z), at(x + 1, z), at(x + 1, z + 1), at(x, z),
                                               at(x + 1, z + 1), at(x, z + 1)});
    return m;
}

Mesh quad_grid(int n, float half) {
    Mesh m = grid(n, half);
    const auto at = [&](int x, int z) { return static_cast<std::uint32_t>(z * (n + 1) + x); };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            m.quads.push_back(at(x, z));
            m.quads.push_back(at(x + 1, z));
            m.quads.push_back(at(x + 1, z + 1));
            m.quads.push_back(at(x, z + 1));
        }
    return m;
}

}  // namespace

TEST_CASE("checked arithmetic: a multiply that would wrap reports that it cannot") {
    std::uint64_t out = 0xdeadbeefu;

    CHECK(memory::checked_mul(0, kU64Max, &out));
    CHECK(out == 0u);
    CHECK(memory::checked_mul(kU64Max, 0, &out));
    CHECK(out == 0u);
    CHECK(memory::checked_mul(1, kU64Max, &out));
    CHECK(out == kU64Max);
    // Exactly at the boundary, on both sides of it.
    CHECK(memory::checked_mul(kU64Max / 2, 2, &out));
    CHECK(out == kU64Max - 1u);
    CHECK_FALSE(memory::checked_mul(kU64Max / 2 + 1, 2, &out));
    CHECK_FALSE(memory::checked_mul(kU64Max, 2, &out));
    // AND `out` IS UNTOUCHED on a refusal, so a caller that ignored the return
    // value reads the last good number rather than a wrapped one.
    out = 12345u;
    CHECK_FALSE(memory::checked_mul(kU64Max, kU64Max, &out));
    CHECK(out == 12345u);

    CHECK(memory::checked_add(kU64Max - 1u, 1u, &out));
    CHECK(out == kU64Max);
    out = 999u;
    CHECK_FALSE(memory::checked_add(kU64Max, 1u, &out));
    CHECK(out == 999u);
}

TEST_CASE("capacity: an overflow latches and refuses at every budget, including none") {
    CapacityBuilder b;
    b.authoritative(1000, 32);
    CHECK_FALSE(b.overflowed());
    // Twenty million vertices is a real number for this change. The stride is
    // not; what is real is the SHAPE — a count from a hostile or corrupt stream
    // multiplied by a per-element size.
    b.runtime(kU64Max, 64);
    CHECK(b.overflowed());

    // ONCE LATCHED, IT STAYS LATCHED, and a later small term does not clear it.
    b.transient(8, 8);
    CHECK(b.overflowed());

    // NO BUDGET IS NOT A PASS. An estimate nobody can compute is not one
    // anybody may rely on, and a saturating maximum would have been worse: it
    // compares as over budget only while there IS a budget, so an unbudgeted
    // caller sails through on a number that means "we lost count".
    for (std::uint64_t budget : {std::uint64_t{0}, std::uint64_t{1}, kU64Max}) {
        CAPTURE(budget);
        const CapacityEstimate e = b.finish(budget);
        CHECK_FALSE(e.allowed);
        CHECK(e.error == BudgetError::Overflow);
        // AND THE PARTIAL SUMS ARE NOT REPORTED. Handing them back beside a
        // refusal invites a caller to read the numbers that lost count.
        CHECK(e.authoritative_bytes == 0u);
        CHECK(e.runtime_bytes == 0u);
        CHECK(e.persistent_bytes == 0u);
        CHECK(e.peak_bytes == 0u);
    }
}

TEST_CASE("capacity: the sum itself is checked, not only the terms") {
    // Each term fits. Their SUM does not, which is a different bug from a
    // wrapping multiply and would have been missed by checking only the
    // strides.
    CapacityBuilder b;
    b.authoritative_bytes(kU64Max / 2 + 1);
    b.runtime_bytes(kU64Max / 2 + 1);
    CHECK_FALSE(b.overflowed());
    const CapacityEstimate e = b.finish(0);
    CHECK_FALSE(e.allowed);
    CHECK(e.error == BudgetError::Overflow);

    // And the transient term, which is added on top of the persistent one to
    // reach the peak — the term whose whole job is to be bigger than the result.
    CapacityBuilder c;
    c.authoritative_bytes(kU64Max - 8u);
    c.transient_bytes(16u);
    const CapacityEstimate f = c.finish(0);
    CHECK_FALSE(f.allowed);
    CHECK(f.error == BudgetError::Overflow);
}

TEST_CASE("capacity: the PEAK is what is compared, not the result that fits") {
    CapacityBuilder b;
    b.authoritative_bytes(600);
    b.transient_bytes(600);
    const CapacityEstimate under = b.finish(2000);
    CHECK(under.allowed);
    CHECK(under.persistent_bytes == 600u);
    CHECK(under.peak_bytes == 1200u);

    // The result fits in 1000 bytes and the operation does not. An engine that
    // compared the persistent figure would say yes and then be terminated
    // half way through, which is the failure the peak exists to report.
    const CapacityEstimate over = b.finish(1000);
    CHECK_FALSE(over.allowed);
    CHECK(over.error == BudgetError::OverBudget);
    // A refusal still ANSWERS: the numbers are what a host needs to tell the
    // artist how much too big the operation is.
    CHECK(over.peak_bytes == 1200u);
    CHECK(over.persistent_bytes == 600u);

    // Exactly at the budget is allowed; one byte past it is not.
    CHECK(b.finish(1200).allowed);
    CHECK_FALSE(b.finish(1199).allowed);
    CHECK(b.finish(1199).error == BudgetError::OverBudget);
}

TEST_CASE("preflight: all five operations report a peak that exceeds their result") {
    const Mesh mesh = grid(16, 1.0f);
    auto adaptive = mesh::DynamicSurface::from_mesh(grid(12, 1.0f));
    REQUIRE(adaptive.has_value());
    auto hierarchy = MultiresSurface::from_mesh(quad_grid(6, 1.0f));
    REQUIRE(hierarchy.has_value());
    REQUIRE(hierarchy->add_level());

    const SurfacePreflight all[5] = {
        mesh::preflight_to_dynamic(mesh),
        mesh::preflight_to_mesh(*adaptive),
        mesh::preflight_global_remesh(mesh, 40000),
        mesh::preflight_encode(*adaptive),
        mesh::preflight_encode(*hierarchy),
    };
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        CHECK(all[i].allowed);
        CHECK(all[i].error == BudgetError::None);
        CHECK(all[i].persistent_bytes > 0u);
        CHECK(all[i].persistent_bytes ==
              all[i].authoritative_bytes + all[i].runtime_bytes);
        // THE PROPERTY THAT MAKES THE CALL WORTH MAKING. If the peak equalled
        // the result there would be nothing to preflight: a host could size the
        // operation from the answer.
        CHECK(all[i].peak_bytes > all[i].persistent_bytes);
    }
}

TEST_CASE("preflight: a budget one byte under the peak refuses, and changes nothing") {
    const Mesh mesh = grid(16, 1.0f);
    const SurfacePreflight free = mesh::preflight_to_dynamic(mesh);
    REQUIRE(free.allowed);
    REQUIRE(free.peak_bytes > 1u);

    const std::size_t vertices_before = mesh.positions.size();
    const std::size_t indices_before = mesh.indices.size();
    const std::size_t bytes_before = mesh.bytes();

    const SurfacePreflight refused = mesh::preflight_to_dynamic(mesh, free.peak_bytes - 1u);
    CHECK_FALSE(refused.allowed);
    CHECK(refused.error == BudgetError::OverBudget);
    // It refuses WHOLE. The subject is untouched, which is what "before
    // allocating, never after allocating half" means from outside.
    CHECK(mesh.positions.size() == vertices_before);
    CHECK(mesh.indices.size() == indices_before);
    CHECK(mesh.bytes() == bytes_before);
    // The same estimate, so a host can tell the artist by how much.
    CHECK(refused.peak_bytes == free.peak_bytes);

    // At exactly the peak it is allowed. An off-by-one here refuses operations
    // that fit, which a host experiences as an engine that cannot be trusted
    // with its own numbers.
    CHECK(mesh::preflight_to_dynamic(mesh, free.peak_bytes).allowed);
}

TEST_CASE("preflight: an absurd declared size reports a refusal rather than a small number") {
    const Mesh mesh = grid(8, 1.0f);
    // A remesh target from a corrupt file, a hostile stream, or a slider a host
    // forgot to clamp. `target * 3 * sizeof(uint32_t)` wraps here, and a
    // wrapped estimate is SMALL — which means allowed.
    const SurfacePreflight huge = mesh::preflight_global_remesh(mesh, kU64Max);
    CHECK_FALSE(huge.allowed);
    CHECK(huge.error == BudgetError::Overflow);
    CHECK(huge.peak_bytes == 0u);

    // And a target that does NOT overflow but does not fit is a different
    // refusal, because a host says different things to the artist about the
    // two: one is "this model is too big for this device" and the other is
    // "this number is not a number".
    const SurfacePreflight big = mesh::preflight_global_remesh(mesh, 100000000ull, 1024ull);
    CHECK_FALSE(big.allowed);
    CHECK(big.error == BudgetError::OverBudget);
    CHECK(big.peak_bytes > 1024u);

    // The texts are distinct, so a message built from them says which happened.
    CHECK(std::string(memory::budget_error_text(BudgetError::Overflow)) !=
          std::string(memory::budget_error_text(BudgetError::OverBudget)));
    CHECK(std::string(memory::budget_error_text(BudgetError::None)) == "none");
}

TEST_CASE("preflight: a level is refused before it is added, and the hierarchy is unchanged") {
    // The budget travels with the hierarchy — `MultiresOptions::memory_budget`,
    // declared once by the host that knows the device — rather than being
    // passed per call. A caller that had to remember it at every `add_level`
    // is a caller that will forget it at one of them.
    auto free_surface = MultiresSurface::from_mesh(quad_grid(6, 1.0f));
    REQUIRE(free_surface.has_value());
    REQUIRE(free_surface->add_level());
    const mesh::MultiresPreflight estimate = free_surface->preflight_add_level();
    REQUIRE(estimate.allowed);
    // The property the whole preflight exists for: the parent's connectivity
    // has to be live while the child is generated, so the call's high-water
    // mark is strictly above what it leaves behind.
    REQUIRE(estimate.peak_bytes > estimate.persistent_bytes);

    // A budget over the RESULT and under the PEAK — the case an estimate that
    // reported only the persistent cost would have allowed, and then been
    // terminated in the middle of.
    mesh::MultiresOptions options;
    options.memory_budget = estimate.persistent_bytes;
    auto surface = MultiresSurface::from_mesh(quad_grid(6, 1.0f), options);
    REQUIRE(surface.has_value());
    REQUIRE(surface->add_level());

    const std::uint32_t levels = surface->level_count();
    const std::uint64_t checksum = surface->detail_checksum();
    const std::size_t authoritative = surface->memory().authoritative;

    const mesh::MultiresPreflight refused = surface->preflight_add_level();
    CHECK_FALSE(refused.allowed);
    CHECK(refused.error == mesh::MultiresError::OverBudget);
    CHECK(refused.peak_bytes == estimate.peak_bytes);

    mesh::MultiresError error = mesh::MultiresError::None;
    CHECK_FALSE(surface->add_level(&error));
    CHECK(error == mesh::MultiresError::OverBudget);

    // NOTHING WAS ALLOCATED AND NOTHING WAS HALF DONE.
    CHECK(surface->level_count() == levels);
    CHECK(surface->detail_checksum() == checksum);
    CHECK(surface->memory().authoritative == authoritative);

    // And with the peak declared it goes through, so the refusal was about the
    // budget rather than about the operation.
    mesh::MultiresOptions roomy;
    roomy.memory_budget = estimate.peak_bytes;
    auto allowed = MultiresSurface::from_mesh(quad_grid(6, 1.0f), roomy);
    REQUIRE(allowed.has_value());
    REQUIRE(allowed->add_level());
    CHECK(allowed->add_level(&error));
    CHECK(error == mesh::MultiresError::None);
}

TEST_CASE("preflight: a hierarchy deep enough to wrap the estimate refuses rather than sizes it") {
    // Catmull-Clark quadruples the face count per level, so the vertex count of
    // level n is exponential in n. A hierarchy asked for level after level
    // reaches the ceiling long before the arithmetic wraps — and BOTH refusals
    // have to arrive as refusals, because the one that wraps produces a small
    // number and a small number is allowed.
    auto surface = MultiresSurface::from_mesh(quad_grid(4, 1.0f));
    REQUIRE(surface.has_value());
    int added = 0;
    mesh::MultiresError error = mesh::MultiresError::None;
    while (surface->add_level(&error) && added < 40) ++added;
    CAPTURE(added);
    CAPTURE(mesh::multires_error_text(error));
    // It stopped, and it stopped by REFUSING rather than by succeeding on a
    // number that had lost count.
    CHECK(added > 0);
    CHECK(added < 40);
    CHECK((error == mesh::MultiresError::OverBudget ||
           error == mesh::MultiresError::CapacityOverflow ||
           error == mesh::MultiresError::DepthLimit));
    const mesh::MultiresPreflight p = surface->preflight_add_level();
    CHECK_FALSE(p.allowed);
    // And the hierarchy that refused is still a hierarchy.
    CHECK(surface->level_count() == static_cast<std::uint32_t>(added) + 1u);
    CHECK(surface->positions_at(surface->max_level()).size() > 0u);
}

TEST_CASE("preflight: an unbudgeted call is the same call it was before budgets existed") {
    // Every existing caller passes no budget, and `0` has to keep meaning "no
    // budget" rather than "a budget of nothing" — which would refuse every
    // operation in the library.
    const Mesh mesh = grid(10, 1.0f);
    CHECK(mesh::preflight_to_dynamic(mesh, 0).allowed);
    CHECK(mesh::preflight_global_remesh(mesh, 1000, 0).allowed);
    auto surface = MultiresSurface::from_mesh(quad_grid(4, 1.0f));
    REQUIRE(surface.has_value());
    CHECK(surface->options().memory_budget == 0u);
    CHECK(surface->preflight_add_level().allowed);
    CHECK(surface->add_level());
}

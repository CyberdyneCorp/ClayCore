// The measurement behind `add-cpu-simd-path`'s retarget (issue #207), kept so
// its design.md table can be re-derived rather than trusted.
//
// NOT a gated benchmark. It compares four SHAPES of the same tape evaluation to
// establish where the per-instruction cost lives, before the blocked evaluator
// this change proposes exists to be gated:
//
//   V0  ctape_eval, the shipping interpreter
//   V1  per point, colour carried      — V0 minus the checks for features the
//                                        instruction does not use
//   V2  per point, distance only       — colour's contribution, unblocked
//   V3  blocked,   colour carried      — blocking's contribution
//   V4  blocked,   distance only       — both
//
// V1..V4 restructure the LOOP only: the primitive and combine maths are the
// shipping kernel functions, so what is measured is bookkeeping. Each variant
// is asserted bit-identical to V0 — if that fails, the variant is not the same
// maths and its timing means nothing. That guard is the point of the file.
//
// The tape it builds is spheres under hard unions: no deformers, repeats, gates
// or sampled volumes. So V1's margin over V0 is precisely the price of asking
// questions whose answer is always no, and a corpus that USES those features
// pays for them legitimately. Re-measure there before quoting V1 (task 1.10).
//
//   ./tape_block_prototype [items] [points] [block]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clay/scene/commands.h"
#include "clay/scene/tape.h"

using namespace clay;
using namespace clay::kernel;
using kernel::cf3;

namespace {

std::uint64_t next(std::uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s;
}

float unit(std::uint64_t& s) {
    return static_cast<float>((next(s) >> 40) & 0xFFFFFF) / 16777216.0f * 2.0f - 1.0f;
}

std::vector<float> make_points(std::size_t count) {
    std::vector<float> p(count * 3);
    std::uint64_t s = 99991;
    for (float& v : p) v = unit(s);
    return p;
}

scene::Document make_document(std::size_t items) {
    scene::Document doc;
    scene::Layer& l = doc.add_sdf_layer("prototype");
    std::uint64_t s = 4242;
    for (std::size_t i = 0; i < items; ++i) {
        scene::Node n;
        n.prim = scene::Prim::sphere(0.12f);
        n.xform.position = cf3(unit(s) * 0.8f, unit(s) * 0.8f, unit(s) * 0.8f);
        n.op = scene::Op::Add;
        n.blend = scene::Blend{scene::BlendProfile::Hard, 0.0f};
        l.sdf->insert(n);
    }
    return doc;
}

// One prim instruction's parameter header. The whole argument of the blocked
// shape is that this is loaded once per BLOCK rather than once per point.
struct PrimHeader {
    cfloat4x4 inv;
    float scale;
    float round;
    cfloat3 color;
};

PrimHeader load_header(const float* pr) {
    PrimHeader h;
    h.inv.c0 = cf4(pr[0], pr[1], pr[2], 0.0f);
    h.inv.c1 = cf4(pr[3], pr[4], pr[5], 0.0f);
    h.inv.c2 = cf4(pr[6], pr[7], pr[8], 0.0f);
    h.inv.c3 = cf4(pr[9], pr[10], pr[11], 1.0f);
    h.scale = pr[12];
    h.round = pr[13];
    h.color = cf3(pr[14], pr[15], pr[16]);
    return h;
}

template <class F>
double best_ms(F&& f) {
    double best = 1e30;
    for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t items = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 2500;
    const std::size_t count = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 20480;
    const std::size_t block = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 512;

    const std::vector<float> pts = make_points(count);
    const scene::Document doc = make_document(items);
    const scene::Tape tape = scene::compile_document(doc);
    const CTapeInstr* in = tape.instrs.data();
    const std::size_t ni = tape.instrs.size();
    const float* pa = tape.params.data();
    const float* bl = tape.blob.data();

    auto point_at = [&](std::size_t i) {
        return cf3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
    };

    std::vector<float> r0(count), r1(count), r2(count), r3(count), r4(count);

    const double t0 = best_ms([&] {
        for (std::size_t i = 0; i < count; ++i) r0[i] = tape.eval(point_at(i)).d;
    });

    const double t1 = best_ms([&] {
        for (std::size_t i = 0; i < count; ++i) {
            const cfloat3 p = point_at(i);
            CTapeValue st[CLAY_TAPE_MAX_STACK];
            std::size_t top = 0;
            for (std::size_t k = 0; k < ni; ++k) {
                const float* pr = pa + in[k].param_offset;
                if (in[k].op == ctape_combine) {
                    const CTapeValue b = st[top - 1];
                    const CTapeValue a = st[top - 2];
                    --top;
                    st[top - 1] = ctape_combine_values(a, b, CLAY_INT(pr[0]), CLAY_INT(pr[1]),
                                                       pr[2], pr[3]);
                } else {
                    const PrimHeader h = load_header(pr);
                    CTapeValue v;
                    v.color = h.color;
                    v.d = ctape_prim_dist(in[k].op, pr + CLAY_TAPE_PRIM_HEADER, bl,
                                          cmul_point(h.inv, p)) * h.scale - h.round;
                    st[top++] = v;
                }
            }
            r1[i] = st[top - 1].d;
        }
    });

    const double t2 = best_ms([&] {
        for (std::size_t i = 0; i < count; ++i) {
            const cfloat3 p = point_at(i);
            float st[CLAY_TAPE_MAX_STACK];
            std::size_t top = 0;
            for (std::size_t k = 0; k < ni; ++k) {
                const float* pr = pa + in[k].param_offset;
                if (in[k].op == ctape_combine) {
                    const float b = st[top - 1];
                    const float a = st[top - 2];
                    --top;
                    st[top - 1] = ctape_smin_m(CLAY_INT(pr[1]), a, b, pr[2]).x;
                } else {
                    const PrimHeader h = load_header(pr);
                    st[top++] = ctape_prim_dist(in[k].op, pr + CLAY_TAPE_PRIM_HEADER, bl,
                                                cmul_point(h.inv, p)) * h.scale - h.round;
                }
            }
            r2[i] = st[top - 1];
        }
    });

    std::vector<CTapeValue> stack3(block * CLAY_TAPE_MAX_STACK);
    const double t3 = best_ms([&] {
        for (std::size_t base = 0; base < count; base += block) {
            const std::size_t n = std::min(block, count - base);
            std::size_t top = 0;
            for (std::size_t k = 0; k < ni; ++k) {
                const float* pr = pa + in[k].param_offset;
                if (in[k].op == ctape_combine) {
                    const int mode = CLAY_INT(pr[0]);
                    const int profile = CLAY_INT(pr[1]);
                    CTapeValue* b = &stack3[(top - 1) * block];
                    CTapeValue* a = &stack3[(top - 2) * block];
                    for (std::size_t j = 0; j < n; ++j)
                        a[j] = ctape_combine_values(a[j], b[j], mode, profile, pr[2], pr[3]);
                    --top;
                } else {
                    const PrimHeader h = load_header(pr);  // once per block
                    CTapeValue* s = &stack3[top * block];
                    for (std::size_t j = 0; j < n; ++j) {
                        s[j].color = h.color;
                        s[j].d = ctape_prim_dist(in[k].op, pr + CLAY_TAPE_PRIM_HEADER, bl,
                                                 cmul_point(h.inv, point_at(base + j))) *
                                     h.scale - h.round;
                    }
                    ++top;
                }
            }
            for (std::size_t j = 0; j < n; ++j) r3[base + j] = stack3[(top - 1) * block + j].d;
        }
    });

    std::vector<float> stack4(block * CLAY_TAPE_MAX_STACK);
    const double t4 = best_ms([&] {
        for (std::size_t base = 0; base < count; base += block) {
            const std::size_t n = std::min(block, count - base);
            std::size_t top = 0;
            for (std::size_t k = 0; k < ni; ++k) {
                const float* pr = pa + in[k].param_offset;
                if (in[k].op == ctape_combine) {
                    const int profile = CLAY_INT(pr[1]);
                    float* b = &stack4[(top - 1) * block];
                    float* a = &stack4[(top - 2) * block];
                    for (std::size_t j = 0; j < n; ++j)
                        a[j] = ctape_smin_m(profile, a[j], b[j], pr[2]).x;
                    --top;
                } else {
                    const PrimHeader h = load_header(pr);  // once per block
                    float* s = &stack4[top * block];
                    for (std::size_t j = 0; j < n; ++j)
                        s[j] = ctape_prim_dist(in[k].op, pr + CLAY_TAPE_PRIM_HEADER, bl,
                                               cmul_point(h.inv, point_at(base + j))) *
                                   h.scale - h.round;
                    ++top;
                }
            }
            for (std::size_t j = 0; j < n; ++j) r4[base + j] = stack4[(top - 1) * block + j];
        }
    });

    const auto deviation = [&](const std::vector<float>& v) {
        double w = 0.0;
        for (std::size_t i = 0; i < count; ++i)
            w = std::max(w, static_cast<double>(std::fabs(v[i] - r0[i])));
        return w;
    };
    const double worst = std::max(std::max(deviation(r1), deviation(r2)),
                                  std::max(deviation(r3), deviation(r4)));

    std::printf("items=%zu instrs=%zu points=%zu block=%zu\n", items, ni, count, block);
    std::printf("max deviation from ctape_eval: %.3g%s\n\n", worst,
                worst == 0.0 ? "  (bit-identical)" : "  <-- NOT the same maths");

    const struct { const char* name; double ms; } rows[] = {
        {"V0 ctape_eval          ", t0}, {"V1 per point, colour   ", t1},
        {"V2 per point, dist only", t2}, {"V3 blocked,   colour   ", t3},
        {"V4 blocked,   dist only", t4}};
    for (const auto& r : rows)
        std::printf("  %s %9.2f ms  %6.2f ns/instr  %6.2fx\n", r.name, r.ms,
                    r.ms * 1e6 / (static_cast<double>(count) * static_cast<double>(ni)),
                    t0 / r.ms);

    // The guard is the file's reason to exist: a variant that is not bit-identical
    // is not the same maths, and its timing is not a comparison.
    return worst == 0.0 ? 0 : 1;
}

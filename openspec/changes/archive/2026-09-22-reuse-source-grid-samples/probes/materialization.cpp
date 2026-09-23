#include "clay/field/volume.h"
#include "clay/session/sdf_prefix_cache.h"
#include "clay/eval/backend.h"
#include <cmath>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstdlib>
using namespace clay;
using field::FieldVolume;
using kernel::cf3;
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    std::ofstream output(argv[1], std::ios::binary);
    auto write = [&](const auto &value) {
        output.write(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    scene::Document doc;
    auto &layer = doc.add_sdf_layer("body");
    scene::Node base;
    base.prim = scene::Prim::sphere(1.0f);
    layer.sdf->insert(base);
    const int dabs = std::atoi(argv[2]);
    for (int i = 1; i < dabs; ++i) {
        const float t = float(i) * 2.399963f;
        const float z = 1.0f - 2.0f * (float(i) + .5f) / float(dabs);
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        scene::Node d;
        d.prim = scene::Prim::sphere(.09f);
        d.xform.position = cf3(r * std::cos(t), r * std::sin(t), z);
        d.blend = {scene::BlendProfile::Quadratic, .04f};
        layer.sdf->insert(d);
    }
    std::puts("case,repeat,ms");
    for (int which = 0; which < 6; ++which) {
        for (int repeat = 0; repeat < 7; ++repeat) {
            const math::Aabb bounds{cf3(-1, -1, -1), cf3(1, 1, 1)};
            auto volume = FieldVolume::empty_lattice(bounds, .02f, .06f);
            auto source = session::SdfSourceField::open(doc, doc.layers.front().id, nullptr);
            if (!source || !eval::Registry::instance().find("cpu"))
                return 4;
            const auto fill = source->block_fill();
            std::vector<FieldVolume::BrickCoord> added;
            auto region = which % 2 == 0 ? FieldVolume::Region{bounds}
                                         : FieldVolume::Region::ball(cf3(0, 0, 0), .35f);
            if (which >= 2)
                volume.materialize_region(FieldVolume::Region::ball(cf3(-.6f, 0, 0), .2f), fill,
                                          &added);
            const auto start = std::chrono::steady_clock::now();
            auto tally = volume.materialize_region(region, fill, &added);
            if (which >= 4)
                tally = volume.materialize_region(region, fill, &added);
            const auto stop = std::chrono::steady_clock::now();
            std::printf("%d,%d,%.6f\n", which, repeat,
                        std::chrono::duration<double, std::milli>(stop - start).count());
            write(tally.evaluated);
            write(tally.kept);
            write(tally.added);
            const auto lip = volume.sample_lipschitz();
            write(lip);
            const auto blob = volume.to_blob();
            write(blob.size());
            output.write(reinterpret_cast<const char *>(blob.data()),
                         static_cast<std::streamsize>(blob.size() * sizeof(blob[0])));
            write(added.size());
            for (const auto &v : added) {
                write(v.x);
                write(v.y);
                write(v.z);
            }
        }
    }
    return output ? 0 : 3;
}

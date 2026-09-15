#pragma once

#include <array>
#include <cassert>
#include <cstddef>

namespace clay::mesh::detail {

// Closed lattice of one ordinary brick. The caller restricts dimensions to
// 1..16; boundary cells outside this brick keep using the original sampler.
class BrickSamples {
  public:
    static constexpr int max_dimension = 16;
    static constexpr std::size_t max_samples = 17 * 17 * 17;

    template <class Sample>
    BrickSamples(int dimension, const int low[3], const Sample& sample)
        : low_{low[0], low[1], low[2]}, side_(dimension + 1) {
        assert(dimension > 0 && dimension <= max_dimension);
        for (int z = 0; z < side_; ++z)
            for (int y = 0; y < side_; ++y)
                for (int x = 0; x < side_; ++x)
                    values_[(z * side_ + y) * side_ + x] =
                        sample(low_[0] + x, low_[1] + y, low_[2] + z);
    }

    float operator()(int x, int y, int z) const {
        return values_[((z - low_[2]) * side_ + (y - low_[1])) * side_ + (x - low_[0])];
    }

  private:
    std::array<int, 3> low_;
    int side_;
    // Only the active closed lattice is initialized or read.
    std::array<float, max_samples> values_;
};

}  // namespace clay::mesh::detail

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace clay::mesh::detail {

// A recording range owns this cache and its sampler outlives it. Unlike the
// full-brick cache, sample only lattice points that a boundary cell asks for.
template <class Sample>
class BoundarySamples {
  public:
    static constexpr int max_dimension = 16;
    static constexpr std::size_t max_samples = 17 * 17 * 17;

    BoundarySamples(int dimension, const Sample& sample)
        : dimension_(dimension), side_(dimension + 1), sample_(sample) {
        assert(dimension > 0 && dimension <= max_dimension);
    }

    void select_cell(int x, int y, int z) {
        const std::array<int, 3> owner{floor_div(x), floor_div(y), floor_div(z)};
        if (ready_ && owner == owner_) return;
        owner_ = owner;
        for (int axis = 0; axis < 3; ++axis) low_[axis] = owner[axis] * dimension_;
        std::fill_n(valid_.begin(), (side_ * side_ * side_ + 63) / 64, std::uint64_t{0});
        ready_ = true;
    }

    float operator()(int x, int y, int z) const {
        assert(ready_);
        const int ix = x - low_[0], iy = y - low_[1], iz = z - low_[2];
        assert(ix >= 0 && ix < side_ && iy >= 0 && iy < side_ && iz >= 0 && iz < side_);
        const std::size_t at = (iz * side_ + iy) * side_ + ix;
        const std::uint64_t bit = std::uint64_t{1} << (at % 64);
        if (!(valid_[at / 64] & bit)) {
            values_[at] = sample_(x, y, z);
            valid_[at / 64] |= bit;
        }
        return values_[at];
    }

  private:
    int floor_div(int value) const {
        return value >= 0 ? value / dimension_ : (value + 1) / dimension_ - 1;
    }

    int dimension_, side_;
    const Sample& sample_;
    std::array<int, 3> owner_{}, low_{};
    bool ready_ = false;
    mutable std::array<std::uint64_t, (max_samples + 63) / 64> valid_;
    mutable std::array<float, max_samples> values_;
};

}  // namespace clay::mesh::detail

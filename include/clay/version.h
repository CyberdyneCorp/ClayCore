#pragma once

namespace clay {

struct Version {
    int major;
    int minor;
    int patch;
};

Version version() noexcept;

}  // namespace clay

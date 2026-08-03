#include "clay/version.h"

namespace clay {

Version version() noexcept {
    return Version{CLAY_VERSION_MAJOR, CLAY_VERSION_MINOR, CLAY_VERSION_PATCH};
}

}  // namespace clay

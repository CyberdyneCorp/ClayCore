#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "clay/version.h"

TEST_CASE("version reports the project version") {
    const clay::Version v = clay::version();
    CHECK(v.major == 0);
    CHECK(v.minor >= 1);
    CHECK(v.patch >= 0);
}

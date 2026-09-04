// Phase 0's only test. It asserts almost nothing about the project and
// everything about the build: that CMake configures, that Catch2 was fetched
// and linked, that cs_core compiles and links, that the test binary runs, and
// that CTest can see individual test cases.
//
// A trivial test that runs in CI is worth more right now than a real test that
// does not, because every later test inherits this machinery.

#include <catch2/catch_test_macros.hpp>

#include "core/version.hpp"

TEST_CASE("core is linkable and reports its version", "[smoke]") {
    STATIC_REQUIRE(cs::version() == "0.2.0");
    REQUIRE_FALSE(cs::build_flavour().empty());
}

TEST_CASE("the test binary knows which build it is", "[smoke]") {
    // Guards against the mistake of benchmarking a debug build, or of a test
    // passing only because NDEBUG compiled an assertion away.
    const auto flavour = cs::build_flavour();
    REQUIRE((flavour == "debug" || flavour == "release"));
}

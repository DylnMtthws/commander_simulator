# Tests, and the third-party dependency policy.
#
# DEPENDENCIES: FetchContent, not a package manager. CMake downloads and builds
# the dependency as part of the configure step, so there is nothing to install,
# no system state to drift, and the exact version is pinned in this file where
# it is visible in a diff. vcpkg or Conan would be the right answer at twenty
# dependencies; at two they are a second toolchain to learn and maintain.
#
# FIND_PACKAGE_ARGS makes CMake prefer an installed Catch2 if one exists and
# fall back to downloading. That keeps CI fast when the runner has it cached.
#
# TEST FRAMEWORK: Catch2 v3 over GoogleTest. Catch2 uses plain expressions -
# REQUIRE(a == b) - and reports both operand values on failure by decomposing
# the expression, so there is no ASSERT_EQ/ASSERT_NE/ASSERT_LT family to learn.
# For someone new to C++ that is a materially smaller surface. GoogleTest wins
# on mocking, which this project does not need: the core takes values and
# returns values. doctest compiles faster but has a smaller community, and at
# this project's size compile time will not be the bottleneck.

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1          # pinned, never a branch
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS NAMES Catch2)
FetchContent_MakeAvailable(Catch2)

add_executable(cs_tests
    unit/test_smoke.cpp)

# Catch2WithMain supplies main(), so no test file writes one.
target_link_libraries(cs_tests PRIVATE cs_core cs_warnings Catch2::Catch2WithMain)

# catch_discover_tests asks the built binary for its test list and registers
# each one with CTest individually, so `ctest` reports per-test results and can
# run them in parallel - rather than treating the whole binary as one test.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(cs_tests)

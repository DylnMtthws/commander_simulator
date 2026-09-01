#pragma once

#include <string_view>

// Placeholder content for Phase 0. There is deliberately no domain logic here
// yet - the point of this phase is a green build, so the first Magic-shaped
// code lands in a project that already compiles, tests, and runs in CI.
namespace cs {

// consteval, so a caller cannot accidentally pay for this at runtime and the
// value is guaranteed to be a compile-time constant.
[[nodiscard]] consteval std::string_view version() noexcept { return "0.1.0"; }

// Not consteval: something has to live in version.cpp, or cs_core is a library
// with no translation unit and CMake has nothing to compile.
[[nodiscard]] std::string_view build_flavour() noexcept;

}  // namespace cs

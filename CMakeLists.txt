# The top-level build file.
#
# CMake is a build *generator*, not a build system: it reads these files and
# writes files for a backend, which here is Ninja. That indirection is the
# reason CMake is worth learning despite its awkward language - every C++
# library assumes it, and nanobind/pybind11 (the Python bindings in a later
# phase) are CMake-native. Choosing anything else means fighting the ecosystem.
#
# Read this file top to bottom; it is the whole project in miniature.

cmake_minimum_required(VERSION 3.25)

project(commander_simulator
    VERSION 0.1.0
    DESCRIPTION "Monte Carlo goldfishing simulator for one cEDH Commander deck"
    LANGUAGES CXX)

# --- Language level -------------------------------------------------------
#
# C++20, set globally so no target can quietly disagree. We want std::span
# (a non-owning view over a contiguous range - the correct parameter type for
# "here is an array I do not own"), std::popcount from <bit> for the zone
# bitsets, and designated initialisers. C++23's library support in libc++ is
# still patchy, so it is not worth the trouble yet.
#
# CXX_EXTENSIONS OFF means -std=c++20 rather than -std=gnu++20: no compiler
# extensions, so the code stays portable and a GNU-ism cannot creep in
# unnoticed.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd / editors understand the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings are configured once, here, as an INTERFACE library rather than as
# global flags. An INTERFACE library carries no code - it is a named bundle of
# settings that targets opt into by linking it. That keeps our warning policy
# off any third-party code we fetch, which would otherwise drown the build in
# warnings we cannot fix.
add_library(cs_warnings INTERFACE)
target_compile_options(cs_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow            # a local shadowing a member is a classic silent bug
    -Wconversion        # implicit narrowing, which bitset/index code invites
    -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Wdouble-promotion)

option(CS_WERROR "Treat warnings as errors (on in CI)" OFF)
if(CS_WERROR)
    target_compile_options(cs_warnings INTERFACE -Werror)
endif()

include(CTest)   # defines BUILD_TESTING (default ON) and the `test` target

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

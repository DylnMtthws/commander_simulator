# The simulation core.
#
# THE RULE THIS FILE EXISTS TO ENFORCE: cs_core links nothing but the C++
# standard library. No I/O, no JSON, no TOML, no logging, no CLI.
#
# That single constraint is what makes the later Python bindings additive
# rather than a rewrite: a binding module calls into cs_core and never touches
# the loading or printing code. It is also what keeps the core testable without
# fixtures on disk, and callable concurrently (SIM_PLAN.md INVARIANT S1).
#
# Note the absence below: there is no target_link_libraries(cs_core PUBLIC ...)
# naming any project library. Adding one is the violation, and because io/ will
# be a separate target, a core file that reaches for it fails to LINK rather
# than merely failing review. scripts/check_core_is_sealed.sh catches the
# subtler cases that still compile.

add_library(cs_core
    version.cpp
    mana.cpp
    state.cpp
    pattern.cpp
    policy.cpp
    sim.cpp)

# PUBLIC on the include directory: anything linking cs_core inherits the path,
# so the CLI and the tests can #include <core/version.hpp> without repeating it.
# The include root is src/, which is why headers are included with the `core/`
# prefix - it makes every include say which layer it comes from.
target_include_directories(cs_core PUBLIC ${PROJECT_SOURCE_DIR}/src)

# PRIVATE: our warning policy applies when COMPILING cs_core, but is not
# inherited by things that merely link it. PUBLIC would push our warnings onto
# consumers, which is rude in a library and noisy in practice.
target_link_libraries(cs_core PRIVATE cs_warnings)

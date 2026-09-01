// Phase 0 entry point: proves the executable links against the core and runs.
// Argument parsing, the metric banner and the honesty header (SIM_PLAN.md
// section 9.5) arrive in Phase 6.

#include <cstdio>

#include "core/version.hpp"

int main() {
    // std::printf rather than <iostream>: this is a placeholder and iostream
    // drags in a large static initialiser for no benefit. The real CLI will use
    // std::format. Either is fine in cli/; neither is allowed in core/.
    //
    // The "%.*s" is not incidental. A std::string_view is NOT guaranteed to be
    // null-terminated - it is a pointer and a length, and it may point into the
    // middle of a larger buffer. Passing .data() to a "%s" therefore reads
    // until it happens to find a zero byte, which is undefined behaviour that
    // usually looks fine because the view came from a string literal. Print a
    // view by passing its length explicitly, every time.
    const auto v = cs::version();
    const auto flavour = cs::build_flavour();
    std::printf("commander_simulator %.*s (%.*s build)\n",
                static_cast<int>(v.size()), v.data(),
                static_cast<int>(flavour.size()), flavour.data());
    return 0;
}

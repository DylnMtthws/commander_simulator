#include "core/version.hpp"

namespace cs {

// Reports which build configuration produced this binary. Useful the first
// time a benchmark looks impossibly fast because it was a debug build, or a
// test passes only because assertions were compiled out.
std::string_view build_flavour() noexcept {
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

}  // namespace cs

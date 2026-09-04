#pragma once

#include <string>
#include <string_view>

namespace cs::io {

// Lowercase SHA-256 hex. I/O owns integrity/canonicalisation; the simulation
// core never hashes, parses JSON, or knows a candidate exists.
[[nodiscard]] std::string sha256_hex(std::string_view input);

}  // namespace cs::io

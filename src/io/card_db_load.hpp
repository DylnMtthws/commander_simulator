#pragma once

// Reading a card database from the file the exporter writes.
//
// Note what this header does NOT include: nlohmann/json. The JSON library is a
// private implementation detail of card_db_load.cpp, so nothing that loads a
// card database inherits a parser.

#include <filesystem>
#include <stdexcept>
#include <string>

#include "core/card.hpp"

namespace cs::io {

// Thrown for any file that cannot be trusted: unreadable, malformed, or
// internally inconsistent. The message always names the specific problem and,
// where it makes sense, the card.
//
// There is deliberately no "load as much as possible" path. A partially loaded
// deck is the section 2.7 failure mode in a new costume - the simulation would
// run happily on 94 cards and report a plausible number.
class LoadError : public std::runtime_error {
public:
    explicit LoadError(const std::string& what) : std::runtime_error(what) {}
};

[[nodiscard]] CardDb load_card_db(const std::filesystem::path& path);

}  // namespace cs::io

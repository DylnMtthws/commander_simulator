#pragma once

// Reading the deck file: patterns, engines, and the declared assumptions.
//
// All validation happens HERE, at load, and every failure is fatal. Never
// mid-simulation - a simulation that can fail is a simulation whose failures
// correlate with the seed (SIM_PLAN.md section 9.3).

#include <filesystem>
#include <stdexcept>
#include <string>

#include "core/card.hpp"
#include "core/pattern.hpp"

namespace cs::io {

class DeckError : public std::runtime_error {
public:
    explicit DeckError(const std::string& what) : std::runtime_error(what) {}
};

struct TableContext {
    int opponents = 0;
    bool on_the_play = true;
};

struct DeckFile {
    std::string name;
    std::string commander;
    TableContext table;
    std::string ablation_replacement;
    PatternSet patterns;
};

// Parses and validates against a loaded card database. The database is required
// because half the validation is "does this name exist in the deck at all".
[[nodiscard]] DeckFile load_deck(const std::filesystem::path& path, const CardDb& db);

}  // namespace cs::io

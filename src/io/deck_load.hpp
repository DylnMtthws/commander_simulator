#pragma once

// Reading the deck file: patterns, engines, and the declared assumptions.
//
// All validation happens HERE, at load, and every failure is fatal. Never
// mid-simulation - a simulation that can fail is a simulation whose failures
// correlate with the seed (SIM_PLAN.md section 9.3).

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "core/pattern.hpp"
#include "core/policy.hpp"

namespace cs::io {

class DeckError : public std::runtime_error {
public:
    explicit DeckError(const std::string& what) : std::runtime_error(what) {}
};

// Mirrors core's TableContext; kept separate so io owns the parsing and core
// owns the meaning.
struct DeckTable {
    int opponents = 0;
    ColourMask opponent_colors = 0;
    bool on_the_play = true;
};

// Where the decklist came from, and when.
//
// A snapshot of a living list is a claim about a moment. Without a date it is a
// claim about nothing checkable - see the block at the top of
// data/kinnan.deck.toml.
struct Provenance {
    std::string source;
    std::string source_url;   // may be empty; the header says so loudly
    std::string snapshot_date;
    std::string snapshot_note;
    std::string cards_sha256;
};

// Commander/deck-specific strategy, explicitly versioned. The mechanics stay
// in core; this metadata says which authored policy/pattern package may use
// them and makes accidental Kinnan fallback rejectable at the service edge.
struct StrategyPack {
    std::string id;
    std::string version;
    std::vector<std::string> supported_commander_oracle_ids;
    std::vector<std::string> supported_effect_definitions;
    std::vector<std::string> assembly_objectives;
    std::string play_policy_implementation;
    std::vector<std::string> declared_table_assumptions;
    std::vector<std::string> known_blind_spots;
};

struct DeckFile {
    std::string name;
    std::string commander;
    StrategyPack strategy_pack;
    Provenance provenance;
    DeckTable table;
    std::string ablation_replacement;
    PatternSet patterns;
    PolicyWeights weights;
    int life_floor = 10;
    bool derived = false;
    std::vector<std::string> rank_override_names;
};

// Parses and validates against a loaded card database. The database is required
// because half the validation is "does this name exist in the deck at all".
[[nodiscard]] DeckFile load_deck(const std::filesystem::path& path, const CardDb& db);

}  // namespace cs::io

#pragma once

// Authored card effects, as the simulation sees them.
//
// Phase 7 authors these in DEPENDENCY ORDER: MANA_SOURCE and
// STATIC_MANA_MODIFIER first, because Kinnan's multiplier is the piece every
// other effect is measured through.

#include <cstdint>
#include <string>
#include <vector>

#include "core/card.hpp"
#include "core/mana.hpp"
#include "core/state.hpp"

namespace cs {

enum class AuthorStatus : std::uint8_t { Unauthored = 0, Modeled, Inert };

// What decides whether a land enters tapped.
//
// RULE K1 (SIM_PLAN.md section 4.2): a flag carrying per-card predicate logic
// is a kind wearing a flag's clothes, and the threshold for promoting it is the
// FOURTH distinct shape. These are the three that exist. Authoring every land
// in the deck did not add a fourth, so the flag stays a flag.
enum class EntersTappedUnless : std::uint8_t {
    Never = 0,      // enters untapped unconditionally
    LandCount,      // Botanical Sanctum: unless you control <= N other lands
    PayLife,        // Breeding Pool, Soporific Springs: unless you pay N life
    OpponentCount,  // Rejuvenating Springs: unless you have >= N opponents
};

struct ManaSourceEffect {
    ColourMask produces = 0;
    bool colours_from_table = false;  // Exotic Orchard, Fellwar Stone
    std::uint8_t amount = 1;
    bool is_land = false;
    bool is_creature = false;
    bool untaps_normally = true;
    EntersTappedUnless enters_tapped_unless = EntersTappedUnless::Never;
    int enters_tapped_param = 0;
    // Recorded and unused: life is not tracked, so every life cost is treated
    // as free, which OVERSTATES the deck. Kept so the data is already here the
    // day it matters.
    int life_cost = 0;
};

enum class ModifierMode : std::uint8_t { Multiply, GrantCreatureMana };

struct ModifierEffect {
    ModifierMode mode = ModifierMode::Multiply;
    std::uint8_t bonus = 1;
    bool nonland_only = true;
    ColourMask grants = 0;
    std::uint8_t grant_amount = 1;
};

struct CardEffects {
    AuthorStatus status = AuthorStatus::Unauthored;
    bool has_mana_source = false;
    ManaSourceEffect mana_source;
    bool has_modifier = false;
    ModifierEffect modifier;
    std::string reason_category;
};

struct EffectDb {
    std::vector<CardEffects> by_slot;
    int modeled = 0;
    int inert = 0;
    int unauthored = 0;
    std::vector<std::string> inert_categories;   // parallel arrays, sorted
    std::vector<int> inert_counts;
};

// What the table looks like, from the deck file's [table] block. Read only by
// effects whose text tests it (section 2.8).
struct TableContext {
    int opponents = 0;
    ColourMask opponent_colors = 0;
    bool on_the_play = true;
};

// Mana available from the battlefield, with modifiers applied.
void collect_sources(const CardDb& db, const EffectDb& effects, const GameState& state,
                     const TableContext& table, std::vector<Source>& out);

// Does this land enter tapped, given the board and the declared table context?
[[nodiscard]] bool enters_tapped(const ManaSourceEffect& effect, const CardDb& db,
                                 const EffectDb& effects, const GameState& state,
                                 const TableContext& table) noexcept;

}  // namespace cs

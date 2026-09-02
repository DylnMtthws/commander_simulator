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

// A DYNAMIC_MANA_SOURCE's gate. The difference from MANA_SOURCE is that the
// loop must EVALUATE something rather than read a constant, which is the test
// for a kind rather than a flag (section 4.2).
enum class SourceCondition : std::uint8_t {
    None = 0,
    ArtifactCountGte,           // Mox Opal's metalcraft
    LegendaryCreatureCountGte,  // Mox Amber
    UntappedCreatureGte,        // Springleaf Drum
    UntappedPermanentGte,       // Gene Pollinator
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
    SourceCondition condition = SourceCondition::None;
    int condition_param = 0;
    // What tapping this source costs in life. Ancient Tomb 2, City of Brass 1,
    // Tarnished Citadel 3, and so on.
    int life_cost = 0;
};

// FETCH: sacrifice for a land from the library, then shuffle. `finds` matches
// against all_types, so land subtypes make the predicate per-card.
struct FetchEffect {
    std::vector<std::string> finds;
    int life_cost = 0;
};

// CARD_COST: paid in cards from hand, not mana.
enum class CardFilter : std::uint8_t { Any = 0, Land, Nonland };

struct CardCostEffect {
    int cards = 1;
    CardFilter filter = CardFilter::Any;
};

// RITUAL: one-shot mana from somewhere other than tapping a permanent.
enum class RitualZone : std::uint8_t { Battlefield = 0, Hand };

struct RitualEffect {
    ColourMask produces = 0;
    std::uint8_t amount = 1;
    RitualZone from_zone = RitualZone::Battlefield;
};

// MASS_UNTAP: a one-shot untap of a set of permanents. Dramatic Reversal is
// the only one here. Straightforward under a detect-not-execute model, because
// it happens once when it resolves rather than looping.
struct MassUntapEffect {
    bool nonland_only = true;
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
    bool has_fetch = false;
    FetchEffect fetch;
    bool has_card_cost = false;
    CardCostEffect card_cost;
    bool has_ritual = false;
    RitualEffect ritual;
    bool has_mass_untap = false;
    MassUntapEffect mass_untap;
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

    // Life below which the deck stops paying life for mana.
    //
    // A FLOOR, not a scored decision, and that is the whole reason life stayed
    // small. With a floor, "pay 2 life for an untapped Breeding Pool" is a
    // constant-time rule rather than a term the scorer has to weigh - so life
    // gates which SOURCES EXIST and never enters can_pay or the policy at all.
    //
    // With no opponents nothing attacks, so the only pressure on life is the
    // deck's own mana base. A floor keeps that from becoming absurd over twelve
    // turns of Ancient Tomb without pretending to model a real life total.
    int life_floor = 10;
};

// Mana available from the battlefield, with modifiers applied.
void collect_sources(const CardDb& db, const EffectDb& effects, const GameState& state,
                     const TableContext& table, std::vector<Source>& out);

// What tapping this set of sources would cost in life, given the same board.
// The turn loop deducts it; nothing else needs to know.
[[nodiscard]] int life_cost_of_tapping(const EffectDb& effects, const GameState& state,
                                       int slot) noexcept;

// Is this dynamic source's condition met right now?
[[nodiscard]] bool condition_met(const ManaSourceEffect& effect, const CardDb& db,
                                 const EffectDb& effects, const GameState& state) noexcept;

// Library slots this fetch could find. Empty means the fetch does nothing,
// which is a real outcome worth seeing rather than an error.
void fetch_candidates(const FetchEffect& fetch, const CardDb& db, const GameState& state,
                      std::vector<int>& out);

// Does this land enter tapped, given the board and the declared table context?
[[nodiscard]] bool enters_tapped(const ManaSourceEffect& effect, const CardDb& db,
                                 const EffectDb& effects, const GameState& state,
                                 const TableContext& table) noexcept;

}  // namespace cs

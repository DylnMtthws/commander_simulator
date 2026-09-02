#pragma once

// Authored card effects, as the simulation sees them.
//
// Phase 7 authors these in DEPENDENCY ORDER: MANA_SOURCE and
// STATIC_MANA_MODIFIER first, because Kinnan's multiplier is the piece every
// other effect is measured through.

#include <cstdint>
#include <string>
#include <string_view>
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

// TUTOR: search the library by a predicate, move the result to a named zone.
//
// DESTINATION IS NOT COSMETIC. A tutor to the battlefield puts a permanent into
// play now; a tutor to hand puts a card you must still cast, next turn, for its
// mana cost. Scoring both as "this card arrives" would make Trophy Mage look
// like Finale of Devastation.
//
// The scorer handles it by building the hypothetical state in the RIGHT ZONE:
// a to-hand tutor's candidate is scored with the card in hand, so it can only
// complete a pattern with an `in_hand` term, and a to-battlefield tutor's is
// scored on the battlefield. No lookahead is added - the scorer still sees only
// the current state plus one card - and the turn of delay is simply not
// credited, which is an approximation in the CONSERVATIVE direction.
enum class TutorDestination : std::uint8_t { Battlefield = 0, Hand };

// What a tutor may find.
enum class TutorFilter : std::uint8_t {
    Any = 0,
    Creature,
    NonHumanCreature,  // Invasion of Ikoria, and Kinnan's own dig
    Artifact,
    Land,
};

struct TutorEffect {
    TutorFilter filter = TutorFilter::Any;
    TutorDestination destination = TutorDestination::Battlefield;
    // -1 == no limit. Trophy Mage is exactly 3; Finale is "X or less", which
    // makes the cap a function of the mana spent rather than a constant.
    int max_mana_value = -1;
    bool max_from_x = false;
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

// CLONE: enter as a copy of a permanent already on the battlefield.
//
// A clone's value is entirely "what is the best permanent available to copy",
// which the existing scorer answers directly - the candidate set is the
// battlefield instead of the hand and nothing else changes. What it DID need is
// state (GameState::copy_of), because a copy is genuinely another card.
//
// A clone with no legal target is DEAD, not merely mediocre, and is scored as
// uncastable rather than as zero.
enum class CloneFilter : std::uint8_t { NonlandPermanent = 0, Artifact, Enchantment, Creature,
                                        ArtifactOrEnchantment };

struct CloneEffect {
    CloneFilter filter = CloneFilter::NonlandPermanent;
    bool max_from_x = false;  // Mockingbird: mana value <= mana spent
};

// DRAW: put N cards from the library into hand, on resolution.
//
// The kind arrived for BORNE UPON A WIND, and how it arrived is the point. The
// card's first line grants flash, which is `timing_only` and inert here - the
// same reading that made High Fae Trickster inert. Its second line is "Draw a
// card". Categorising on the first line and stopping would have filed a cantrip
// under `timing_only` and made it invisible.
struct DrawEffect {
    int cards = 1;
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
    bool has_tutor = false;
    TutorEffect tutor;
    bool has_clone = false;
    CloneEffect clone;
    bool has_draw = false;
    DrawEffect draw;

    // CONVOKE is a property of the COST, not an effect, which is why it is a
    // card-level flag and not a member of the closed kind set. Section 4.2's
    // test: it does not ask the loop for a verb it lacks, it changes what may
    // pay for an existing one. Chord of Calling is the only user.
    bool convoke = false;

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

// What each reason_category MEANS, for the run report's grouped table.
//
// It lives beside the closed set rather than in the CLI because section 9.5's
// third rule says an assumption's text is emitted by the thing that owns the
// assumption: a hand-written caveat block goes stale, and one generated from
// the enum cannot, since changing the enum changes the text.
//
// Returns nullptr for a category outside the set, which is what the loader
// tests to reject one.
[[nodiscard]] const char* reason_category_meaning(std::string_view category) noexcept;

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

// Library slots this tutor could find, given the mana actually available.
void tutor_candidates(const TutorEffect& tutor, const CardDb& db, const GameState& state,
                      int mana_available, std::vector<int>& out);

// Battlefield slots this clone could copy.
void clone_candidates(const CloneEffect& clone, const CardDb& db, const GameState& state,
                      int mana_available, std::vector<int>& out);

// APPENDS the mana convoke makes available to one particular card.
//
// Convoke pays {1} or one mana of a tapped creature's colour, so each eligible
// creature is worth exactly one Source of its colour identity. Two properties
// are deliberate and both are stated in data/effects.toml:
//
//   * A creature that already taps for mana is NOT offered. Under Kinnan it
//     produces two mana and convoking it produces one, so tapping it for mana
//     weakly dominates convoking it in every board this deck reaches. That
//     makes the omission exact rather than an approximation.
//   * Convoke mana is NOT multiplied. Kinnan triggers on tapping a nonland
//     permanent FOR MANA; convoke is a cost payment and taps no mana ability.
//
// The result is per-card and must never be merged into collect_sources - these
// sources exist only while paying for THIS spell.
void convoke_sources(const CardDb& db, const EffectDb& effects, const GameState& state,
                     std::vector<Source>& out);

// The battlefield slots convoke may tap, ascending. One predicate, two callers:
// the scorer wants Sources and the turn loop wants slots to tap, and deriving
// them from different tests is how they would come to disagree.
void convoke_slots(const CardDb& db, const EffectDb& effects, const GameState& state,
                   std::vector<int>& out);

// Does this card stay on the battlefield after it resolves?
//
// Battle is deliberately excluded: SIM_PLAN.md section 4.6 decided the model
// never gains the type, because a Siege flips by being attacked and there is no
// combat here - so Invasion of Ikoria's ETB is authored and the permanent is
// discarded.
[[nodiscard]] bool is_permanent(const Card& card) noexcept;

// Does this land enter tapped, given the board and the declared table context?
[[nodiscard]] bool enters_tapped(const ManaSourceEffect& effect, const CardDb& db,
                                 const EffectDb& effects, const GameState& state,
                                 const TableContext& table) noexcept;

}  // namespace cs

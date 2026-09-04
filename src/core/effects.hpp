#pragma once

// Authored card effects, as the simulation sees them.
//
// Phase 7 authors these in DEPENDENCY ORDER: MANA_SOURCE and
// STATIC_MANA_MODIFIER first, because Kinnan's multiplier is the piece every
// other effect is measured through.

#include <cstdint>
#include <span>
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
    // EXACTLY this mana value, not "this or less". -1 == no restriction.
    //
    // The name was `max_mana_value` and the comparison was `>`, which made all
    // three constant-cap tutors in this deck able to find anything CHEAPER than
    // their real restriction. Verified against the printed text of each:
    //
    //   Trophy Mage        "an artifact card with mana value 3"
    //   Drift of Phantasms transmute: "a card with the SAME mana value as this"
    //   Dizzy Spell        transmute: the same, and this card is 1
    //
    // None of them says "or less". The authoring knew - Trophy Mage's comment in
    // data/effects.toml read "Exactly mana value 3" while the field it set was
    // named for a maximum and compared as one. The comment and the code
    // disagreed and the code won, silently, for eight phases.
    int mana_value_exactly = -1;

    // "X or less", which is a genuine maximum and a function of the mana spent
    // rather than a constant. Finale of Devastation, Chord of Calling,
    // Nature's Rhythm, Invasion of Ikoria.
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

// SELECT: look at N cards, choose K by a policy decision, dispose of the rest.
//
// §4.2's kind 7, declared in the closed set from the start and unimplemented
// until R3 (§16.7b). Kinnan's dig is its first implementation and its third user.
//
// AND KINNAN'S DIG IS NOT A TUTOR, which is what data/effects.toml called it for
// eight phases. The difference is not pedantic: a TUTOR searches the whole
// library and always finds; this looks at five cards and **misses 26% of the
// time** (§16.7's measurement). Modelling it as a tutor would have overstated
// the deck's primary card-advantage engine by exactly that margin, and would
// have made a hand holding Kinnan look like a hand holding a tutor.
//
// The cost is carried here rather than inferred, because an activated ability's
// cost is not its card's mana cost - Kinnan costs {G}{U} and his dig costs
// {5}{G}{U}.
struct SelectEffect {
    int look = 5;                  // cards revealed
    // NO `take` FIELD. It was written, loaded, and never read - Kinnan's dig
    // keeps at most one and no other SELECT is authored. The every-field-has-a-
    // reader check (scripts/check_effects_are_read.sh) caught it in the same
    // session that added it, which is the first time that check has fired on
    // anything but a deliberate test. A field with no user is speculation
    // (§4.2), and Sylvan Library's "draw two, keep both or bottom them" can add
    // it when Sylvan Library is authored.
    TutorFilter filter = TutorFilter::Any;
    TutorDestination destination = TutorDestination::Battlefield;
    std::uint8_t cost_generic = 0;
    std::array<std::uint8_t, kColourCount> cost_pips{};
    // Activatable as often as the mana allows. Kinnan's dig has no tap symbol
    // (§16.7) and no once-per-turn clause.
    bool repeatable = true;
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

// EXILE_LIBRARY is a verb rather than a DRAW flag: cards leave the library for
// exile and never become available to cast. `leave` is explicit authoring data
// because "empty the library" and "leave one" produce different Oracle states.
struct ExileLibraryEffect {
    int leave = 0;
};

// MILL moves cards from library to graveyard. Scaling by the current per-turn
// storm count is a quantity flag on that verb, not another effect kind.
struct MillEffect {
    int cards = 0;
    bool times_storm = false;
};

// ESCAPE grants the verb "cast a nonland card from your graveyard" and pays
// its non-mana cost by exiling other graveyard cards. Underworld Breach is the
// first user. The spell's printed mana cost remains the mana cost.
struct EscapeEffect {
    int exile_cards = 0;
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
    bool has_select = false;
    SelectEffect select;
    bool has_exile_library = false;
    ExileLibraryEffect exile_library;
    bool has_mill = false;
    MillEffect mill;
    bool has_escape = false;
    EscapeEffect escape;

    // CONVOKE is a property of the COST, not an effect, which is why it is a
    // card-level flag and not a member of the closed kind set. Section 4.2's
    // test: it does not ask the loop for a verb it lacks, it changes what may
    // pay for an existing one. Chord of Calling is the only user.
    bool convoke = false;

    std::string reason_category;

    // Set when the author believes this card's `inert` classification is WRONG
    // but has not re-authored it.
    //
    // The inert table is the model's statement of its own limits (section 4.4),
    // and a reader is entitled to know that one entry in it is disputed by the
    // person who wrote it. Printing it is the difference between a documented
    // error and a hidden one: a wrong category noted in a comment is auditable
    // only by someone reading the effects file, and this belongs in front of
    // anyone reading a number.
    std::string disputed;
};

struct EffectDb {
    std::vector<CardEffects> by_slot;
    int modeled = 0;
    int inert = 0;
    int unauthored = 0;
    // Inert entries the author believes are misclassified. Counted separately
    // because "31 cards the model cannot see" and "1 of those 31 is filed wrong"
    // are different claims and a reader needs both.
    int disputed = 0;
    // Card names whose text is not represented. Kept explicitly so both the
    // human header and the service result can emit one warning per card; a
    // count alone does not identify the approximation a consumer inherited.
    std::vector<std::string> unauthored_names;
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
// `self` is the slot being evaluated, and it is EXCLUDED from the count.
//
// Gene Pollinator's cost is "{T}, Tap an untapped permanent you control" - the
// {T} taps Gene Pollinator, so the permanent tapped as the additional cost is
// necessarily a DIFFERENT one. Counting itself made `untapped_permanent_gte = 1`
// true whenever Gene Pollinator was untapped, which is exactly when the
// condition is asked. The gate was always open.
[[nodiscard]] bool condition_met(const ManaSourceEffect& effect, const CardDb& db,
                                 const EffectDb& effects, const GameState& state,
                                 int self) noexcept;

// Library slots this fetch could find. Empty means the fetch does nothing,
// which is a real outcome worth seeing rather than an error.
void fetch_candidates(const FetchEffect& fetch, const CardDb& db, const GameState& state,
                      std::vector<int>& out);

// Library slots this tutor could find, given the mana actually available.
void tutor_candidates(const TutorEffect& tutor, const CardDb& db, const GameState& state,
                      int mana_available, std::vector<int>& out);

// Which of the revealed cards a SELECT may keep. Filters by the same predicate
// a tutor uses, over a candidate set of five instead of the whole library.
void select_candidates(const SelectEffect& select, const CardDb& db,
                       std::span<const int> revealed, std::vector<int>& out);

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

// Cards in hand that could pay a CARD_COST, ascending by slot.
//
// Chrome Mox exiles a nonland card from hand; Mox Diamond discards a land. Both
// were AUTHORED, validated as required by the loader, and never charged - the
// `has_card_cost` flag had zero readers for four phases, so both moxen were
// free. RULE K2 (section 4.2) singles this kind out as sitting on the value
// function's most sensitive input, which made it the worst one to leave
// uncalled: the sweep had both in its top eight.
//
// A guard with no caller, in the ingestion repo's PLAN.md 11.0 sense, and it is
// listed there under a mechanism quietly not running because nothing about the
// output distinguishes a free Chrome Mox from a paid one.
void card_cost_candidates(const CardCostEffect& cost, const CardDb& db, const GameState& state,
                          std::vector<int>& out);

// Returns the number of OTHER graveyard cards required to escape `slot`, or
// -1 when no active effect grants permission.
[[nodiscard]] int escape_exile_cost(const CardDb& db, const EffectDb& effects,
                                    const GameState& state, int slot) noexcept;

void escape_cost_candidates(const GameState& state, int escaping,
                            std::vector<int>& out);

// Puts a permanent onto the battlefield, with everything that entails.
//
// ONE DEFINITION, because there were four entry points and only one of them was
// complete. The land drop applied enters_tapped AND paid the life for entering
// untapped; the fetch applied enters_tapped and PAID NO LIFE, so a fetched
// Breeding Pool entered untapped for free while a played one cost 2; and the
// cast and tutor-to-battlefield paths checked neither, which is currently
// unexercised only because no nonland in this deck enters tapped.
//
// The twelfth rule in the ingestion repo's PLAN.md 11.0, and the same shape that
// let convoke bypass the mana system: a second derivation of a concept that
// already had one.
void enter_battlefield(const CardDb& db, const EffectDb& effects, GameState& state,
                       const TableContext& table, int slot);

// Does this land enter tapped, given the board and the declared table context?
// `self` is the land entering, and it is EXCLUDED from the land count.
//
// Botanical Sanctum "enters tapped unless you control two or fewer OTHER lands",
// and enter_battlefield puts it on the battlefield before asking - so it counted
// itself and entered tapped one land earlier than the card says. The word in the
// comment beside this enum was already "other"; the code never implemented it.
[[nodiscard]] bool enters_tapped(const ManaSourceEffect& effect, const CardDb& db,
                                 const EffectDb& effects, const GameState& state,
                                 const TableContext& table, int self) noexcept;

}  // namespace cs

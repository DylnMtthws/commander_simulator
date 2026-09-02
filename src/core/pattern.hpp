#pragma once

// Win patterns: conjunctions over game state.
//
// SIM_PLAN.md section 5. Two levels and no more: ENGINES set flags, WINS
// reference flags and cards, and neither may reference its own kind. That is
// exactly the indirection the engine/outlet structure needs and no more, so
// there are no cycles to detect.
//
// NAMING, enforced rather than documented: a pattern names a STATE, not an
// outcome. This deck contains no card that reads "you win the game" - every
// real kill is opponent-facing and outside the model (section 5.4) - so the
// quantity reported is TURNS TO ASSEMBLY. `validate` rejects a pattern named
// like an outcome, because the one place a caveat survives is the name.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <span>

#include "core/card.hpp"
#include "core/mana.hpp"
#include "core/state.hpp"

namespace cs {

using FlagMask = std::uint32_t;
inline constexpr std::size_t kMaxFlags = 32;

// No pattern fired. A named constant rather than a magic number, and never
// stored in a field that also holds a real id (section 7.2).
inline constexpr std::uint8_t kNoPattern = 0xFF;

// A conjunction. Every populated term must hold; there is no OR and no
// negation. Two alternatives are two patterns, which keeps "which pattern
// fired" meaningful, and negation is banned because a term firing on the
// ABSENCE of something is how a modelling gap becomes a silent success.
struct Requirement {
    Zone in_play;           // all of these on the battlefield
    Zone in_hand;           // all of these in hand
    Zone in_play_or_hand;   // each of these in either zone
    Zone untapped;          // all of these present AND untapped
    Zone any_of;            // at least one of these on the battlefield
    bool has_any_of = false;
    FlagMask flags = 0;     // all of these flags set by some engine this turn
    int turn_gte = 0;
    int creature_count_gte = 0;
    int library_size_lte = -1;  // -1 == not required

    // The cost, in generic mana, of ENTERING a self-untap loop.
    //
    // This is the term that makes an engine a claim about the board rather than
    // a claim about which cards are on it. Kinnan + Basalt is only unbounded if
    // you can actually pay the {3} to untap - and note the check needs no
    // special case for whether Basalt is currently tapped, because an untapped
    // Basalt is ITSELF one of the sources offering 4:
    //
    //   Basalt untapped -> sources include 4x{C}, so {3} is payable        (loop)
    //   Basalt tapped   -> it is not a source, so {3} must come from elsewhere
    //
    // Authored this way from the start rather than as "both pieces are on the
    // board", because presence-only fires with Kinnan, a tapped Basalt and two
    // mana - overstating the deck exactly at the turn boundaries the CDF is
    // most sensitive to, and that is the kind of thing simplified now and
    // tightened never.
    int loop_entry_cost = -1;  // -1 == not required

    // How many activations the pattern claims are needed. REQUIRED wherever
    // loop_entry_cost is, with no default, because it is a JUDGEMENT ABOUT
    // MAGIC and not a fact about the card - and it is worth 2.97 points of
    // P(assembled by turn 3) between 1 and 2, which is more than every card in
    // the deck except three (SIM_PLAN.md section 16.7).
    //
    // Splitting it from loop_entry_cost separates the two kinds of claim. The
    // cost is read off the card and can be checked against printed text; the
    // count is someone deciding when a line has been assembled, and it belongs
    // in the honesty header beside opponents and on_the_play.
    int activations = 1;

    // COLOURED pips of the entry cost - R2 in §16.7's option set.
    //
    // §16.7 rejected a coloured entry cost for Kinnan's dig, because that
    // constraint genuinely is a QUANTITY: under WIDE_COLOUR every creature taps
    // for any colour, so a board that can pay 7N can find the pips among it.
    //
    // `infinite_C_outlet_in_hand` is the opposite case and needs exactly this.
    // Its requirement is that Thrasios be CAST for {G}{U} while the engine
    // supplies only {C} - a colour question with no quantity in it at all, and
    // one a generic entry cost cannot express however large you make it. Same
    // machinery, and it fits here because the constraint has a different shape.
    std::array<std::uint8_t, kColourCount> entry_pips{};

    // Set when a card this requirement names has been ABLATED out of the deck
    // (core/ablation.hpp). Requirements compile to SLOT MASKS at load, and a
    // slot swap silently rebinds them: with Enduring Vitality replaced by a
    // Forest, `in_play = [Kinnan, <that slot>]` asks for Kinnan and a Forest,
    // which the deck plays almost every game. The engine then fired far MORE
    // often without its own key card, and the sweep read Enduring Vitality as
    // costing the deck 7.6 points.
    //
    // Removing the slot from the mask would be worse - it makes the requirement
    // EASIER. An ablated card's pattern has to become unsatisfiable, which is
    // what this flag says, and it is a bool rather than a mask trick so that it
    // is visible in a debugger and in this comment.
    bool impossible = false;
};

struct Engine {
    std::string name;
    Requirement requires_;
    FlagMask sets = 0;
};

struct WinPattern {
    std::string name;
    Requirement requires_;
};

struct PatternSet {
    std::vector<std::string> flag_names;
    std::vector<Engine> engines;
    std::vector<WinPattern> patterns;

    // Slots that are creatures, precomputed at load so creature_count_gte is a
    // mask-and-popcount rather than a type-string scan per turn.
    Zone creature_slots;
};

[[nodiscard]] bool requirement_holds(const Requirement& requirement, const PatternSet& set,
                                     const GameState& state, FlagMask active,
                                     std::span<const Source> sources) noexcept;

// Flags set by every engine whose requirement currently holds.
[[nodiscard]] FlagMask active_flags(const PatternSet& set, const GameState& state,
                                    std::span<const Source> sources) noexcept;

// Index of the first satisfied pattern, or -1. Declaration order is the
// tiebreak, so two patterns true on the same turn always resolve the same way
// (section 6.5).
[[nodiscard]] int first_satisfied(const PatternSet& set, const GameState& state,
                                  std::span<const Source> sources) noexcept;

// Bit i set if pattern i's requirement holds. Used to tell a DEAD pattern from
// a SHADOWED one: patterns are checked in declaration order and the first hit
// wins, so an overlapping pattern declared later never fires even when its
// state is reached. Without this the never-fired report cannot distinguish
// "this line does not happen" from "this line always happens at the same time
// as an earlier one", and those call for opposite responses.
[[nodiscard]] FlagMask all_satisfied(const PatternSet& set, const GameState& state,
                                     std::span<const Source> sources) noexcept;

}  // namespace cs

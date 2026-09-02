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

#include <cstdint>
#include <string>
#include <vector>

#include "core/card.hpp"
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
                                     const GameState& state, FlagMask active) noexcept;

// Flags set by every engine whose requirement currently holds.
[[nodiscard]] FlagMask active_flags(const PatternSet& set, const GameState& state) noexcept;

// Index of the first satisfied pattern, or -1. Declaration order is the
// tiebreak, so two patterns true on the same turn always resolve the same way
// (section 6.5).
[[nodiscard]] int first_satisfied(const PatternSet& set, const GameState& state) noexcept;

// Bit i set if pattern i's requirement holds. Used to tell a DEAD pattern from
// a SHADOWED one: patterns are checked in declaration order and the first hit
// wins, so an overlapping pattern declared later never fires even when its
// state is reached. Without this the never-fired report cannot distinguish
// "this line does not happen" from "this line always happens at the same time
// as an earlier one", and those call for opposite responses.
[[nodiscard]] FlagMask all_satisfied(const PatternSet& set, const GameState& state) noexcept;

}  // namespace cs

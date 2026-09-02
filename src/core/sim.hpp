#pragma once

// The turn loop.
//
// Phase 3 scope: untap, draw, land drop, cast, repeat. There are no win
// patterns yet (Phase 4), so a game here runs to the turn cap and reports what
// happened rather than whether anything was achieved.

#include <cstdint>

#include "core/card.hpp"
#include "core/policy.hpp"
#include "core/state.hpp"

namespace cs {

// Per-game counters.
//
// can_pay_calls is the one that matters. Section 11.1 measured can_pay at ~65
// ns and noted that the number deciding the budget is calls-per-game, which
// needs a policy to exist. Counting from the first turn loop is far cheaper
// than reconstructing it later, and the counter costs an increment on a path
// that already does a search.
struct GameStats {
    std::uint32_t can_pay_calls = 0;
    std::uint32_t turns = 0;
    std::uint32_t cards_drawn = 0;
    std::uint32_t lands_played = 0;
    std::uint32_t spells_cast = 0;
};

struct GameResult {
    GameStats stats;
    std::uint8_t turns_simulated = 0;

    // A hash of the final game state: library order, every zone, and the turn.
    //
    // Exists because aggregate counters are a WEAK signature. The seeding tests
    // initially compared only counts and reported that two different seeds
    // produced the same game - true of the counters, false of the game. On a
    // small fixture the stub plays the same number of things whatever it draws,
    // so counts cannot see the draw order at all.
    //
    // Comparing state rather than summaries is also what the Phase 5 golden
    // traces will need.
    std::uint64_t state_digest = 0;
};

struct GameConfig {
    std::uint8_t turn_cap = 12;
    std::uint8_t opening_hand = 7;
    // From the deck file's [table] block (section 2.8). Required there, so it
    // is never silently defaulted; the value here is only a struct default.
    bool on_the_play = true;
};

// Plays one game. A pure function of (db, config, seed) - INVARIANT S1.
[[nodiscard]] GameResult run_game(const CardDb& db, const GameConfig& config, const Policy& policy,
                                  std::uint64_t seed);

}  // namespace cs

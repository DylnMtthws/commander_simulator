#pragma once

// The decision boundary.
//
// SIM_PLAN.md section 6.1: ALL decisions live behind Policy, and GameState
// contains no decision logic. If the state machine ever "chooses", the boundary
// has leaked. The interface exists now because it is nearly free and expensive
// to retrofit; the real authored policy is Phase 5 and the search-based one is
// not designed at all.

#include "core/card.hpp"
#include "core/mana.hpp"
#include "core/state.hpp"

namespace cs {

struct GameStats;

class Policy {
public:
    virtual ~Policy() = default;

    // Which card in hand to play as a land this turn, or -1 for none.
    [[nodiscard]] virtual int choose_land(const CardDb& db, const GameState& state) const = 0;

    // Which card in hand to cast next, or -1 to stop casting this turn.
    [[nodiscard]] virtual int choose_spell(const CardDb& db, const GameState& state,
                                           std::span<const Source> sources,
                                           GameStats& stats) const = 0;

    // Shown in traces and run output so a number is never separated from the
    // piloting that produced it.
    [[nodiscard]] virtual const char* name() const = 0;
};

// ===========================================================================
// NOT THE REAL POLICY. NOT A DRAFT OF THE REAL POLICY.
// ===========================================================================
//
// This exists to prove the turn loop runs, and for no other purpose. It is
// deliberately, visibly stupid:
//
//   * It plays the LOWEST-NUMBERED land in hand. Not the best land, not an
//     untapped one - the lowest slot index, which is alphabetical by card name.
//   * It casts the first thing it can afford, in the same order, with no notion
//     of whether casting it helps.
//   * It never holds mana, never sequences, never considers what it is building
//     towards, and has never heard of a win condition.
//
// Any number produced with this policy in place is a number about the LOOP, not
// about the deck. Section 6.2's authored policy replaces it wholesale in Phase
// 5; nothing here should be carried forward.
class StubPolicyDoNotUseForResults final : public Policy {
public:
    [[nodiscard]] int choose_land(const CardDb& db, const GameState& state) const override;
    [[nodiscard]] int choose_spell(const CardDb& db, const GameState& state,
                                   std::span<const Source> sources,
                                   GameStats& stats) const override;
    [[nodiscard]] const char* name() const override {
        return "StubPolicyDoNotUseForResults (plays the lowest-indexed legal thing)";
    }
};

}  // namespace cs

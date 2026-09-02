#pragma once

// The decision boundary.
//
// SIM_PLAN.md section 6.1: ALL decisions live behind Policy, and GameState
// contains no decision logic. If the state machine ever "chooses", the boundary
// has leaked. The interface exists now because it is nearly free and expensive
// to retrofit; the real authored policy is Phase 5 and the search-based one is
// not designed at all.

#include <vector>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "core/mana.hpp"
#include "core/observer.hpp"
#include "core/pattern.hpp"
#include "core/state.hpp"

namespace cs {

struct GameStats;

// Everything a decision is allowed to see. Bundled so the signature does not
// grow a parameter every time a term is added, and so it is visibly a view of
// the CURRENT state: there is no move list here, which is the wall against the
// scorer becoming a search (section 6.3).
struct Context {
    const CardDb& db;
    const PatternSet& patterns;
    const GameState& state;
    std::span<const Source> sources;
    Observer* observer = nullptr;
    // Nullable so existing tests can build a Context without one. Present in
    // every real call; only the clone check reads it.
    const EffectDb* effects = nullptr;
};

class Policy {
public:
    virtual ~Policy() = default;

    // Which card in hand to play as a land this turn, or -1 for none.
    [[nodiscard]] virtual int choose_land(const Context& context, GameStats& stats) const = 0;

    // Which card to cast next, or -1 to stop casting this turn.
    [[nodiscard]] virtual int choose_spell(const Context& context, GameStats& stats) const = 0;

    // Which card to tutor for, or -1 for none. `to_hand` decides which zone
    // the hypothetical is built in, which is the whole of how destination is
    // handled (core/effects.hpp).
    [[nodiscard]] virtual int choose_tutor(const Context& context, std::span<const int> candidates,
                                           bool to_hand, GameStats& stats) const = 0;

    // Which permanent to copy, or -1 if there is nothing legal to copy.
    [[nodiscard]] virtual int choose_clone(const Context& context, std::span<const int> candidates,
                                           GameStats& stats) const = 0;

    // Which of the REVEALED cards a SELECT keeps, or -1 to keep none.
    //
    // The same scorer over a candidate set of five. §6.3's wall against lookahead
    // does not apply: the five cards have been LOOKED AT, so they are current
    // information exactly like a tutor's candidates, and scoring them evaluates
    // no hypothetical future position. This is the one decision in the interface
    // the existing scorer fits without stretching.
    [[nodiscard]] virtual int choose_select(const Context& context,
                                            std::span<const int> revealed,
                                            GameStats& stats) const = 0;

    // Which card in hand to GIVE UP to a CARD_COST, or -1 if none is legal.
    //
    // The only decision in the interface where the right answer is the LOWEST
    // score. Chrome Mox exiles a card and Mox Diamond discards one; you keep the
    // good ones. It goes through the same scorer for the same reason every other
    // choice does - one function to test and one thing to print in a trace.
    [[nodiscard]] virtual int choose_card_cost(const Context& context,
                                               std::span<const int> candidates,
                                               GameStats& stats) const = 0;

    // Which land to fetch from a set of candidates, or -1 for none.
    //
    // A fetch IS a tutor with a small candidate set, so it goes through the
    // same scorer rather than a list of its own (section 6.3). One function,
    // several call sites.
    [[nodiscard]] virtual int choose_fetch(const Context& context, std::span<const int> candidates,
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
    [[nodiscard]] int choose_land(const Context& context, GameStats& stats) const override;
    [[nodiscard]] int choose_spell(const Context& context, GameStats& stats) const override;
    [[nodiscard]] int choose_fetch(const Context& context, std::span<const int> candidates,
                                   GameStats& stats) const override;
    [[nodiscard]] int choose_tutor(const Context& context, std::span<const int> candidates,
                                   bool to_hand, GameStats& stats) const override;
    [[nodiscard]] int choose_clone(const Context& context, std::span<const int> candidates,
                                   GameStats& stats) const override;
    [[nodiscard]] int choose_card_cost(const Context& context, std::span<const int> candidates,
                                       GameStats& stats) const override;
    [[nodiscard]] int choose_select(const Context& context, std::span<const int> revealed,
                                    GameStats& stats) const override;
    [[nodiscard]] const char* name() const override {
        return "StubPolicyDoNotUseForResults (plays the lowest-indexed legal thing)";
    }
};

// The authored policy (section 6.2).
//
// ONE scoring function, used everywhere, not two mechanisms:
//
//     score(card, state) = 1000 * authored_rank[card] + situational(card, state)
//
// The "static priority list" IS the scorer with a large constant term. That
// matters for three reasons: one thing to test, one thing to print in a trace,
// and no question about which mechanism governs a given decision.
//
// THE WALL AGAINST SEARCH: situational() takes the CURRENT state and no move
// list. It never evaluates a hypothetical future position. That single rule is
// what separates this from a 1-ply search and it is enforceable by inspection.
// The weight LADDER, stated as an ordering rather than as a set of numbers.
//
// Each tier must dominate every tier below it no matter what ranks are
// authored, so the magnitudes are derived from the rank range rather than
// picked. Ranks are 0..100 and the rank term is rank*1000, so the rank term
// spans 0..100,000 and every tier below is a decade clear of it:
//
//   uncastable          -100,000,000   excludes, always
//   completes a pattern  +10,000,000   the game ends; nothing else competes
//   completes an engine   +1,000,000   the only card that matters this turn
//   land below floor        +400,000   beats any spell on rank alone
//   land above ceiling      -300,000   flooding stops without a separate rule
//   authored rank            0..100,000
//
// This ordering was NOT the first attempt. completes_engine started at 50,000
// against a rank term of up to 100,000, so a rank gap of 70 outvoted finishing
// the engine - "an enormous bonus" that a static ranking could overrule, which
// is precisely the term failing to do its job. A test caught it; reading the
// numbers did not.
struct PolicyWeights {
    // Ranks are per-card and authored; anything unlisted gets default_rank.
    std::vector<int> rank;
    int land_floor = 3;
    int land_ceiling = 6;

    int completes_pattern = 10000000;
    int completes_engine = 1000000;
    int land_below_floor = 400000;
    int land_above_ceiling = -300000;
    int uncastable = -100000000;
};

class AuthoredPolicy final : public Policy {
public:
    explicit AuthoredPolicy(PolicyWeights weights) : weights_(std::move(weights)) {}

    [[nodiscard]] int choose_land(const Context& context, GameStats& stats) const override;
    [[nodiscard]] int choose_spell(const Context& context, GameStats& stats) const override;
    [[nodiscard]] int choose_fetch(const Context& context, std::span<const int> candidates,
                                   GameStats& stats) const override;
    [[nodiscard]] int choose_tutor(const Context& context, std::span<const int> candidates,
                                   bool to_hand, GameStats& stats) const override;
    [[nodiscard]] int choose_clone(const Context& context, std::span<const int> candidates,
                                   GameStats& stats) const override;
    [[nodiscard]] int choose_card_cost(const Context& context, std::span<const int> candidates,
                                       GameStats& stats) const override;
    [[nodiscard]] int choose_select(const Context& context, std::span<const int> revealed,
                                    GameStats& stats) const override;
    [[nodiscard]] const char* name() const override {
        return "AuthoredPolicy (authored ranks plus state-dependent terms)";
    }

    // Exposed for testing: the terms are the thing worth asserting on, and a
    // test that can only see the winner cannot tell WHY it won.
    [[nodiscard]] Consideration score(const Context& context, int slot, bool as_land,
                                      GameStats& stats) const;

    // Scores a card as if it arrived in `to_hand ? hand : battlefield`. Used for
    // tutor targets, where the destination changes what the card is worth.
    [[nodiscard]] Consideration score_arrival(const Context& context, int slot, bool to_hand,
                                              GameStats& stats) const;

private:
    PolicyWeights weights_;
};

}  // namespace cs

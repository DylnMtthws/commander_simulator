#include "core/policy.hpp"

#include <algorithm>

#include "core/sim.hpp"

namespace cs {
namespace {

int lands_on_board(const Context& context) noexcept {
    int count = 0;
    context.state.battlefield.for_each([&](int slot) {
        count += context.db.cards[static_cast<std::size_t>(slot)].plays_as_land() ? 1 : 0;
    });
    return count;
}

// Would putting this card on the battlefield satisfy something that is not
// satisfied now?
//
// THE state-dependent term, and the one a fixed priority list cannot express:
// Basalt Monolith is an ordinary rock most games and the only card that matters
// on a board with Kinnan. Computed by setting the bit and re-asking, which is
// cheap because a requirement is a mask compare.
//
// Note this looks only at the CURRENT state plus this one card. It does not
// look a turn ahead, which is what keeps it a scorer rather than a search.
int pattern_completion(const Context& context, int slot, const PolicyWeights& weights,
                       bool to_hand = false) noexcept {
    GameState hypothetical = context.state;
    if (to_hand) {
        // The card ARRIVES IN HAND. It can therefore only complete a pattern
        // with an `in_hand` term, which is exactly right: a tutored card that
        // still has to be cast has not completed a board state.
        hypothetical.hand.set(slot);
    } else {
        hypothetical.hand.clear(slot);
        hypothetical.command_zone.clear(slot);
        hypothetical.battlefield.set(slot);
    }

    if (first_satisfied(context.patterns, hypothetical, context.sources) >= 0 &&
        first_satisfied(context.patterns, context.state, context.sources) < 0) {
        return weights.completes_pattern;
    }
    const FlagMask before = active_flags(context.patterns, context.state, context.sources);
    const FlagMask after = active_flags(context.patterns, hypothetical, context.sources);
    if ((after & ~before) != 0) {
        return weights.completes_engine;
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Stub
// ---------------------------------------------------------------------------

int StubPolicyDoNotUseForResults::choose_land(const Context& context, GameStats&) const {
    int chosen = -1;
    context.state.hand.for_each([&](int slot) {
        if (chosen == -1 && context.db.cards[static_cast<std::size_t>(slot)].plays_as_land()) {
            chosen = slot;
        }
    });
    return chosen;
}

int StubPolicyDoNotUseForResults::choose_spell(const Context& context, GameStats& stats) const {
    int chosen = -1;
    const Zone castable_from = context.state.hand | context.state.command_zone;
    castable_from.for_each([&](int slot) {
        if (chosen != -1) {
            return;
        }
        const Card& card = context.db.cards[static_cast<std::size_t>(slot)];
        const Face* face = card.castable_face();
        if (face == nullptr) {
            return;
        }
        ++stats.can_pay_calls;
        if (can_pay(*face->cost, context.sources, 0)) {
            chosen = slot;
        }
    });
    return chosen;
}

int StubPolicyDoNotUseForResults::choose_fetch(const Context&, std::span<const int> candidates,
                                               GameStats&) const {
    return candidates.empty() ? -1 : candidates.front();
}

int StubPolicyDoNotUseForResults::choose_tutor(const Context&, std::span<const int> candidates,
                                               bool, GameStats&) const {
    return candidates.empty() ? -1 : candidates.front();
}

int StubPolicyDoNotUseForResults::choose_clone(const Context&, std::span<const int> candidates,
                                               GameStats&) const {
    return candidates.empty() ? -1 : candidates.front();
}

int StubPolicyDoNotUseForResults::choose_card_cost(const Context&,
                                                   std::span<const int> candidates,
                                                   GameStats&) const {
    return candidates.empty() ? -1 : candidates.front();
}

int StubPolicyDoNotUseForResults::choose_select(const Context&, std::span<const int> revealed,
                                                GameStats&) const {
    return revealed.empty() ? -1 : revealed.front();
}

// ---------------------------------------------------------------------------
// Authored
// ---------------------------------------------------------------------------

Consideration AuthoredPolicy::score(const Context& context, int slot, bool as_land,
                                    GameStats& stats) const {
    const Card& card = context.db.cards[static_cast<std::size_t>(slot)];
    Consideration result;
    result.slot = slot;

    const auto index = static_cast<std::size_t>(slot);
    const int rank = index < weights_.rank.size() ? weights_.rank[index] : 0;
    result.rank_term = rank * 1000;

    if (as_land) {
        const int lands = lands_on_board(context);
        result.castable = true;
        if (lands < weights_.land_floor) {
            result.land_term = weights_.land_below_floor;
        } else if (lands >= weights_.land_ceiling) {
            result.land_term = weights_.land_above_ceiling;
        }
        result.pattern_term = pattern_completion(context, slot, weights_);
    } else {
        const Face* face = card.castable_face();
        if (face != nullptr) {
            // CONVOKE changes what may pay for THIS card and nothing else, so
            // the extra sources are built here and thrown away. Merging them
            // into collect_sources would let every other spell in hand spend a
            // creature that only Chord of Calling can tap.
            std::vector<Source> with_convoke;
            std::span<const Source> payable = context.sources;
            if (context.effects != nullptr &&
                context.effects->by_slot[index].convoke) {
                with_convoke.assign(context.sources.begin(), context.sources.end());
                convoke_sources(context.db, *context.effects, context.state, with_convoke);
                payable = with_convoke;
            }
            ++stats.can_pay_calls;
            result.castable = can_pay(*face->cost, payable, 0);
        }
        if (result.castable && context.effects != nullptr) {
            // A CLONE with nothing legal to copy is DEAD, not merely mediocre.
            // Scoring it zero would leave it mid-table on an empty board and
            // the policy would cast it for nothing; the uncastable term is what
            // says "this card does not function right now".
            const CardEffects& entry =
                context.effects->by_slot[static_cast<std::size_t>(slot)];
            if (entry.has_clone) {
                std::vector<int> targets;
                clone_candidates(entry.clone, context.db, context.state,
                                 total_mana(context.sources), targets);
                if (targets.empty()) {
                    result.castable = false;
                }
            }
            // Same shape for a CARD_COST. Chrome Mox with no nonland card left
            // in hand imprints nothing and taps for nothing; Mox Diamond with no
            // land to discard is sacrificed. Both are DEAD rather than merely
            // mediocre, so they read as uncastable and not as zero.
            if (entry.has_card_cost) {
                std::vector<int> payable;
                card_cost_candidates(entry.card_cost, context.db, context.state, payable);
                // The card being scored is itself in hand and cannot pay for
                // itself, so it does not count towards the requirement.
                int usable = 0;
                for (const int candidate : payable) {
                    usable += candidate == slot ? 0 : 1;
                }
                if (usable < entry.card_cost.cards) {
                    result.castable = false;
                }
            }
        }
        if (!result.castable) {
            result.castable_term = weights_.uncastable;
        } else {
            result.pattern_term = pattern_completion(context, slot, weights_);
        }
    }
    result.score = result.rank_term + result.pattern_term + result.land_term + result.castable_term;
    return result;
}

namespace {

// Picks the highest score, and resolves ties EXPLICITLY.
//
// Section 6.5: a tie broken by iteration order is a reproducibility bug that
// looks like variance. Zone::for_each already ascends, but relying on that
// leaves the rule implicit and one refactor from being wrong. So the tiebreak
// is stated: score descending, then export_index ascending. export_index is
// dense, unique and stable, so no tie can survive it and the comparison is a
// total order.
int best_of(std::span<const Consideration> candidates, int floor_score) {
    int best = -1;
    int best_score = floor_score;
    int best_index = 0;
    for (const Consideration& candidate : candidates) {
        const bool better = candidate.score > best_score ||
                            (candidate.score == best_score && best >= 0 &&
                             candidate.slot < best_index);
        if (best < 0 ? candidate.score > best_score : better) {
            best = candidate.slot;
            best_score = candidate.score;
            best_index = candidate.slot;
        }
    }
    return best;
}

}  // namespace

Consideration AuthoredPolicy::score_arrival(const Context& context, int slot, bool to_hand,
                                            GameStats& stats) const {
    Consideration result;
    result.slot = slot;
    const auto index = static_cast<std::size_t>(slot);
    result.rank_term = (index < weights_.rank.size() ? weights_.rank[index] : 0) * 1000;
    result.castable = true;  // it is being fetched, not cast
    result.pattern_term = pattern_completion(context, slot, weights_, to_hand);
    static_cast<void>(stats);
    result.score = result.rank_term + result.pattern_term;
    return result;
}

int AuthoredPolicy::choose_tutor(const Context& context, std::span<const int> candidates,
                                 bool to_hand, GameStats& stats) const {
    // §6.3's claim under test: tutors and selection are the same problem, so
    // this is the SAME scorer with a different candidate set - the library
    // rather than the hand. The set is larger and the cards are unseen, but the
    // decision is identical in kind.
    std::vector<Consideration> scored;
    scored.reserve(candidates.size());
    for (const int slot : candidates) {
        scored.push_back(score_arrival(context, slot, to_hand, stats));
    }
    if (context.observer != nullptr) {
        context.observer->considering(scored, to_hand ? "tutor target (to hand)"
                                                      : "tutor target (to battlefield)");
    }
    int best = -1;
    int best_score = 0;
    for (const Consideration& candidate : scored) {
        if (best < 0 || candidate.score > best_score ||
            (candidate.score == best_score && candidate.slot < best)) {
            best = candidate.slot;
            best_score = candidate.score;
        }
    }
    return best;
}

int AuthoredPolicy::choose_clone(const Context& context, std::span<const int> candidates,
                                 GameStats& stats) const {
    // The same scorer again, with the BATTLEFIELD as its candidate set. No new
    // term was needed - a clone's value is "what is the best permanent to
    // copy", and "best permanent" is what the scorer already computes.
    std::vector<Consideration> scored;
    scored.reserve(candidates.size());
    for (const int slot : candidates) {
        scored.push_back(score_arrival(context, slot, /*to_hand=*/false, stats));
    }
    if (context.observer != nullptr) {
        context.observer->considering(scored, "clone target");
    }
    int best = -1;
    int best_score = 0;
    for (const Consideration& candidate : scored) {
        if (best < 0 || candidate.score > best_score ||
            (candidate.score == best_score && candidate.slot < best)) {
            best = candidate.slot;
            best_score = candidate.score;
        }
    }
    return best;
}

int AuthoredPolicy::choose_fetch(const Context& context, std::span<const int> candidates,
                                 GameStats& stats) const {
    // Same scorer, different candidate set. A fetch's candidates come from the
    // library rather than the hand, and that is the only difference.
    std::vector<Consideration> scored;
    scored.reserve(candidates.size());
    for (const int slot : candidates) {
        scored.push_back(score(context, slot, /*as_land=*/true, stats));
    }
    if (context.observer != nullptr) {
        context.observer->considering(scored, "fetch target");
    }
    // No floor: having fetched, taking SOMETHING always beats taking nothing,
    // even above the land ceiling - the fetch is already sacrificed.
    int best = -1;
    int best_score = 0;
    for (const Consideration& candidate : scored) {
        if (best < 0 || candidate.score > best_score ||
            (candidate.score == best_score && candidate.slot < best)) {
            best = candidate.slot;
            best_score = candidate.score;
        }
    }
    return best;
}

int AuthoredPolicy::choose_land(const Context& context, GameStats& stats) const {
    std::vector<Consideration> candidates;
    context.state.hand.for_each([&](int slot) {
        if (context.db.cards[static_cast<std::size_t>(slot)].plays_as_land()) {
            candidates.push_back(score(context, slot, /*as_land=*/true, stats));
        }
    });
    if (context.observer != nullptr) {
        context.observer->considering(candidates, "land drop");
    }
    // A land is only played if it scores above zero. Above the ceiling its term
    // is strongly negative, so flooding stops without a separate rule.
    return best_of(candidates, 0);
}

int AuthoredPolicy::choose_spell(const Context& context, GameStats& stats) const {
    std::vector<Consideration> candidates;
    const Zone castable_from = context.state.hand | context.state.command_zone;
    castable_from.for_each([&](int slot) {
        if (context.db.cards[static_cast<std::size_t>(slot)].castable_face() != nullptr) {
            candidates.push_back(score(context, slot, /*as_land=*/false, stats));
        }
    });
    if (context.observer != nullptr) {
        context.observer->considering(candidates, "cast");
    }
    // Floor of 0: an uncastable card carries a large negative term, so it can
    // never be chosen, and there is no second code path deciding castability.
    return best_of(candidates, 0);
}

int AuthoredPolicy::choose_card_cost(const Context& context, std::span<const int> candidates,
                                     GameStats& stats) const {
    // The same scorer, and the ONLY call site that takes the minimum. A card
    // being given up should be the least valuable one available, and
    // score_arrival's pattern term is what stops the deck exiling the card that
    // would have completed a line - it carries +10,000,000, so it is never the
    // minimum.
    std::vector<Consideration> scored;
    scored.reserve(candidates.size());
    for (const int slot : candidates) {
        scored.push_back(score_arrival(context, slot, /*to_hand=*/true, stats));
    }
    if (context.observer != nullptr) {
        context.observer->considering(scored, "card to give up");
    }
    int worst = -1;
    int worst_score = 0;
    for (const Consideration& candidate : scored) {
        if (worst < 0 || candidate.score < worst_score ||
            (candidate.score == worst_score && candidate.slot < worst)) {
            worst = candidate.slot;
            worst_score = candidate.score;
        }
    }
    return worst;
}

int AuthoredPolicy::choose_select(const Context& context, std::span<const int> revealed,
                                  GameStats& stats) const {
    // choose_tutor's body over five cards instead of the library. The candidate
    // set is smaller and the cards are random rather than chosen, and that is the
    // ONLY difference - which is why R3 needed no new scorer term (§16.7b).
    std::vector<Consideration> scored;
    scored.reserve(revealed.size());
    for (const int slot : revealed) {
        scored.push_back(score_arrival(context, slot, /*to_hand=*/false, stats));
    }
    if (context.observer != nullptr) {
        context.observer->considering(scored, "dig: keep which of the five");
    }
    int best = -1;
    int best_score = 0;
    for (const Consideration& candidate : scored) {
        if (best < 0 || candidate.score > best_score ||
            (candidate.score == best_score && candidate.slot < best)) {
            best = candidate.slot;
            best_score = candidate.score;
        }
    }
    return best;
}

}  // namespace cs

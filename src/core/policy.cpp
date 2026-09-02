#include "core/policy.hpp"

#include <algorithm>

#include "core/sim.hpp"

namespace cs {
namespace {

const Face* castable_face_of(const Card& card) noexcept {
    for (const Face& face : card.faces) {
        if (face.is_castable()) {
            return &face;
        }
    }
    return nullptr;
}

bool card_has_land_face(const Card& card) noexcept {
    for (const Face& face : card.faces) {
        if (face.is_land) {
            return true;
        }
    }
    return false;
}

int lands_on_board(const Context& context) noexcept {
    int count = 0;
    context.state.battlefield.for_each([&](int slot) {
        count += card_has_land_face(context.db.cards[static_cast<std::size_t>(slot)]) ? 1 : 0;
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
int pattern_completion(const Context& context, int slot, const PolicyWeights& weights) noexcept {
    GameState hypothetical = context.state;
    hypothetical.hand.clear(slot);
    hypothetical.command_zone.clear(slot);
    hypothetical.battlefield.set(slot);

    if (first_satisfied(context.patterns, hypothetical) >= 0 &&
        first_satisfied(context.patterns, context.state) < 0) {
        return weights.completes_pattern;
    }
    const FlagMask before = active_flags(context.patterns, context.state);
    const FlagMask after = active_flags(context.patterns, hypothetical);
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
        if (chosen == -1 && card_has_land_face(context.db.cards[static_cast<std::size_t>(slot)])) {
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
        const Face* face = castable_face_of(card);
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
        const Face* face = castable_face_of(card);
        if (face != nullptr) {
            ++stats.can_pay_calls;
            result.castable = can_pay(*face->cost, context.sources, 0);
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
        if (card_has_land_face(context.db.cards[static_cast<std::size_t>(slot)])) {
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
        if (castable_face_of(context.db.cards[static_cast<std::size_t>(slot)]) != nullptr) {
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

}  // namespace cs

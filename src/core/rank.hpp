#pragma once

#include <span>
#include <vector>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "core/pattern.hpp"
#include "core/policy.hpp"

namespace cs {

struct RankOverride {
    int slot = -1;
    int rank = 0;
};

// A deck-independent role ranking. It reads only properties already compiled
// into core values; no card names, deck IDs, or I/O-layer data are permitted.
[[nodiscard]] int derive_card_rank(const Card& card, const CardEffects& effects,
                                   const PatternSet& patterns) noexcept;

void derive_policy_ranks(const CardDb& db, const EffectDb& effects,
                         const PatternSet& patterns, std::span<const RankOverride> overrides,
                         PolicyWeights& weights);

}  // namespace cs

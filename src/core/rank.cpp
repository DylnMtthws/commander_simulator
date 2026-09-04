#include "core/rank.hpp"

#include <algorithm>
#include <bit>
#include <string_view>

namespace cs {
namespace {

bool names(const Requirement& requirement, int slot) noexcept {
    return requirement.in_play.test(slot) || requirement.in_hand.test(slot) ||
           requirement.in_play_or_hand.test(slot) || requirement.untapped.test(slot) ||
           requirement.any_of.test(slot) || requirement.resolved.test(slot) ||
           requirement.in_graveyard.test(slot) ||
           requirement.imprinted_permanent == slot || requirement.imprinted_card == slot;
}

bool type_contains(const Card& card, std::string_view type) noexcept {
    for (const Face& face : card.faces) {
        if (face.type_line.find(type) != std::string::npos) return true;
    }
    return false;
}

int land_rank(const Card& card, const CardEffects& effects) noexcept {
    if (effects.has_fetch) {
        return 32 + std::min(5, static_cast<int>(effects.fetch.finds.size()) * 2);
    }
    if (!effects.has_mana_source) return 10;
    const ManaSourceEffect& source = effects.mana_source;
    if (card.has_land_face) return 16;
    if (source.colours_from_table) return 36;
    switch (source.enters_tapped_unless) {
        case EntersTappedUnless::OpponentCount: return 38;
        case EntersTappedUnless::LandCount: return 32;
        case EntersTappedUnless::PayLife: return 35;
        case EntersTappedUnless::Never: break;
    }
    const int colours = std::popcount(static_cast<unsigned int>(source.produces));
    if (source.amount >= 2) return 26;
    if (colours >= 5) return source.life_cost > 0 ? 28 : 39;
    if (colours >= 2) return source.life_cost > 0 ? 30 : 40;
    if (type_contains(card, "Basic")) {
        return 22 + ((source.produces & colour_bit(Colour::Blue)) != 0 ? 1 : 0);
    }
    if (type_contains(card, "Legendary")) {
        return 17 + ((source.produces & colour_bit(Colour::Blue)) != 0 ? 1 : 0);
    }
    return 20;
}

}  // namespace

int derive_card_rank(const Card& card, const CardEffects& effects,
                     const PatternSet& patterns) noexcept {
    if (card.is_commander) return 100;
    if (card.plays_as_land()) return land_rank(card, effects);

    int rank = 10;
    if (effects.has_mana_source) {
        const ManaSourceEffect& source = effects.mana_source;
        if (source.is_creature) {
            rank = std::popcount(static_cast<unsigned int>(source.produces)) > 1 ? 60 : 55;
            if (source.condition != SourceCondition::None) rank -= 10;
        } else if (source.condition != SourceCondition::None) {
            rank = card.mana_value == 0 ? 45 : 40;
        } else if (effects.has_card_cost) {
            rank = 50;
        } else if (source.amount >= 2) {
            rank = source.untaps_normally ? 70 : 65;
        } else {
            rank = 40;
        }
    }
    if (effects.has_ritual) rank = std::max(rank, 55);
    if (effects.has_tutor) {
        rank = std::max(rank, effects.tutor.max_from_x ? 60 :
                              (effects.tutor.mana_value_exactly >= 0 ? 45 : 58));
    }
    if (effects.has_draw || effects.has_select) rank = std::max(rank, 45);
    if (effects.has_mass_untap || effects.has_untap_target) rank = std::max(rank, 48);

    int engine_membership = 0;
    int win_membership = 0;
    for (const Engine& engine : patterns.engines) {
        engine_membership += names(engine.requires_, card.export_index) ? 1 : 0;
    }
    for (const WinPattern& pattern : patterns.patterns) {
        win_membership += names(pattern.requires_, card.export_index) ? 1 : 0;
    }
    if (engine_membership > 0) {
        rank = std::max(rank, 80 + (effects.has_mana_source ? 10 : 0));
    }
    if (win_membership > 0) rank = std::max(rank, 88);
    return std::clamp(rank, 0, 100);
}

void derive_policy_ranks(const CardDb& db, const EffectDb& effects,
                         const PatternSet& patterns, std::span<const RankOverride> overrides,
                         PolicyWeights& weights) {
    weights.rank.resize(db.cards.size());
    for (const Card& card : db.cards) {
        weights.rank[static_cast<std::size_t>(card.export_index)] =
            derive_card_rank(card, effects.by_slot[static_cast<std::size_t>(card.export_index)],
                             patterns);
    }
    for (const RankOverride& override : overrides) {
        weights.rank[static_cast<std::size_t>(override.slot)] = override.rank;
    }
}

}  // namespace cs

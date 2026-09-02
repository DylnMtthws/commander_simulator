#include "core/effects.hpp"

namespace cs {

bool enters_tapped(const ManaSourceEffect& effect, const CardDb& db, const EffectDb& effects,
                   const GameState& state, const TableContext& table) noexcept {
    switch (effect.enters_tapped_unless) {
        case EntersTappedUnless::Never:
            return false;
        case EntersTappedUnless::LandCount: {
            int lands = 0;
            state.battlefield.for_each([&](int slot) {
                const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
                lands += (entry.has_mana_source && entry.mana_source.is_land) ? 1 : 0;
            });
            static_cast<void>(db);
            return lands > effect.enters_tapped_param;
        }
        case EntersTappedUnless::PayLife:
            // Life is not tracked, so the payment always succeeds and the land
            // always enters untapped. Overstates; see effects.toml.
            return false;
        case EntersTappedUnless::OpponentCount:
            return table.opponents < effect.enters_tapped_param;
    }
    return false;
}

void collect_sources(const CardDb& db, const EffectDb& effects, const GameState& state,
                     const TableContext& table, std::vector<Source>& out) {
    out.clear();

    // Modifiers first: they change what every other source produces, so they
    // have to be known before any source is read. This is the dependency order
    // the authoring followed, expressed in code.
    bool multiply = false;
    ModifierEffect multiplier;
    ColourMask granted_to_creatures = 0;
    std::uint8_t granted_amount = 0;

    state.battlefield.for_each([&](int slot) {
        const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
        if (!entry.has_modifier) {
            return;
        }
        if (entry.modifier.mode == ModifierMode::Multiply) {
            multiply = true;
            multiplier = entry.modifier;
        } else {
            granted_to_creatures |= entry.modifier.grants;
            granted_amount = entry.modifier.grant_amount;
        }
    });

    state.battlefield.for_each([&](int slot) {
        if (state.tapped.test(slot)) {
            return;
        }
        const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
        const Card& card = db.cards[static_cast<std::size_t>(slot)];

        Source source;
        bool have = false;

        if (entry.has_mana_source) {
            const ManaSourceEffect& mana = entry.mana_source;
            source.produces = mana.colours_from_table ? table.opponent_colors : mana.produces;
            source.amount = mana.amount;
            source.is_land = mana.is_land;
            source.is_creature = mana.is_creature;
            have = true;
        }

        // Enduring Vitality grants every creature a mana ability. A creature
        // that is ALREADY a source keeps its own - the grant does not replace
        // Birds of Paradise's ability, and stacking both would double-count.
        if (!have && granted_to_creatures != 0) {
            bool is_creature = false;
            for (const std::string& type : card.all_types) {
                is_creature = is_creature || type == "Creature";
            }
            if (is_creature) {
                source.produces = granted_to_creatures;
                source.amount = granted_amount;
                source.is_creature = true;
                have = true;
            }
        }

        if (!have || source.amount == 0) {
            return;
        }
        // A source producing nothing is not a source. Exotic Orchard with
        // opponent_colors = [] lands here, which is the point of section 2.8's
        // "produces nothing" case being real rather than hypothetical.
        if (source.produces == 0 && source.is_land && entry.mana_source.colours_from_table) {
            return;
        }
        if (multiply) {
            source = with_multiplier(source, ManaMultiplier{.bonus = multiplier.bonus,
                                                            .nonland_only = multiplier.nonland_only});
        }
        out.push_back(source);
    });
}

}  // namespace cs

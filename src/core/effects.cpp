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
            // Pay if it leaves us above the floor, otherwise take it tapped.
            return state.life - effect.enters_tapped_param < table.life_floor;
        case EntersTappedUnless::OpponentCount:
            return table.opponents < effect.enters_tapped_param;
    }
    return false;
}

bool condition_met(const ManaSourceEffect& effect, const CardDb& db, const EffectDb& effects,
                   const GameState& state) noexcept {
    if (effect.condition == SourceCondition::None) {
        return true;
    }
    int count = 0;
    state.battlefield.for_each([&](int slot) {
        const Card& card = db.cards[static_cast<std::size_t>(slot)];
        const bool tapped = state.tapped.test(slot);
        bool creature = false;
        bool artifact = false;
        bool legendary = false;
        for (const std::string& type : card.all_types) {
            creature = creature || type == "Creature";
            artifact = artifact || type == "Artifact";
            legendary = legendary || type == "Legendary";
        }
        switch (effect.condition) {
            case SourceCondition::None:
                break;
            case SourceCondition::ArtifactCountGte:
                count += artifact ? 1 : 0;
                break;
            case SourceCondition::LegendaryCreatureCountGte:
                count += (legendary && creature) ? 1 : 0;
                break;
            case SourceCondition::UntappedCreatureGte:
                count += (creature && !tapped) ? 1 : 0;
                break;
            case SourceCondition::UntappedPermanentGte:
                count += tapped ? 0 : 1;
                break;
        }
    });
    static_cast<void>(effects);
    return count >= effect.condition_param;
}

void fetch_candidates(const FetchEffect& fetch, const CardDb& db, const GameState& state,
                      std::vector<int>& out) {
    out.clear();
    // The UNDRAWN part of the library only. Searching the drawn prefix would
    // fetch a card that is already in hand or on the battlefield.
    for (std::size_t i = state.drawn; i < state.library_count; ++i) {
        const int slot = state.library[i];
        const Card& card = db.cards[static_cast<std::size_t>(slot)];
        bool is_land = false;
        bool matches = false;
        for (const std::string& type : card.all_types) {
            is_land = is_land || type == "Land";
            for (const std::string& wanted : fetch.finds) {
                matches = matches || type == wanted;
            }
        }
        if (is_land && matches) {
            out.push_back(slot);
        }
    }
}

void tutor_candidates(const TutorEffect& tutor, const CardDb& db, const GameState& state,
                      int mana_available, std::vector<int>& out) {
    out.clear();
    for (std::size_t i = state.drawn; i < state.library_count; ++i) {
        const int slot = state.library[i];
        const Card& card = db.cards[static_cast<std::size_t>(slot)];

        bool creature = false;
        bool human = false;
        bool artifact = false;
        bool land = false;
        for (const std::string& type : card.all_types) {
            creature = creature || type == "Creature";
            human = human || type == "Human";
            artifact = artifact || type == "Artifact";
            land = land || type == "Land";
        }
        bool matches = false;
        switch (tutor.filter) {
            case TutorFilter::Any: matches = true; break;
            case TutorFilter::Creature: matches = creature; break;
            case TutorFilter::NonHumanCreature: matches = creature && !human; break;
            case TutorFilter::Artifact: matches = artifact; break;
            case TutorFilter::Land: matches = land; break;
        }
        if (!matches) {
            continue;
        }
        // The mana-value cap. For an X tutor the cap is what the mana can pay
        // for, which is why this takes mana_available rather than reading a
        // constant - Finale of Devastation for X=0 finds a creature with mana
        // value 0, and there are none (SIM_PLAN.md section 2.2).
        const int cap = tutor.max_from_x ? mana_available : tutor.max_mana_value;
        if (cap >= 0 && card.mana_value > cap) {
            continue;
        }
        out.push_back(slot);
    }
}

void clone_candidates(const CloneEffect& clone, const CardDb& db, const GameState& state,
                      int mana_available, std::vector<int>& out) {
    out.clear();
    state.battlefield.for_each([&](int slot) {
        // Copy what the target IS, so cloning a clone copies the original.
        const int effective = state.effective(slot);
        const Card& card = db.cards[static_cast<std::size_t>(effective)];
        bool creature = false;
        bool artifact = false;
        bool enchantment = false;
        bool land = false;
        for (const std::string& type : card.all_types) {
            creature = creature || type == "Creature";
            artifact = artifact || type == "Artifact";
            enchantment = enchantment || type == "Enchantment";
            land = land || type == "Land";
        }
        bool matches = false;
        switch (clone.filter) {
            case CloneFilter::NonlandPermanent: matches = !land; break;
            case CloneFilter::Artifact: matches = artifact; break;
            case CloneFilter::Enchantment: matches = enchantment; break;
            case CloneFilter::Creature: matches = creature; break;
            case CloneFilter::ArtifactOrEnchantment: matches = artifact || enchantment; break;
        }
        if (matches && clone.max_from_x && card.mana_value > mana_available) {
            matches = false;
        }
        if (matches) {
            out.push_back(effective);
        }
    });
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

    // Rituals in HAND. Elvish Spirit Guide exiles from hand for {G}, which is
    // the verb the loop would otherwise lack and the reason RITUAL is a kind.
    state.hand.for_each([&](int slot) {
        const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
        if (entry.has_ritual && entry.ritual.from_zone == RitualZone::Hand) {
            out.push_back(Source{.produces = entry.ritual.produces,
                                 .amount = entry.ritual.amount,
                                 .is_land = false,
                                 .is_creature = false});
        }
    });

    state.battlefield.for_each([&](int slot) {
        if (state.tapped.test(slot)) {
            return;
        }
        // A clone taps as the card it copied - and because it is a separate
        // permanent, Kinnan multiplies it independently. That falls out of
        // resolving the effect here rather than needing a special case, but it
        // is verified rather than assumed (tests/unit/test_clone.cpp).
        const int effective = state.effective(slot);
        const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(effective)];
        const Card& card = db.cards[static_cast<std::size_t>(effective)];

        Source source;
        bool have = false;

        if (entry.has_ritual && entry.ritual.from_zone == RitualZone::Battlefield) {
            // Lotus Petal sacrifices itself, so it is a source exactly once. The
            // turn loop removes it when tapped.
            out.push_back(Source{.produces = entry.ritual.produces,
                                 .amount = entry.ritual.amount,
                                 .is_land = false,
                                 .is_creature = false});
            return;
        }
        if (entry.has_mana_source) {
            const ManaSourceEffect& mana = entry.mana_source;
            // A source we cannot afford to use is not a source. This is the
            // whole of life's interaction with payment: can_pay never learns
            // about life, it just sees a shorter list.
            if (mana.life_cost > 0 && state.life - mana.life_cost < table.life_floor) {
                return;
            }
            if (!condition_met(mana, db, effects, state)) {
                return;
            }
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

int life_cost_of_tapping(const EffectDb& effects, const GameState& state, int slot) noexcept {
    static_cast<void>(state);
    const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
    return entry.has_mana_source ? entry.mana_source.life_cost : 0;
}

}  // namespace cs

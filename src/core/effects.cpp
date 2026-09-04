#include "core/effects.hpp"

#include <string_view>

namespace cs {

const char* reason_category_meaning(std::string_view category) noexcept {
    // The closed enumeration of section 4.4. A category outside it is refused
    // at load: the categories are the whole reason the inert set has a shape
    // rather than a size, and one free-text outlier makes the grouped table a
    // list again.
    if (category == "interaction") {
        return "counters or removal with nothing to answer";
    }
    if (category == "opponent_trigger") {
        return "fires only when an opponent acts";
    }
    if (category == "opponent_permanent") {
        return "targets, copies or steals an opponent's permanent";
    }
    if (category == "timing_only") {
        return "alters timing; no stack and no priority here";
    }
    if (category == "no_object_in_model") {
        return "needs combat, the stack, or a meaningful graveyard";
    }
    return nullptr;
}

bool enters_tapped(const ManaSourceEffect& effect, const CardDb& db, const EffectDb& effects,
                   const GameState& state, const TableContext& table, int self) noexcept {
    switch (effect.enters_tapped_unless) {
        case EntersTappedUnless::Never:
            return false;
        case EntersTappedUnless::LandCount: {
            int lands = 0;
            state.battlefield.for_each([&](int slot) {
                if (slot == self) {
                    return;  // "two or fewer OTHER lands"
                }
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
                   const GameState& state, int self) noexcept {
    if (effect.condition == SourceCondition::None) {
        return true;
    }
    int count = 0;
    state.battlefield.for_each([&](int slot) {
        // The permanent being asked about never counts toward its own condition:
        // Mox Opal's metalcraft is the one case where the card DOES count itself
        // ("you control three or more artifacts"), and it is handled by counting
        // it back in below rather than by making the loop card-specific.
        if (slot == self && effect.condition == SourceCondition::UntappedPermanentGte) {
            return;
        }
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

        // The FRONT FACE, because this is a search of the LIBRARY, and a card
        // in the library has only its front face's characteristics. all_types
        // is a union over faces, and using it here let every creature tutor in
        // the deck find INVASION OF IKORIA - a Battle card whose back face is
        // Zilortha, a Legendary Creature. You cannot Chord of Calling for a
        // Battle. It was being offered at rank 58, above three of the mana
        // dorks, so it was not a target the scorer merely tolerated.
        //
        // Third instance of one shape in one sitting: a card-level field that
        // aggregates faces answers a question about no particular face, and
        // never errors. See is_permanent above, and the ingestion PLAN.md 11.0
        // rule about oracle_text.
        //
        // The battlefield-facing predicates below and in collect_sources still
        // read all_types, and that is correct there rather than an oversight:
        // for every card in this deck that reaches the battlefield, all_types
        // is a superset that agrees. Sink into Stupor is only ever there as its
        // land face and all_types contains Land; Invasion of Ikoria never
        // reaches the battlefield at all, because a Battle is not a permanent
        // this model keeps (section 4.6).
        const std::string& line = card.faces.empty() ? card.name : card.faces.front().type_line;
        const bool creature = line.find("Creature") != std::string::npos;
        const bool human = line.find("Human") != std::string::npos;
        const bool artifact = line.find("Artifact") != std::string::npos;
        const bool land = line.find("Land") != std::string::npos;
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
        // TWO DIFFERENT RESTRICTIONS, and conflating them was a real bug.
        //
        // An X tutor is a genuine MAXIMUM, and the cap is what the mana can pay
        // for rather than a constant - Finale of Devastation at X=0 finds a
        // creature of mana value 0, and there are none (section 2.2).
        //
        // A constant-cap tutor in this deck is EXACT. Trophy Mage searches for
        // "an artifact card with mana value 3", and transmute searches for "the
        // same mana value as this card". Neither says "or less".
        if (tutor.max_from_x) {
            if (card.mana_value > mana_available) {
                continue;
            }
        } else if (tutor.mana_value_exactly >= 0 &&
                   card.mana_value != tutor.mana_value_exactly) {
            continue;
        }
        out.push_back(slot);
    }
}

void select_candidates(const SelectEffect& select, const CardDb& db,
                       std::span<const int> revealed, std::vector<int>& out) {
    out.clear();
    for (const int slot : revealed) {
        const Card& card = db.cards[static_cast<std::size_t>(slot)];
        // The FRONT FACE, for the same reason tutor_candidates reads it: these
        // cards are in the library, and a card in the library has only its front
        // face's characteristics.
        const std::string& line = card.faces.empty() ? card.name : card.faces.front().type_line;
        const bool creature = line.find("Creature") != std::string::npos;
        const bool human = line.find("Human") != std::string::npos;
        const bool artifact = line.find("Artifact") != std::string::npos;
        const bool land = line.find("Land") != std::string::npos;
        bool matches = false;
        switch (select.filter) {
            case TutorFilter::Any: matches = true; break;
            case TutorFilter::Creature: matches = creature; break;
            case TutorFilter::NonHumanCreature: matches = creature && !human; break;
            case TutorFilter::Artifact: matches = artifact; break;
            case TutorFilter::Land: matches = land; break;
        }
        if (matches) {
            out.push_back(slot);
        }
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

bool is_permanent(const Card& card) noexcept {
    // The FRONT FACE's type line, not all_types.
    //
    // all_types is a union over every face, and reading it here got the answer
    // exactly backwards on the one card it matters for: Invasion of Ikoria is a
    // Battle whose back face is Zilortha, a Legendary Creature, so the card-level
    // list contains "Creature" and the Siege would have stayed on the
    // battlefield - against section 4.6, which decided the permanent is
    // discarded because a Siege flips by being attacked and there is no combat.
    //
    // Same shape as the ingestion PLAN.md 11.0 rule about oracle_text: a
    // card-level field that aggregates faces answers a question about no
    // particular face, and does it without an error. A test caught this one.
    if (card.faces.empty()) {
        return false;
    }
    const std::string& line = card.faces.front().type_line;
    static constexpr std::string_view kPermanent[] = {"Artifact", "Creature", "Enchantment",
                                                      "Land", "Planeswalker"};
    for (const std::string_view type : kPermanent) {
        if (line.find(type) != std::string::npos) {
            return true;
        }
    }
    // Battle is absent on purpose - section 4.6, and the comment above.
    return false;
}

void convoke_slots(const CardDb& db, const EffectDb& effects, const GameState& state,
                   std::vector<int>& out) {
    out.clear();
    state.battlefield.for_each([&](int slot) {
        if (state.tapped.test(slot)) {
            return;
        }
        const int effective = state.effective(slot);
        const Card& card = db.cards[static_cast<std::size_t>(effective)];
        bool creature = false;
        for (const std::string& type : card.all_types) {
            creature = creature || type == "Creature";
        }
        if (!creature) {
            return;
        }
        // A creature that taps for mana is worth more tapped for mana. Skipping
        // it here is what makes convoke additive rather than a choice the policy
        // would have to make - see the header for why that is exact.
        if (effects.by_slot[static_cast<std::size_t>(effective)].has_mana_source) {
            return;
        }
        out.push_back(slot);
    });
}

void convoke_sources(const CardDb& db, const EffectDb& effects, const GameState& state,
                     std::vector<Source>& out) {
    std::vector<int> slots;
    convoke_slots(db, effects, state, slots);
    for (const int slot : slots) {
        const Card& card = db.cards[static_cast<std::size_t>(state.effective(slot))];
        out.push_back(Source{.produces = card.colour_identity,
                             .amount = 1,
                             .is_land = false,
                             .is_creature = true});
    }
}

void card_cost_candidates(const CardCostEffect& cost, const CardDb& db, const GameState& state,
                          std::vector<int>& out) {
    out.clear();
    state.hand.for_each([&](int slot) {
        const Card& card = db.cards[static_cast<std::size_t>(slot)];
        const bool land = card.plays_as_land();
        switch (cost.filter) {
            case CardFilter::Any: break;
            case CardFilter::Land:
                if (!land) return;
                break;
            case CardFilter::Nonland:
                if (land) return;
                break;
            case CardFilter::Instant: {
                const std::string& line =
                    card.faces.empty() ? card.name : card.faces.front().type_line;
                if (line.find("Instant") == std::string::npos) return;
                break;
            }
        }
        if (cost.mana_value_lte >= 0 && card.mana_value > cost.mana_value_lte) {
            return;
        }
        out.push_back(slot);
    });
}

int escape_exile_cost(const CardDb& db, const EffectDb& effects, const GameState& state,
                       int slot) noexcept {
    if (!state.graveyard.test(slot) || db.cards[static_cast<std::size_t>(slot)].plays_as_land()) {
        return -1;
    }
    int cost = -1;
    state.battlefield.for_each([&](int permanent) {
        const CardEffects& entry =
            effects.by_slot[static_cast<std::size_t>(state.effective(permanent))];
        if (entry.has_escape && (cost < 0 || entry.escape.exile_cards < cost)) {
            cost = entry.escape.exile_cards;
        }
    });
    return cost;
}

void escape_cost_candidates(const GameState& state, int escaping, std::vector<int>& out) {
    out.clear();
    state.graveyard.for_each([&](int slot) {
        if (slot != escaping) {
            out.push_back(slot);
        }
    });
}

void untap_target_candidates(const UntapTargetEffect& effect, const CardDb& db,
                             const GameState& state, std::vector<int>& out) {
    out.clear();
    state.tapped.for_each([&](int slot) {
        if (!state.battlefield.test(slot)) {
            return;
        }
        if (effect.nonland_only &&
            db.cards[static_cast<std::size_t>(state.effective(slot))].plays_as_land()) {
            return;
        }
        out.push_back(slot);
    });
}

void enter_battlefield(const CardDb& db, const EffectDb& effects, GameState& state,
                       const TableContext& table, int slot) {
    state.battlefield.set(slot);
    const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
    if (!entry.has_mana_source) {
        return;
    }
    if (enters_tapped(entry.mana_source, db, effects, state, table, slot)) {
        state.tapped.set(slot);
    } else if (entry.mana_source.enters_tapped_unless == EntersTappedUnless::PayLife) {
        // Entering untapped was a choice and it was paid for. The fetch path
        // used to skip this, which made every fetched Breeding Pool free.
        state.life -= entry.mana_source.enters_tapped_param;
    }
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
                                 .is_creature = false,
                                 .slot = slot});
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
                                 .is_creature = false,
                                 .slot = slot});
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
            if (!condition_met(mana, db, effects, state, slot)) {
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
        // The slot as it sits on the battlefield, NOT the effective one: paying
        // taps the permanent that is there, and a clone is its own permanent.
        source.slot = slot;
        out.push_back(source);
    });
}

int life_cost_of_tapping(const EffectDb& effects, const GameState& state, int slot) noexcept {
    // Through effective(), because a clone taps as the card it copied and pays
    // that card's life cost.
    const CardEffects& entry =
        effects.by_slot[static_cast<std::size_t>(state.effective(slot))];
    return entry.has_mana_source ? entry.mana_source.life_cost : 0;
}

}  // namespace cs

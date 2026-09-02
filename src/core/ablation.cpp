#include "core/ablation.hpp"

namespace cs {

int slot_of_listed(const CardDb& db, const std::string& listed) noexcept {
    for (const Card& card : db.cards) {
        if (card.listed_name == listed) {
            return card.export_index;
        }
    }
    return -1;
}

AblatedDeck ablate(const CardDb& db, const EffectDb& effects, const PolicyWeights& weights,
                   const PatternSet& patterns, int slot, int replacement_slot) {
    const auto index = static_cast<std::size_t>(slot);
    const auto source = static_cast<std::size_t>(replacement_slot);
    if (slot < 0 || index >= db.cards.size()) {
        throw AblationError("ablation slot out of range");
    }
    if (replacement_slot < 0 || source >= db.cards.size()) {
        throw AblationError("replacement slot out of range");
    }
    // Section 9.4: ablating the replacement card itself is an error, not a
    // special case. It would compare a deck against itself and report zero,
    // which reads as "this card is worthless" rather than as a bad question.
    if (slot == replacement_slot) {
        throw AblationError("cannot ablate the replacement card itself (" +
                            db.cards[index].listed_name +
                            "): the two arms would be the same deck, and the result would "
                            "read as a measured zero rather than as a malformed comparison");
    }
    // The commander is not ablatable under a replacement design. It starts in
    // the command zone and the policy only ever plays lands from HAND, so a
    // Forest swapped in there is stuck forever - which is a 99-card deck, the
    // exact conflation section 9.4 exists to prevent.
    if (db.cards[index].is_commander) {
        throw AblationError("cannot ablate the commander (" + db.cards[index].listed_name +
                            "): the replacement would sit in the command zone unplayable, "
                            "making this a 99-card deck rather than a substitution");
    }

    AblatedDeck arm{db, effects, weights, patterns};

    Card replacement = db.cards[source];
    // The slot NUMBER is the identity everything downstream indexes by - zones,
    // patterns, ranks - so it stays. Only what lives in it changes.
    replacement.export_index = slot;
    replacement.is_commander = false;
    arm.db.cards[index] = replacement;

    arm.effects.by_slot[index] = effects.by_slot[source];

    if (index < arm.weights.rank.size() && source < weights.rank.size()) {
        arm.weights.rank[index] = weights.rank[source];
    }

    // The creature mask, recomputed from the ablated deck. Enduring Vitality is
    // an enchantment CREATURE and a Forest is not, so leaving this alone would
    // let creature_count_gte keep counting a card that is no longer there.
    arm.patterns.creature_slots.clear(slot);
    if (arm.db.cards[index].is_creature()) {
        arm.patterns.creature_slots.set(slot);
    }

    // Any requirement naming the ablated slot becomes unsatisfiable. NOT
    // "remove the slot from the mask", which would make the requirement easier
    // and is the more tempting of the two.
    const auto names_slot = [slot](const Requirement& requirement) {
        return requirement.in_play.test(slot) || requirement.in_hand.test(slot) ||
               requirement.in_play_or_hand.test(slot) || requirement.untapped.test(slot) ||
               (requirement.has_any_of && requirement.any_of.test(slot));
    };
    for (Engine& engine : arm.patterns.engines) {
        engine.requires_.impossible = engine.requires_.impossible || names_slot(engine.requires_);
    }
    for (WinPattern& pattern : arm.patterns.patterns) {
        pattern.requires_.impossible =
            pattern.requires_.impossible || names_slot(pattern.requires_);
    }
    return arm;
}

}  // namespace cs

#pragma once

// Leave-one-out ablation (SIM_PLAN.md section 9.4).
//
// Ablation REPLACES the removed card; it does not shrink the deck. A 98-card
// deck conflates "this card is good" with "a smaller deck draws its good cards
// sooner", and value-above-replacement is undefined without a replacement.
//
// The replacement is a per-deck declaration, so the bias is stated rather than
// hardcoded: for a mana-hungry deck a Forest raises the land count by one, so
// every nonland ablation is measured against a slightly better-manaed deck.
// That bias is inherent in choosing any replacement and is printed with the
// result.
//
// WHY THIS IS A SLOT SWAP AND NOT A DECK REBUILD, which matters more than it
// looks: the ablated deck keeps the SAME NUMBER OF SLOTS in the SAME ORDER, so
// for a given seed the shuffle is the identical permutation of slot indices.
// Only the identity of one slot differs. Section 10.4 warned that common random
// numbers would be imperfectly coupled because "the same seed does not produce
// the same shuffle of a deck with Forest in slot 42 as of one with Mana Crypt
// there" - under a slot swap it does, exactly, and the two arms diverge only
// where the policy makes a different decision.

#include <stdexcept>
#include <string>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "core/pattern.hpp"
#include "core/policy.hpp"

namespace cs {

class AblationError : public std::runtime_error {
public:
    explicit AblationError(const std::string& what) : std::runtime_error(what) {}
};

// One arm of a comparison: a deck with exactly one slot swapped.
//
// Carries its own copies rather than referring back, because the arms are run
// concurrently and a shared mutable view is the thing section 7.3 spent its
// design budget avoiding.
struct AblatedDeck {
    CardDb db;
    EffectDb effects;
    PolicyWeights weights;
    // Its OWN patterns, because a pattern naming the ablated card must become
    // unsatisfiable rather than rebind to whatever took the slot. See
    // Requirement::impossible for what that cost before it was noticed.
    PatternSet patterns;
};

// Replaces `slot` with the card at `replacement_slot`, everywhere it is defined:
// the card record, the authored effects, the policy rank, the pattern set's
// creature mask, and any requirement that named the ablated card. FIVE places,
// one call.
//
// The count is the point. The first version updated three of them and produced
// a sweep in which Enduring Vitality, an engine piece, read as costing the deck
// 7.6 points - because its engine's slot mask now asked for the Forest that
// replaced it, and the deck plays a Forest almost every game. Leaving any one
// of these behind gives a card that draws as a Forest and scores, or wins, as
// something else.
[[nodiscard]] AblatedDeck ablate(const CardDb& db, const EffectDb& effects,
                                 const PolicyWeights& weights, const PatternSet& patterns,
                                 int slot, int replacement_slot);

// The slot of a listed card name, or -1.
[[nodiscard]] int slot_of_listed(const CardDb& db, const std::string& listed) noexcept;

}  // namespace cs

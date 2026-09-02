#pragma once

// Paying a cost from a set of mana sources.
//
// This is the hot path (SIM_PLAN.md section 11): can_pay is called for every
// candidate card, every turn, of every game. It is written for correctness
// first and has not been optimised - see section 2.4 on why the budget is
// roughly 10x larger than the original estimate.
//
// THE CENTRAL CONSTRAINT, and the reason this is not a sum:
//
//   A source produces `amount` mana, and all of it is ONE colour chosen from
//   `produces`. That is not a simplification, it is Kinnan: "add one mana of
//   any type that permanent produced" (section 2.6). A Birds of Paradise under
//   Kinnan makes two mana of one colour, NOT one green and one blue - so it
//   cannot pay {G}{U} by itself, and a model that summed available colours
//   would say it could.

#include <cstdint>
#include <span>

#include "core/card.hpp"

namespace cs {

// A tapped-for-mana ability, reduced to what payment needs to know.
struct Source {
    // Which colours this can produce. Zero means colourless-only: the mana is
    // real and pays generic costs, but cannot satisfy a coloured pip. Sol Ring
    // and Basalt Monolith are the common case.
    ColourMask produces = 0;
    std::uint8_t amount = 1;

    // Kinnan multiplies NONLAND permanents only. This flag is the difference
    // between the deck's engine working and not, which is why it is the one
    // MANA_SOURCE field with a test of its own (section 4.2).
    bool is_land = false;
    bool is_creature = false;

    // WHERE this mana came from, so that spending it can consume it.
    //
    // -1, not 0, and the choice is the eleventh rule in the ingestion repo's
    // PLAN.md 11.0: a "no value" state expressible as a valid value gets read
    // as one. Slot 0 is a real card. -1 is outside the domain of deck slots and
    // cannot be produced by value-initialisation, by memset, or by a caller
    // that builds a Source with designated initialisers and forgets this field
    // - which every test in this repo does.
    //
    // A Source with no slot is legitimate: a test constructs abstract mana that
    // came from nowhere. It simply cannot be spent.
    int slot = -1;
};

// Kinnan, and anything shaped like it.
//
// "Whenever you tap a nonland permanent for mana, add one mana of any type that
// permanent produced." An ADDITIVE +1 of an already-produced type, not a
// doubler: Basalt Monolith taps for {C}{C}{C} and becomes 4, not 6.
struct ManaMultiplier {
    std::uint8_t bonus = 1;
    bool nonland_only = true;
};

[[nodiscard]] Source with_multiplier(Source source, const ManaMultiplier& multiplier) noexcept;

// WHICH sources pay a cost, not merely whether it can be paid.
//
// This exists because the answer was being computed and thrown away. The
// coloured-pip search commits specific sources to specific colours - it has to,
// since a source produces one colour and payability is a matching, not a sum -
// and `can_pay` returned a bool and discarded the matching. The turn loop then
// re-derived "which sources do I tap" in slot order, crudely and differently.
//
// That is the twelfth rule in the ingestion repo's PLAN.md 11.0 - two functions
// independently deriving one concept - and its cost here was MEASURED before
// this was written: swapping the turn loop's arbitrary tie-break from slot order
// to lands-first moved P(assembled by turn 3) by 1.33 points, against a 95%
// interval of 0.34. An uncontrolled term four times the size of the stated
// uncertainty, at the objective section 4.1 selects.
//
// `spend` is a bitmask over INDICES INTO THE SOURCES SPAN, not over deck slots:
// this type knows nothing about zones, and the caller maps an index back to
// whatever it has to tap, sacrifice or exile.
struct Payment {
    bool payable = false;
    std::uint64_t spend[2] = {0, 0};
    // Total mana the chosen sources produce. >= the cost; the excess is the
    // overpayment the selection below tries to minimise.
    int spent = 0;

    [[nodiscard]] constexpr bool spends(std::size_t index) const noexcept {
        return index < 128 && (spend[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
    }
};

// Plans a payment: the colour matching, plus enough further sources to cover
// generic.
//
// THE SELECTION RULE FOR GENERIC IS STATED, because there is no free answer and
// an unstated one is how an arbitrary tie-break became worth 1.33 points:
//
//   Sources committed by the colour matching are spent - they are required.
//   The remaining generic is then filled BEST FIT: repeatedly take the largest
//   uncommitted source that does not exceed what is still owed, and when none
//   fits, the smallest source that covers it. Ties by index, ascending.
//
// That minimises overpayment, which is a dominance argument rather than a
// preference: mana left untapped is weakly better than mana wasted, because it
// can pay for the next spell this turn.
//
// What it deliberately does NOT do is prefer to keep any PARTICULAR source
// untapped. "Do not tap Kinnan, a pattern needs it untapped" is a policy
// judgement, section 6.4's constraint problem proper, and inventing one here
// would put a decision in the mana layer where the scorer could not see it.
[[nodiscard]] Payment plan_payment(const Cost& cost, std::span<const Source> sources,
                                   int x = 0) noexcept;

// Can this cost be paid from these sources, with {X} set to `x`?
//
// `x` is a parameter rather than something inferred, because choosing X is a
// policy decision and not a cost lookup (section 2.2). Six of this deck's seven
// X spells are castable at X=0 and do nothing there.
[[nodiscard]] bool can_pay(const Cost& cost, std::span<const Source> sources, int x = 0) noexcept;

// The largest X this cost can be paid at, or 0 for a cost with no {X}.
// Returns -1 if the cost cannot be paid at all, even at X=0.
[[nodiscard]] int max_affordable_x(const Cost& cost, std::span<const Source> sources) noexcept;

// Total mana these sources produce if every one is tapped.
[[nodiscard]] int total_mana(std::span<const Source> sources) noexcept;

}  // namespace cs

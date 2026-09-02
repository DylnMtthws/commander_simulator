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

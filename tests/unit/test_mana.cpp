// Paying costs. The hot path, and the piece most likely to be subtly wrong.
//
// Per PLAN.md rule 7a upstream, every rejection here names what it rejects; per
// 7b, helpers that build inputs are trivial enough to have no construction step
// that could silently misfire.

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/mana.hpp"

namespace {

constexpr auto W = cs::colour_bit(cs::Colour::White);
constexpr auto U = cs::colour_bit(cs::Colour::Blue);
constexpr auto B = cs::colour_bit(cs::Colour::Black);
constexpr auto R = cs::colour_bit(cs::Colour::Red);
constexpr auto G = cs::colour_bit(cs::Colour::Green);
constexpr cs::ColourMask kAny = W | U | B | R | G;

// Named after the real cards, so a failure says which board state broke.
const cs::Source kForest{.produces = G, .amount = 1, .is_land = true, .is_creature = false};
const cs::Source kIsland{.produces = U, .amount = 1, .is_land = true, .is_creature = false};
const cs::Source kTropicalIsland{.produces = G | U, .amount = 1, .is_land = true,
                                 .is_creature = false};
const cs::Source kBirds{.produces = kAny, .amount = 1, .is_land = false, .is_creature = true};
const cs::Source kSolRing{.produces = 0, .amount = 2, .is_land = false, .is_creature = false};
const cs::Source kBasalt{.produces = 0, .amount = 3, .is_land = false, .is_creature = false};
const cs::Source kAncientTomb{.produces = 0, .amount = 2, .is_land = true, .is_creature = false};

cs::Cost cost_of(int generic, std::initializer_list<cs::Colour> pips, int variable = 0) {
    cs::Cost cost;
    cost.generic = static_cast<std::uint8_t>(generic);
    cost.variable = static_cast<std::uint8_t>(variable);
    for (const cs::Colour c : pips) {
        ++cost.pips[static_cast<std::size_t>(c)];
    }
    return cost;
}

bool payable(const cs::Cost& cost, const std::vector<cs::Source>& sources, int x = 0) {
    return cs::can_pay(cost, sources, x);
}

}  // namespace

TEST_CASE("pays the ordinary cases", "[mana]") {
    SECTION("a one-drop off a matching land") {
        REQUIRE(payable(cost_of(0, {cs::Colour::Green}), {kForest}));
    }
    SECTION("generic is payable by colourless mana") {
        // Sol Ring's {C}{C} pays {2} but could never pay {G}.
        REQUIRE(payable(cost_of(2, {}), {kSolRing}));
        REQUIRE_FALSE(payable(cost_of(0, {cs::Colour::Green}), {kSolRing}));
    }
    SECTION("a coloured source pays generic once its pip is spent") {
        // Kinnan is {G}{U}: two pips and nothing else.
        REQUIRE(payable(cost_of(0, {cs::Colour::Green, cs::Colour::Blue}),
                        {kForest, kIsland}));
        REQUIRE_FALSE(payable(cost_of(1, {cs::Colour::Green, cs::Colour::Blue}),
                              {kForest, kIsland}));
        REQUIRE(payable(cost_of(1, {cs::Colour::Green, cs::Colour::Blue}),
                        {kForest, kIsland, kSolRing}));
    }
    SECTION("not enough mana is not enough") {
        REQUIRE_FALSE(payable(cost_of(5, {}), {kSolRing, kForest}));
    }
}

TEST_CASE("greedy assignment fails where matching succeeds", "[mana]") {
    // The case that makes this a search rather than a subtraction.
    //
    // Cost {G}{U}, board a Tropical Island (either colour) and an Island (blue
    // only). A greedy pass that walks colours in WUBRG order and takes the
    // first capable source assigns Tropical to BLUE, then finds nothing for
    // green and reports "cannot pay" - on a board that pays it comfortably.
    //
    // The exact pair matters, and this was verified against a deliberately
    // greedy implementation rather than assumed. {G}{U} off [Tropical, Forest]
    // does NOT discriminate: greedy reaches blue first, Tropical is the only
    // blue source, so it happens to make the right choice for the wrong reason.
    // A test that cannot fail against the implementation it exists to rule out
    // is decoration (upstream PLAN.md rule 7a).
    const cs::Cost gu = cost_of(0, {cs::Colour::Green, cs::Colour::Blue});

    SECTION("the discriminating board") {
        REQUIRE(payable(gu, {kTropicalIsland, kIsland}));
        REQUIRE(payable(gu, {kIsland, kTropicalIsland}));  // and independent of order
    }
    SECTION("the mirror, which greedy happens to get right") {
        REQUIRE(payable(gu, {kTropicalIsland, kForest}));
        REQUIRE(payable(gu, {kForest, kTropicalIsland}));
    }
    SECTION("two flexible sources, two needs") {
        REQUIRE(payable(gu, {kTropicalIsland, kTropicalIsland}));
    }
    SECTION("genuinely impossible is still refused") {
        // Two Forests cannot make blue however they are assigned. Guards
        // against a can_pay that simply returns true.
        REQUIRE_FALSE(payable(gu, {kForest, kForest}));
        REQUIRE_FALSE(payable(gu, {kIsland, kIsland}));
    }
}

TEST_CASE("a source produces one colour, not a menu", "[mana][kinnan]") {
    // The Kinnan subtlety, and the reason payment is not a sum over available
    // colours. Kinnan adds "one mana of any type that permanent produced", so a
    // Birds of Paradise under Kinnan makes TWO MANA OF ONE COLOUR.
    const cs::Source kinnanBirds =
        cs::with_multiplier(kBirds, cs::ManaMultiplier{.bonus = 1, .nonland_only = true});
    REQUIRE(kinnanBirds.amount == 2);

    const cs::Cost gu = cost_of(0, {cs::Colour::Green, cs::Colour::Blue});
    const cs::Cost gg = cost_of(0, {cs::Colour::Green, cs::Colour::Green});

    // Two mana, two pips, and the colours are available - yet it cannot pay,
    // because both mana must be the same colour. A model that summed the
    // reachable colours would say yes here and overstate the deck.
    REQUIRE(cs::total_mana(std::array{kinnanBirds}) == 2);
    REQUIRE_FALSE(payable(gu, {kinnanBirds}));

    // Same source, same two mana, one colour: fine.
    REQUIRE(payable(gg, {kinnanBirds}));
}

TEST_CASE("Kinnan does not see lands", "[mana][kinnan]") {
    // is_land is the one MANA_SOURCE flag the loop actually reads, and getting
    // it wrong is the difference between the deck's engine working and not
    // (SIM_PLAN.md section 4.2). Tested directly rather than as a side effect.
    const cs::ManaMultiplier kinnan{.bonus = 1, .nonland_only = true};

    SECTION("a nonland artifact is multiplied") {
        // Basalt Monolith: {C}{C}{C} becomes 4, not 6 - Kinnan is additive.
        const cs::Source multiplied = cs::with_multiplier(kBasalt, kinnan);
        REQUIRE(kBasalt.amount == 3);
        REQUIRE(multiplied.amount == 4);
    }
    SECTION("a nonland creature is multiplied") {
        REQUIRE(cs::with_multiplier(kBirds, kinnan).amount == 2);
    }
    SECTION("a basic land is NOT multiplied") {
        REQUIRE(cs::with_multiplier(kForest, kinnan).amount == kForest.amount);
    }
    SECTION("a land that taps for two is NOT multiplied either") {
        // Ancient Tomb makes {C}{C} and is a land: 2 stays 2. If is_land were
        // dropped this would silently become 3 and every game would run fast.
        REQUIRE(cs::with_multiplier(kAncientTomb, kinnan).amount == 2);
    }
    SECTION("a multiplier that is not nonland-only does see lands") {
        // Guards the flag itself: with nonland_only false, the same land IS
        // multiplied - so the test above is checking is_land, not a no-op.
        const cs::ManaMultiplier everything{.bonus = 1, .nonland_only = false};
        REQUIRE(cs::with_multiplier(kForest, everything).amount == 2);
    }
}

TEST_CASE("the Kinnan plus Basalt engine pays what it should", "[mana][kinnan]") {
    // Basalt under Kinnan taps for 4 and untaps for {3}: net +1 colourless per
    // iteration (SIM_PLAN.md section 2.6). The mana is COLOURLESS, which is why
    // Thrasios ({4}, generic) is the outlet and Kinnan's own {5}{G}{U} is not.
    const cs::Source engine =
        cs::with_multiplier(kBasalt, cs::ManaMultiplier{.bonus = 1, .nonland_only = true});
    REQUIRE(engine.amount == 4);

    SECTION("Thrasios activates off it: {4} is generic") {
        REQUIRE(payable(cost_of(4, {}), {engine}));
    }
    SECTION("Kinnan's own {5}{G}{U} does NOT, however much colourless there is") {
        const cs::Cost dig = cost_of(5, {cs::Colour::Green, cs::Colour::Blue});
        REQUIRE_FALSE(payable(dig, {engine, engine, engine}));
        // It works the moment the two coloured pips come from somewhere else.
        REQUIRE(payable(dig, {engine, engine, kForest, kIsland}));
    }
}

TEST_CASE("X is a parameter, not a lookup", "[mana][x]") {
    // Finale of Devastation, {X}{G}{G}. Castable at X=0 for two mana, where it
    // finds a creature with mana value 0 and does nothing (section 2.2).
    const cs::Cost finale = cost_of(0, {cs::Colour::Green, cs::Colour::Green}, /*variable=*/1);

    SECTION("castable at X=0 off two green") {
        REQUIRE(payable(finale, {kForest, kForest}, 0));
    }
    SECTION("X=1 needs a third mana, of any colour") {
        REQUIRE_FALSE(payable(finale, {kForest, kForest}, 1));
        REQUIRE(payable(finale, {kForest, kForest, kSolRing}, 1));
    }
    SECTION("the pips are still required however much generic is available") {
        REQUIRE_FALSE(payable(finale, {kBasalt, kBasalt, kBasalt}, 0));
    }
    SECTION("a negative X is refused rather than wrapping") {
        REQUIRE_FALSE(payable(finale, {kForest, kForest}, -1));
    }
}

TEST_CASE("max_affordable_x reports the largest payable X", "[mana][x]") {
    const cs::Cost finale = cost_of(0, {cs::Colour::Green, cs::Colour::Green}, 1);

    SECTION("exactly the leftover mana after the pips") {
        // Two Forests pay the pips; Sol Ring's two colourless pay X.
        REQUIRE(cs::max_affordable_x(finale, std::array{kForest, kForest, kSolRing}) == 2);
    }
    SECTION("zero when the pips consume everything") {
        REQUIRE(cs::max_affordable_x(finale, std::array{kForest, kForest}) == 0);
    }
    SECTION("-1 when the cost cannot be paid at all") {
        REQUIRE(cs::max_affordable_x(finale, std::array{kIsland, kIsland}) == -1);
    }
    SECTION("a cost with no {X} reports 0, not its leftover mana") {
        REQUIRE(cs::max_affordable_x(cost_of(1, {}), std::array{kSolRing, kBasalt}) == 0);
    }
    SECTION("scales with a real Kinnan engine") {
        const cs::Source engine =
            cs::with_multiplier(kBasalt, cs::ManaMultiplier{.bonus = 1, .nonland_only = true});
        REQUIRE(cs::max_affordable_x(finale, std::array{kForest, kForest, engine}) == 4);
    }
}

TEST_CASE("a Phyrexian pip currently requires its colour", "[mana]") {
    // Documented simplification with a stated direction: {U/P} is really
    // payable with 2 life, so this understates. Moot here - Mental Misstep is
    // the only one and it is inert - but the test pins the behaviour so the
    // day life is tracked, this fails and gets revisited.
    cs::Cost misstep;
    misstep.phyrexian[static_cast<std::size_t>(cs::Colour::Blue)] = 1;

    REQUIRE(payable(misstep, {kIsland}));
    REQUIRE_FALSE(payable(misstep, {kForest}));
    REQUIRE_FALSE(payable(misstep, {}));
}

TEST_CASE("an empty board pays only a free spell", "[mana]") {
    REQUIRE(payable(cost_of(0, {}), {}));
    REQUIRE_FALSE(payable(cost_of(1, {}), {}));
}

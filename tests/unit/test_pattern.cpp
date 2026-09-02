// Engines, and the difference between a loop being PRESENT and being ENTERABLE.
//
// Unbounded mana here is DETECTED from a declared engine, never produced by
// simulation (SIM_PLAN.md section 5). That is the right call - executing the
// loop would drag a mana pool into GameState and route every cast decision
// through it - but it puts the whole weight of correctness on the engine's
// requirement being tight. These tests are that requirement.

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/pattern.hpp"
#include "core/state.hpp"

namespace {

constexpr int kKinnan = 0;
constexpr int kBasalt = 1;

cs::PatternSet kinnan_basalt(bool with_entry_cost) {
    cs::PatternSet set;
    set.flag_names.emplace_back("INFINITE:C");
    cs::Engine engine;
    engine.name = "kinnan_basalt";
    engine.sets = cs::FlagMask{1};
    engine.requires_.in_play.set(kKinnan);
    engine.requires_.in_play.set(kBasalt);
    if (with_entry_cost) {
        engine.requires_.loop_entry_cost = 3;
    }
    set.engines.push_back(engine);
    return set;
}

cs::Source colourless(std::uint8_t amount) {
    return cs::Source{.produces = 0, .amount = amount};
}
cs::Source land() {
    return cs::Source{.produces = 0x1F, .amount = 1, .is_land = true};
}

bool loop_detected(const cs::PatternSet& set, const cs::GameState& state,
                   const std::vector<cs::Source>& sources) {
    return cs::active_flags(set, state, sources) != 0;
}

cs::GameState board_with_both(bool basalt_untapped) {
    cs::GameState state;
    state.battlefield.set(kKinnan);
    state.battlefield.set(kBasalt);
    if (!basalt_untapped) {
        state.tapped.set(kBasalt);
    }
    return state;
}

}  // namespace

TEST_CASE("an untapped Basalt pays its own loop entry", "[pattern][loop]") {
    // No special case is needed for "is Basalt tapped": an untapped Basalt IS a
    // source, offering 4 under Kinnan, so it pays the {3} itself. One check
    // covers both cases.
    const cs::PatternSet set = kinnan_basalt(true);
    REQUIRE(loop_detected(set, board_with_both(true), {colourless(4)}));
}

TEST_CASE("a tapped Basalt needs the entry cost from elsewhere", "[pattern][loop]") {
    const cs::PatternSet set = kinnan_basalt(true);
    SECTION("three other mana enters the loop") {
        REQUIRE(loop_detected(set, board_with_both(false), {land(), land(), land()}));
    }
    SECTION("two does not") {
        REQUIRE_FALSE(loop_detected(set, board_with_both(false), {land(), land()}));
    }
    SECTION("none does not") {
        REQUIRE_FALSE(loop_detected(set, board_with_both(false), {}));
    }
}

TEST_CASE("presence alone is not the engine", "[pattern][loop][rule8]") {
    // THE discriminating pair. A presence-only requirement - both pieces on the
    // board, no entry cost - fires on a board holding Kinnan, a tapped Basalt
    // and two mana. That is a loop you cannot enter, and it would overstate the
    // deck at exactly the turn boundaries the CDF is most sensitive to.
    const cs::PatternSet strict = kinnan_basalt(true);
    const cs::PatternSet loose = kinnan_basalt(false);
    const cs::GameState state = board_with_both(false);
    const std::vector<cs::Source> two_mana{land(), land()};

    REQUIRE(loop_detected(loose, state, two_mana));         // the wrong answer
    REQUIRE_FALSE(loop_detected(strict, state, two_mana));  // ours
}

TEST_CASE("both pieces are still required", "[pattern][loop]") {
    // Guards the guard: the entry cost must not be sufficient on its own, or
    // the engine would fire off three lands and no combo at all.
    const cs::PatternSet set = kinnan_basalt(true);
    cs::GameState state;
    state.battlefield.set(kKinnan);  // no Basalt
    REQUIRE_FALSE(loop_detected(set, state, {land(), land(), land(), land()}));
}

TEST_CASE("a coloured entry cost is a colour check, not a quantity one", "[pattern][entry_pips]") {
    // R2 (SIM_PLAN.md section 16.7), and the case that needed it: Thrasios in
    // HAND must be CAST for {G}{U}, while INFINITE:C supplies only {C}. No
    // amount of colourless mana pays for it, so a generic entry cost cannot
    // express the requirement however large it is made.
    //
    // This exists because the pattern went to ZERO fires once the cost was
    // added, and a test that only sees zero cannot tell "correctly dead under
    // this policy" from "the check never passes".
    cs::PatternSet set;
    cs::WinPattern pattern;
    pattern.name = "outlet_in_hand";
    pattern.requires_.in_hand.set(0);
    pattern.requires_.loop_entry_cost = 0;
    pattern.requires_.activations = 1;
    pattern.requires_.entry_pips[static_cast<std::size_t>(cs::Colour::Green)] = 1;
    pattern.requires_.entry_pips[static_cast<std::size_t>(cs::Colour::Blue)] = 1;
    set.patterns.push_back(pattern);

    cs::GameState state;
    state.hand.set(0);

    SECTION("unbounded COLOURLESS mana does not satisfy it") {
        const std::vector<cs::Source> colourless(40, cs::Source{.produces = 0, .amount = 3});
        REQUIRE(cs::first_satisfied(set, state, colourless) < 0);
    }
    SECTION("one green and one blue do") {
        const std::vector<cs::Source> coloured{
            cs::Source{.produces = 0x10, .amount = 1},  // {G}
            cs::Source{.produces = 0x02, .amount = 1},  // {U}
        };
        REQUIRE(cs::first_satisfied(set, set.patterns.empty() ? state : state, coloured) == 0);
    }
    SECTION("one dual producing either, alone, does NOT - a source makes one colour") {
        // The section 2.6 property, reaching the pattern layer: a source produces
        // `amount` mana all of ONE colour, so a single {G/U} land is not {G}{U}.
        const std::vector<cs::Source> one_dual{cs::Source{.produces = 0x12, .amount = 2}};
        REQUIRE(cs::first_satisfied(set, state, one_dual) < 0);
    }
    SECTION("activations scale the pips too") {
        set.patterns[0].requires_.activations = 2;
        const std::vector<cs::Source> just_enough_once{
            cs::Source{.produces = 0x10, .amount = 1},
            cs::Source{.produces = 0x02, .amount = 1},
        };
        REQUIRE(cs::first_satisfied(set, state, just_enough_once) < 0);
    }
}

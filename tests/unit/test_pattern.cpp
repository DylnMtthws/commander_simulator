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

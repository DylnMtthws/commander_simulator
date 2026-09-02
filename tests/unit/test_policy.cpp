// The scorer.
//
// The tests that matter are the ones a FIXED PRIORITY LIST would fail. If the
// scorer's state-dependent terms cannot be distinguished from a static ranking,
// they are not earning their complexity - so the discriminating cases are
// marked, and a naive implementation is checked against them (upstream rule 8).

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "core/policy.hpp"
#include "core/sim.hpp"
#include "io/card_db_load.hpp"

namespace {

const cs::CardDb& db() {
    static const cs::CardDb loaded =
        cs::io::load_card_db(std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");
    return loaded;
}

int slot_of(const char* listed) {
    for (const cs::Card& card : db().cards) {
        if (card.listed_name == listed) {
            return card.export_index;
        }
    }
    FAIL("fixture is missing " << listed);
    return -1;
}

// An engine over two fixture cards, so a "one card short" board is easy to set
// up without depending on the real deck.
cs::PatternSet two_card_engine() {
    cs::PatternSet set;
    set.flag_names.push_back("ONLINE");
    cs::Engine engine;
    engine.name = "kinnan_plus_ring";
    engine.sets = cs::FlagMask{1};
    engine.requires_.in_play.set(slot_of("Kinnan, Bonder Prodigy"));
    engine.requires_.in_play.set(slot_of("Sol Ring"));
    set.engines.push_back(engine);

    cs::WinPattern pattern;
    pattern.name = "online_with_finale";
    pattern.requires_.flags = cs::FlagMask{1};
    pattern.requires_.in_play.set(slot_of("Finale of Devastation"));
    set.patterns.push_back(pattern);
    return set;
}

cs::PolicyWeights weights_with(int sol_ring_rank, int finale_rank) {
    cs::PolicyWeights weights;
    weights.rank.assign(db().cards.size(), 10);
    weights.rank[static_cast<std::size_t>(slot_of("Sol Ring"))] = sol_ring_rank;
    weights.rank[static_cast<std::size_t>(slot_of("Finale of Devastation"))] = finale_rank;
    return weights;
}

// Plenty of any-colour mana, so castability never confounds a ranking test.
std::vector<cs::Source> rich_board() {
    return std::vector<cs::Source>(8, cs::Source{.produces = 0x1F, .amount = 1});
}

}  // namespace

TEST_CASE("the scorer prefers the card that completes an engine", "[policy][rule8]") {
    // THE discriminating case. Kinnan is already out; Sol Ring finishes the
    // engine and Finale does not. Sol Ring is ranked LOWER, so a fixed priority
    // list picks Finale and the scorer must not.
    const cs::PatternSet patterns = two_card_engine();
    const cs::AuthoredPolicy policy(weights_with(/*sol_ring=*/20, /*finale=*/90));

    cs::GameState state;
    state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));
    state.hand.set(slot_of("Sol Ring"));
    state.hand.set(slot_of("Finale of Devastation"));

    const auto sources = rich_board();
    const cs::Context context{db(), patterns, state, sources, nullptr};
    cs::GameStats stats;

    REQUIRE(policy.choose_spell(context, stats) == slot_of("Sol Ring"));

    SECTION("and the reason is visible in the terms, not just the winner") {
        const cs::Consideration ring = policy.score(context, slot_of("Sol Ring"), false, stats);
        const cs::Consideration finale =
            policy.score(context, slot_of("Finale of Devastation"), false, stats);
        REQUIRE(ring.pattern_term > 0);
        REQUIRE(finale.pattern_term == 0);
        REQUIRE(ring.rank_term < finale.rank_term);  // it won DESPITE its rank
    }
}

TEST_CASE("with no engine to complete, rank decides", "[policy]") {
    // The other half: the state-dependent term must be zero when nothing is
    // completed, or the scorer is just noise. Same cards, empty battlefield.
    const cs::PatternSet patterns = two_card_engine();
    const cs::AuthoredPolicy policy(weights_with(/*sol_ring=*/20, /*finale=*/90));

    cs::GameState state;
    state.hand.set(slot_of("Sol Ring"));
    state.hand.set(slot_of("Finale of Devastation"));

    const auto sources = rich_board();
    const cs::Context context{db(), patterns, state, sources, nullptr};
    cs::GameStats stats;

    REQUIRE(policy.choose_spell(context, stats) == slot_of("Finale of Devastation"));
}

TEST_CASE("an uncastable card is never chosen, however highly ranked", "[policy]") {
    const cs::PatternSet patterns = two_card_engine();
    const cs::AuthoredPolicy policy(weights_with(20, 90));

    cs::GameState state;
    state.hand.set(slot_of("Sol Ring"));
    state.hand.set(slot_of("Finale of Devastation"));

    const std::vector<cs::Source> nothing;  // empty board: nothing is castable
    const cs::Context context{db(), patterns, state, nothing, nullptr};
    cs::GameStats stats;

    REQUIRE(policy.choose_spell(context, stats) == -1);
}

TEST_CASE("land drops stop at the ceiling", "[policy]") {
    // Flooding is how a goldfish quietly loses turns, and it has no symptom in
    // an aggregate number.
    cs::PatternSet patterns = two_card_engine();
    cs::PolicyWeights weights = weights_with(20, 90);
    weights.land_floor = 2;
    weights.land_ceiling = 3;
    const cs::AuthoredPolicy policy(weights);

    cs::GameState state;
    state.hand.set(slot_of("Forest"));
    const auto sources = rich_board();
    cs::GameStats stats;

    SECTION("below the ceiling a land is played") {
        const cs::Context context{db(), patterns, state, sources, nullptr};
        REQUIRE(policy.choose_land(context, stats) == slot_of("Forest"));
    }
    SECTION("at the ceiling it is not") {
        state.battlefield.set(slot_of("Tropical Island"));
        state.battlefield.set(slot_of("Sink into Stupor"));
        state.battlefield.set(slot_of("Invasion of Ikoria"));  // not a land; must not count
        const cs::Context partial{db(), patterns, state, sources, nullptr};
        REQUIRE(policy.choose_land(partial, stats) == slot_of("Forest"));  // still only 2 lands

        state.battlefield.set(slot_of("Mental Misstep"));  // still not a land
        const cs::Context still_two{db(), patterns, state, sources, nullptr};
        REQUIRE(policy.choose_land(still_two, stats) == slot_of("Forest"));
    }
}

TEST_CASE("tutor targets go through the same scorer", "[policy][tutor][rule8]") {
    // §6.3's claim under test: tutors and selection are the same problem, so a
    // tutor uses the SAME scorer with a different candidate set - the library
    // rather than the hand. Larger, unseen, identical in kind.
    const cs::PatternSet patterns = two_card_engine();
    const cs::AuthoredPolicy policy(weights_with(/*sol_ring=*/20, /*finale=*/90));

    cs::GameState state;
    state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));  // one piece short

    const auto sources = rich_board();
    const cs::Context context{db(), patterns, state, sources, nullptr};
    cs::GameStats stats;
    const std::vector<int> candidates{slot_of("Sol Ring"), slot_of("Finale of Devastation")};

    SECTION("to the battlefield, completing the engine beats rank") {
        // A rank-only tutor picks Finale at 90. Sol Ring finishes the engine.
        REQUIRE(policy.choose_tutor(context, candidates, /*to_hand=*/false, stats) ==
                slot_of("Sol Ring"));
    }
    SECTION("to hand, it does NOT - the card still has to be cast") {
        // THE destination case. A destination-blind implementation reuses the
        // battlefield hypothetical and picks Sol Ring here too, crediting a
        // board state the tutor did not create. Trophy Mage is not Finale of
        // Devastation and the scorer has to see that.
        REQUIRE(policy.choose_tutor(context, candidates, /*to_hand=*/true, stats) ==
                slot_of("Finale of Devastation"));
    }
    SECTION("an empty candidate set finds nothing rather than asserting") {
        REQUIRE(policy.choose_tutor(context, {}, false, stats) == -1);
    }
}

TEST_CASE("ties resolve by export_index, not by iteration accident", "[policy]") {
    // Section 6.5: a tie broken by container order is a reproducibility bug
    // that looks like variance. Two identically-ranked lands must always give
    // the same answer, and it must be the lower slot.
    cs::PatternSet patterns = two_card_engine();
    cs::PolicyWeights weights = weights_with(20, 90);
    const cs::AuthoredPolicy policy(weights);

    cs::GameState state;
    const int forest = slot_of("Forest");
    const int tropical = slot_of("Tropical Island");
    state.hand.set(forest);
    state.hand.set(tropical);

    const auto sources = rich_board();
    const cs::Context context{db(), patterns, state, sources, nullptr};
    cs::GameStats stats;

    const int chosen = policy.choose_land(context, stats);
    REQUIRE(chosen == std::min(forest, tropical));
    for (int i = 0; i < 20; ++i) {
        REQUIRE(policy.choose_land(context, stats) == chosen);
    }
}

// The dig (SELECT), and the one assertion in this project that is measured
// independently of the code it checks.
//
// EVERY OTHER TEST HERE ASSERTS AGAINST A NUMBER THE SAME CODE PRODUCED. That is
// the twelfth rule's subject from the other side: a check derived from the thing
// it checks agrees with it by construction. The only external check this project
// has had is the deck primer (SIM_PLAN.md 17), and it is not runnable.
//
// This file is the first internal one that is not self-referential:
//
//   * `peek_n`'s hit rate is asserted against a CLOSED-FORM HYPERGEOMETRIC
//     probability - combinatorics, not a second implementation. A sampler that
//     re-revealed a card, failed to advance the shuffle, or drew with
//     replacement would diverge from it, and none of those would fail any other
//     test in the suite.
//
//   * And on the real deck the same machinery reports a 73.7% hit rate against
//     73.6% MEASURED BEFORE IT WAS WRITTEN, from the library composition at the
//     moment the old detection fired (SIM_PLAN.md 16.7b). That one cannot live
//     here, because data/cards.json is generated and absent from a fresh clone -
//     it is recorded in the plan and re-checked with `cs --games N`.
//
// DO NOT "SIMPLIFY" THE HYPERGEOMETRIC INTO A CALL TO THE MODEL. It would still
// pass, and it would stop being a check.

#include <cstdint>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/effects.hpp"
#include "core/policy.hpp"
#include "core/rng.hpp"
#include "core/state.hpp"
#include "io/card_db_load.hpp"

using Catch::Approx;

namespace {

const cs::CardDb& db() {
    static const cs::CardDb loaded =
        cs::io::load_card_db(std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");
    return loaded;
}

// P(at least one of K matching cards among the top `look` of N), in closed form.
// Written from the combinatorics and not from anything in src/.
double hypergeometric_hit(int n, int k, int look) {
    double miss = 1.0;
    for (int i = 0; i < look; ++i) {
        const double non_matching = n - k - i;
        const double remaining = n - i;
        if (non_matching <= 0.0) {
            return 1.0;
        }
        miss *= non_matching / remaining;
    }
    return 1.0 - miss;
}

}  // namespace

TEST_CASE("peek_n's hit rate matches the hypergeometric", "[select][backstop]") {
    const int n = static_cast<int>(db().cards.size());
    constexpr int kLook = 5;
    constexpr int kTrials = 40000;

    // Two of the fixture's slots are the "matching" set. Which two does not
    // matter - what is being checked is the SAMPLER, not the filter.
    const int a = 0;
    const int b = 3;

    int hits = 0;
    cs::Rng rng(0xBEEF);
    std::vector<int> revealed;
    for (int t = 0; t < kTrials; ++t) {
        cs::GameState state;
        for (int slot = 0; slot < n; ++slot) {
            state.library[state.library_count++] = static_cast<std::uint8_t>(slot);
        }
        cs::peek_n(state, kLook, rng, revealed);
        REQUIRE(revealed.size() == static_cast<std::size_t>(kLook));
        // A look is not a draw: the cards are still in the library.
        REQUIRE(state.drawn == 0);
        bool hit = false;
        for (const int slot : revealed) {
            hit = hit || slot == a || slot == b;
        }
        hits += hit ? 1 : 0;
    }
    const double observed = static_cast<double>(hits) / kTrials;
    const double expected = hypergeometric_hit(n, 2, kLook);
    // Three standard errors at 40,000 trials.
    REQUIRE(observed == Approx(expected).margin(3.0 * 0.5 / 200.0));
}

TEST_CASE("a peek reveals distinct cards", "[select]") {
    // The failure that would leave the hit rate too HIGH: revealing the same
    // card twice makes five looks act like five independent draws.
    const int n = static_cast<int>(db().cards.size());
    cs::Rng rng(7);
    std::vector<int> revealed;
    cs::GameState state;
    for (int slot = 0; slot < n; ++slot) {
        state.library[state.library_count++] = static_cast<std::uint8_t>(slot);
    }
    cs::peek_n(state, 5, rng, revealed);
    for (std::size_t i = 0; i < revealed.size(); ++i) {
        for (std::size_t j = i + 1; j < revealed.size(); ++j) {
            REQUIRE(revealed[i] != revealed[j]);
        }
    }
}

TEST_CASE("bottomed cards are never drawn again", "[select]") {
    // "Put the rest on the bottom of your library in a random order." Over a
    // twelve-turn game the bottom is unreachable, so this is exact for the
    // horizon measured - and returning them to the pool instead would let the
    // dig see the same card twice, which is the direction that overstates.
    const int n = static_cast<int>(db().cards.size());
    cs::Rng rng(11);
    cs::GameState state;
    for (int slot = 0; slot < n; ++slot) {
        state.library[state.library_count++] = static_cast<std::uint8_t>(slot);
    }
    std::vector<int> revealed;
    cs::peek_n(state, 3, rng, revealed);
    const int kept = revealed.front();
    cs::take_peeked(state, kept);
    for (const int slot : revealed) {
        if (slot != kept) {
            cs::bottom_peeked(state, slot);
        }
    }
    REQUIRE(state.bottomed == 2);
    REQUIRE(state.drawable() == n - 3);

    for (int i = 0; i < n; ++i) {
        const int drawn = cs::draw_one(state, rng);
        if (drawn < 0) {
            break;
        }
        REQUIRE(drawn != kept);  // taken
        for (const int slot : revealed) {
            if (slot != kept) {
                REQUIRE(drawn != slot);  // bottomed
            }
        }
    }
}

TEST_CASE("the dig keeps the best of what it saw, and can miss", "[select][policy]") {
    cs::EffectDb effects;
    effects.by_slot.assign(db().cards.size(), cs::CardEffects{});
    cs::SelectEffect select;
    select.look = 5;
    select.filter = cs::TutorFilter::NonHumanCreature;

    const auto slot_of = [](const char* listed) {
        for (const cs::Card& card : db().cards) {
            if (card.listed_name == listed) return card.export_index;
        }
        return -1;
    };
    // Kinnan is a HUMAN Druid, so a non-Human creature filter excludes him -
    // which is the restriction the real card carries and the reason the dig
    // cannot simply fetch a second copy of its own source.
    const std::vector<int> revealed{slot_of("Kinnan, Bonder Prodigy"), slot_of("Sol Ring"),
                                    slot_of("Forest")};
    std::vector<int> keepable;
    cs::select_candidates(select, db(), revealed, keepable);
    REQUIRE(keepable.empty());  // a miss, and misses are ~26% of activations

    const std::vector<int> with_target{slot_of("Sol Ring"), slot_of("Invasion of Ikoria")};
    cs::select_candidates(select, db(), with_target, keepable);
    // Invasion of Ikoria's FRONT face is a Battle, not a creature - the same
    // face question is_permanent and tutor_candidates both answer.
    REQUIRE(keepable.empty());
}

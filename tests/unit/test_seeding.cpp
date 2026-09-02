// INVARIANT S1: the outcome of game i is a pure function of
// (deck, config, base_seed, i).
//
// Tested DIRECTLY, not as a side effect of other tests, and tested now while
// the turn loop is trivial. Once a policy exists, a failure here would be
// ambiguous between the RNG and the decisions; today it can only be the RNG.
//
// This test gates common random numbers (SIM_PLAN.md section 10.4), and CRN
// fails silently: if S1 breaks, nothing errors, variance reduction quietly
// stops, and every ablation interval needs ~10x the games with no symptom.

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/ablation.hpp"
#include "core/rng.hpp"
#include "core/sweep.hpp"
#include "core/sim.hpp"
#include "io/card_db_load.hpp"
#include "core/policy.hpp"

namespace {

const std::filesystem::path kFixture = std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json";
constexpr std::uint64_t kBaseSeed = 0xC0FFEE;

const cs::CardDb& fixture() {
    static const cs::CardDb db = cs::io::load_card_db(kFixture);
    return db;
}

// An empty pattern set: S1 is about the RNG, and a pattern firing would end
// games early and vary the number of turns for reasons unrelated to seeding.
// An effect set where every fixture card is a plain colourless source, so the
// loop has mana without depending on the authored file. S1 is about the RNG.
const cs::EffectDb& simple_effects() {
    static const cs::EffectDb effects = [] {
        cs::EffectDb db;
        db.by_slot.assign(fixture().cards.size(), cs::CardEffects{});
        for (auto& entry : db.by_slot) {
            entry.status = cs::AuthorStatus::Modeled;
            entry.has_mana_source = true;
            entry.mana_source.produces = 0x1F;
            entry.mana_source.amount = 1;
            entry.mana_source.is_land = true;
        }
        return db;
    }();
    return effects;
}

const cs::PatternSet& no_patterns() {
    static const cs::PatternSet empty;
    return empty;
}

cs::GameResult play(std::uint64_t index) {
    const cs::StubPolicyDoNotUseForResults policy;
    return cs::run_game(fixture(), simple_effects(), no_patterns(), cs::GameConfig{}, policy,
                        cs::seed_for_game(kBaseSeed, index));
}

// Enough to compare two games without needing operator== on the whole result.
struct Signature {
    std::uint32_t can_pay_calls, turns, drawn, lands, spells;
    std::uint8_t simulated;
    // Without this the signature is blind to the draw order: on a small fixture
    // the stub plays the same COUNT of things whatever it draws, so two
    // genuinely different games compare equal. Found by this file's own
    // "different indices give different games" section failing.
    std::uint64_t digest;
    friend bool operator==(const Signature&, const Signature&) = default;
};

Signature sign(const cs::GameResult& r) {
    return {r.stats.can_pay_calls, r.stats.turns,        r.stats.cards_drawn,
            r.stats.lands_played,  r.stats.spells_cast,  r.turns_simulated,
            r.state_digest};
}

constexpr int kGames = 64;

// S1 again, with the AUTHORED policy rather than the stub.
//
// Re-run after the policy landed, because the policy is exactly where new
// nondeterminism enters: a tiebreak resolved by container order would pass
// every mana and pattern test and show up here as variance. The stub is too
// simple to have ties worth breaking; the scorer is not.
cs::PatternSet engine_patterns() {
    cs::PatternSet set;
    set.flag_names.emplace_back("ONLINE");
    cs::Engine engine;
    engine.sets = cs::FlagMask{1};
    engine.requires_.in_play.set(0);
    set.engines.push_back(engine);
    cs::WinPattern pattern;
    pattern.requires_.flags = cs::FlagMask{1};
    pattern.requires_.in_play.set(1);
    set.patterns.push_back(pattern);
    return set;
}

cs::GameResult play_authored(std::uint64_t index) {
    static const cs::PatternSet patterns = engine_patterns();
    static const cs::AuthoredPolicy policy([] {
        cs::PolicyWeights weights;
        weights.rank.assign(fixture().cards.size(), 10);
        weights.rank[0] = 90;
        return weights;
    }());
    return cs::run_game(fixture(), simple_effects(), patterns, cs::GameConfig{}, policy,
                        cs::seed_for_game(kBaseSeed, index));
}

}  // namespace

TEST_CASE("S1: a game is a pure function of its index", "[seeding][S1]") {
    SECTION("the same index twice gives the same game") {
        for (std::uint64_t i = 0; i < 8; ++i) {
            REQUIRE(sign(play(i)) == sign(play(i)));
        }
    }
    SECTION("different indices give different games") {
        // Guards against the degenerate pass: a run_game that ignored its seed
        // entirely would satisfy every other assertion in this file.
        std::vector<Signature> seen;
        for (std::uint64_t i = 0; i < 16; ++i) {
            seen.push_back(sign(play(i)));
        }
        const bool all_identical =
            std::all_of(seen.begin(), seen.end(), [&](const Signature& s) { return s == seen[0]; });
        REQUIRE_FALSE(all_identical);
    }
}

TEST_CASE("S1: batch position does not change a result", "[seeding][S1]") {
    // Game 7 run seventh in a batch must equal game 7 run alone. This is what
    // makes the ablation sweep parallelisable by handing threads disjoint index
    // ranges, and what makes CRN couple two decks run on the same seeds.
    std::vector<Signature> batch;
    batch.reserve(kGames);
    for (std::uint64_t i = 0; i < kGames; ++i) {
        batch.push_back(sign(play(i)));
    }
    for (std::uint64_t i = 0; i < kGames; ++i) {
        REQUIRE(sign(play(i)) == batch[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("S1: reversed iteration order does not change a result", "[seeding][S1]") {
    // The case most likely to catch an accidental dependence on execution
    // order - a generator advanced once per game rather than seeded per index
    // would pass "same index twice" and fail here.
    std::vector<Signature> forward;
    for (std::uint64_t i = 0; i < kGames; ++i) {
        forward.push_back(sign(play(i)));
    }
    std::vector<Signature> backward(kGames);
    for (int i = kGames - 1; i >= 0; --i) {
        backward[static_cast<std::size_t>(i)] = sign(play(static_cast<std::uint64_t>(i)));
    }
    REQUIRE(forward == backward);
}

TEST_CASE("S1: thread count does not change a result", "[seeding][S1]") {
    std::vector<Signature> single;
    for (std::uint64_t i = 0; i < kGames; ++i) {
        single.push_back(sign(play(i)));
    }

    for (const int threads : {1, 4, 8}) {
        std::vector<Signature> shared(kGames);
        std::atomic<int> next{0};
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(threads));
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                for (int i = next.fetch_add(1); i < kGames; i = next.fetch_add(1)) {
                    // Deliberately NOT a contiguous split: work-stealing means a
                    // given game lands on a different thread each run, so a
                    // hidden per-thread dependency would show up as flakiness.
                    shared[static_cast<std::size_t>(i)] = sign(play(static_cast<std::uint64_t>(i)));
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        REQUIRE(shared == single);
    }
}

TEST_CASE("S1 still holds with the authored policy", "[seeding][S1][policy]") {
    // The scorer is where a tie broken by iteration order would hide.
    std::vector<Signature> single;
    for (std::uint64_t i = 0; i < kGames; ++i) {
        single.push_back(sign(play_authored(i)));
    }
    SECTION("same index, same game") {
        for (std::uint64_t i = 0; i < kGames; ++i) {
            REQUIRE(sign(play_authored(i)) == single[static_cast<std::size_t>(i)]);
        }
    }
    SECTION("reversed order changes nothing") {
        for (int i = kGames - 1; i >= 0; --i) {
            REQUIRE(sign(play_authored(static_cast<std::uint64_t>(i))) ==
                    single[static_cast<std::size_t>(i)]);
        }
    }
    SECTION("thread count changes nothing") {
        for (const int threads : {1, 4, 8}) {
            std::vector<Signature> shared(kGames);
            std::atomic<int> next{0};
            std::vector<std::thread> workers;
            for (int t = 0; t < threads; ++t) {
                workers.emplace_back([&] {
                    for (int i = next.fetch_add(1); i < kGames; i = next.fetch_add(1)) {
                        shared[static_cast<std::size_t>(i)] =
                            sign(play_authored(static_cast<std::uint64_t>(i)));
                    }
                });
            }
            for (std::thread& worker : workers) {
                worker.join();
            }
            REQUIRE(shared == single);
        }
    }
}

TEST_CASE("seed derivation is counter-based, not sequential", "[seeding][S1]") {
    SECTION("adjacent indices give uncorrelated streams") {
        // Xoring a small index straight into a base seed gives neighbouring
        // games states differing in a couple of bits. splitmix64 on the index
        // first is what prevents that, so this checks the mixing rather than
        // just the plumbing.
        cs::Rng a(cs::seed_for_game(kBaseSeed, 0));
        cs::Rng b(cs::seed_for_game(kBaseSeed, 1));
        int differing_bits = 0;
        for (int i = 0; i < 8; ++i) {
            differing_bits += std::popcount(a.next() ^ b.next());
        }
        // 512 bits compared; independent streams differ in ~256. Anything under
        // 150 means the streams are related.
        REQUIRE(differing_bits > 150);
    }
    SECTION("a different base seed gives a different game") {
        const cs::StubPolicyDoNotUseForResults policy;
        const auto one = cs::run_game(fixture(), simple_effects(), no_patterns(), cs::GameConfig{}, policy,
                                      cs::seed_for_game(1, 0));
        const auto two = cs::run_game(fixture(), simple_effects(), no_patterns(), cs::GameConfig{}, policy,
                                      cs::seed_for_game(2, 0));
        REQUIRE_FALSE(sign(one) == sign(two));
    }
}

TEST_CASE("below() terminates on power-of-two bounds", "[seeding]") {
    // The bound where the rejection limit is 2^64, which does not fit in 64
    // bits and wraps to zero. An unguarded rejection loop hangs here forever.
    // Every power of two up to the deck size, because a library drawn down to
    // 2, 4, 8 or 16 remaining is an ordinary thing for a long game to do.
    cs::Rng rng(999);
    for (const std::uint64_t bound : {1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U}) {
        for (int i = 0; i < 1000; ++i) {
            REQUIRE(rng.below(bound) < bound);
        }
    }
}

TEST_CASE("below() is unbiased", "[seeding]") {
    // `next() % bound` skews towards small values whenever bound does not
    // divide 2^64. With a 99-card library that would make low-indexed cards
    // measurably likelier to be drawn early - a bias with no symptom.
    cs::Rng rng(12345);
    constexpr std::uint64_t kBound = 99;
    constexpr int kDraws = 200000;
    std::vector<int> counts(kBound, 0);
    for (int i = 0; i < kDraws; ++i) {
        counts[static_cast<std::size_t>(rng.below(kBound))]++;
    }
    const auto [low, high] = std::minmax_element(counts.begin(), counts.end());
    const double expected = static_cast<double>(kDraws) / static_cast<double>(kBound);
    // Loose bounds: this catches gross skew, not a subtle failure. A chi-square
    // would be sharper; the modulo bias this exists to exclude is not subtle.
    REQUIRE(static_cast<double>(*low) > expected * 0.85);
    REQUIRE(static_cast<double>(*high) < expected * 1.15);
}

TEST_CASE("simulate_batch decomposes into disjoint ranges", "[seeding][S1][batch]") {
    // Section 7.4's whole justification: batching is where the parallel
    // decomposition lands, and a decomposition is only one if splitting the
    // range and merging the summaries reproduces the whole range exactly.
    //
    // The digest is the part worth asserting. Counts would agree between two
    // different sets of games that happened to end the same way - the seeding
    // tests above found exactly that, reporting two different seeds as the same
    // game because only the counters were compared. The digest cannot.
    const cs::StubPolicyDoNotUseForResults policy;
    cs::GameConfig config;

    const cs::RunSummary whole = cs::simulate_batch(fixture(), simple_effects(), no_patterns(),
                                                    config, policy, kBaseSeed, 0, 40);

    for (const int splits : {2, 3, 7}) {
        cs::RunSummary merged;
        int first = 0;
        for (int part = 0; part < splits; ++part) {
            const int count = (40 / splits) + (part < 40 % splits ? 1 : 0);
            cs::merge(merged, cs::simulate_batch(fixture(), simple_effects(), no_patterns(),
                                                 config, policy, kBaseSeed, first, count));
            first += count;
        }
        REQUIRE(first == 40);
        REQUIRE(merged.games == whole.games);
        REQUIRE(merged.censored == whole.censored);
        REQUIRE(merged.assembled_on == whole.assembled_on);
        REQUIRE(merged.can_pay_calls == whole.can_pay_calls);
        REQUIRE(merged.digest_xor == whole.digest_xor);
    }
}

TEST_CASE("a batch run in parallel equals the same batch run serially",
          "[seeding][S1][batch]") {
    // The claim section 7.3 makes about the ablation sweep, asserted rather
    // than assumed. Threads take disjoint index ranges; merge order varies with
    // scheduling and the answer does not, because merge() adds counts and XORs
    // digests.
    const cs::StubPolicyDoNotUseForResults policy;
    cs::GameConfig config;
    const cs::RunSummary serial = cs::simulate_batch(fixture(), simple_effects(), no_patterns(),
                                                     config, policy, kBaseSeed, 0, 64);

    for (const int threads : {2, 4, 8}) {
        std::vector<cs::RunSummary> parts(static_cast<std::size_t>(threads));
        std::vector<std::thread> workers;
        const int per = 64 / threads;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                parts[static_cast<std::size_t>(t)] =
                    cs::simulate_batch(fixture(), simple_effects(), no_patterns(), config, policy,
                                       kBaseSeed, t * per, per);
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        cs::RunSummary merged;
        for (const cs::RunSummary& part : parts) {
            cs::merge(merged, part);
        }
        REQUIRE(merged.games == serial.games);
        REQUIRE(merged.assembled_on == serial.assembled_on);
        REQUIRE(merged.digest_xor == serial.digest_xor);
    }
}

TEST_CASE("a paired run is coupled, and coupling is what CRN is", "[seeding][S1][crn]") {
    // Section 10.4's mechanism, asserted rather than assumed: game i is played
    // in BOTH arms with the same seed. When the two arms are the same deck, the
    // two games must be identical - every pair concordant, no discordant pairs
    // at any turn. If S1 ever broke, this is where it would show, and CRN's
    // failure mode is otherwise silent.
    cs::GameConfig config;
    cs::PolicyWeights weights;
    weights.rank.assign(fixture().cards.size(), 10);

    cs::AblatedDeck same;
    same.db = fixture();
    same.effects = simple_effects();
    same.weights = weights;
    same.patterns = no_patterns();

    const cs::PairedRun run = cs::run_paired(fixture(), simple_effects(), weights, same,
                                             no_patterns(), config, kBaseSeed, 0, 50);
    REQUIRE(run.games == 50);
    for (const cs::PairedCounts& cell : run.by_turn) {
        REQUIRE(cell.baseline_only == 0);
        REQUIRE(cell.ablated_only == 0);
    }
    // And the paired standard error of a zero difference is zero, which is the
    // limiting case of the variance reduction the sweep reports.
    const cs::Difference difference =
        cs::paired_difference(run.by_turn[static_cast<std::size_t>(config.turn_cap)]);
    REQUIRE(difference.delta == 0.0);
    REQUIRE(difference.standard_error == 0.0);
}

TEST_CASE("a paired run decomposes across threads like a batch does",
          "[seeding][S1][crn][batch]") {
    cs::GameConfig config;
    cs::PolicyWeights weights;
    weights.rank.assign(fixture().cards.size(), 10);
    const int forest = [] {
        for (const cs::Card& card : fixture().cards) {
            if (card.listed_name == "Forest") return card.export_index;
        }
        return -1;
    }();
    const int ring = [] {
        for (const cs::Card& card : fixture().cards) {
            if (card.listed_name == "Sol Ring") return card.export_index;
        }
        return -1;
    }();
    const cs::AblatedDeck arm =
        cs::ablate(fixture(), simple_effects(), weights, no_patterns(), ring, forest);

    const cs::PairedRun whole = cs::run_paired(fixture(), simple_effects(), weights, arm,
                                               no_patterns(), config, kBaseSeed, 0, 60);
    for (const int threads : {2, 4, 8}) {
        std::vector<cs::PairedRun> parts(static_cast<std::size_t>(threads));
        std::vector<std::thread> workers;
        // The remainder is spread over the first workers, exactly as the CLI
        // driver does it. Dividing 60 by 8 and multiplying back drops four
        // games - which this test did on its first run, and which is the same
        // arithmetic slip that would silently shorten a real sweep.
        int first = 0;
        for (int t = 0; t < threads; ++t) {
            const int count = 60 / threads + (t < 60 % threads ? 1 : 0);
            workers.emplace_back([&, t, first, count] {
                parts[static_cast<std::size_t>(t)] =
                    cs::run_paired(fixture(), simple_effects(), weights, arm, no_patterns(),
                                   config, kBaseSeed, first, count);
            });
            first += count;
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        cs::PairedRun merged;
        for (const cs::PairedRun& part : parts) {
            cs::merge(merged, part);
        }
        REQUIRE(first == 60);
        REQUIRE(merged.games == whole.games);
        for (std::size_t turn = 0; turn < whole.by_turn.size(); ++turn) {
            REQUIRE(merged.by_turn[turn].both == whole.by_turn[turn].both);
            REQUIRE(merged.by_turn[turn].baseline_only == whole.by_turn[turn].baseline_only);
            REQUIRE(merged.by_turn[turn].ablated_only == whole.by_turn[turn].ablated_only);
            REQUIRE(merged.by_turn[turn].neither == whole.by_turn[turn].neither);
        }
    }
}

TEST_CASE("a specified opening hand is dealt exactly, and S1 still holds",
          "[seeding][S1][hand]") {
    // SIM_PLAN.md section 1 says the interface takes a SPECIFIC opening hand.
    // Section 7.1 wrote the signature; nothing implemented it for seven phases,
    // because the only caller that needs it is the mulligan solver. Section 14
    // item 4 read as done throughout.
    cs::Rng rng(kBaseSeed);
    const cs::Zone hand = cs::sample_hand(static_cast<int>(fixture().cards.size()), -1, 3, rng);
    REQUIRE(hand.count() == 3);

    SECTION("the hand is exact, not conditioned-on") {
        cs::GameState state;
        cs::Rng fresh(kBaseSeed);
        cs::begin_game_with_hand(state, static_cast<int>(fixture().cards.size()), -1, hand, fresh);
        REQUIRE(state.hand.count() == 3);
        hand.for_each([&](int slot) { REQUIRE(state.hand.test(slot)); });
        // And its cards are NOT still in the library, or they could be drawn twice.
        for (std::size_t i = 0; i < state.library_count; ++i) {
            REQUIRE_FALSE(hand.test(state.library[i]));
        }
        REQUIRE(state.library_count == fixture().cards.size() - 3);
    }
    SECTION("game i is still a pure function of (hand, seed, i)") {
        const cs::StubPolicyDoNotUseForResults policy;
        cs::GameConfig config;
        const cs::RunSummary once =
            cs::simulate_batch(fixture(), simple_effects(), no_patterns(), config, policy,
                               kBaseSeed, 0, 20, &hand);
        const cs::RunSummary twice =
            cs::simulate_batch(fixture(), simple_effects(), no_patterns(), config, policy,
                               kBaseSeed, 0, 20, &hand);
        REQUIRE(once.digest_xor == twice.digest_xor);
        // And a DIFFERENT hand gives a different game, so the parameter is read.
        cs::Rng other_rng(kBaseSeed ^ 0xFFFF);
        const cs::Zone other =
            cs::sample_hand(static_cast<int>(fixture().cards.size()), -1, 3, other_rng);
        if (other.count() == 3) {
            const cs::RunSummary elsewhere =
                cs::simulate_batch(fixture(), simple_effects(), no_patterns(), config, policy,
                                   kBaseSeed, 0, 20, &other);
            REQUIRE(elsewhere.digest_xor != once.digest_xor);
        }
    }
}

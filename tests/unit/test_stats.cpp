// The statistics (SIM_PLAN.md section 10).
//
// Testable without running a game, which is the point of RunSummary being
// counts rather than derived numbers: there is one place a reported figure can
// be wrong and it takes no simulation to interrogate.

#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/stats.hpp"

using Catch::Approx;

namespace {

// The normal approximation, written down ONLY so a test can show what it does
// at the extremes. Section 11.0's eighth rule upstream: write the wrong
// implementation and assert the test notices. It is never called by the project.
cs::Interval normal_approximation(int successes, int trials) noexcept {
    const auto n = static_cast<double>(trials);
    const double p = static_cast<double>(successes) / n;
    const double half = cs::kZ95 * std::sqrt(p * (1.0 - p) / n);
    return cs::Interval{p - half, p + half};
}

cs::RunSummary summary_from(const std::vector<int>& by_turn, int censored) {
    cs::RunSummary summary;
    summary.assembled_on.assign(by_turn.size() + 1, 0);
    for (std::size_t i = 0; i < by_turn.size(); ++i) {
        summary.assembled_on[i + 1] = by_turn[i];
        summary.games += by_turn[i];
    }
    summary.games += censored;
    summary.censored = censored;
    return summary;
}

}  // namespace

TEST_CASE("Wilson stays inside [0,1] where the normal approximation does not", "[stats]") {
    // THE reason section 10.2 specifies Wilson. p is near zero exactly at turn 2
    // and turn 3, which are the rows this deck is read for, and the normal
    // approximation returns a lower bound below zero there - a negative
    // probability, printed as a measurement.
    const cs::Interval wrong = normal_approximation(3, 20000);
    REQUIRE(wrong.low < 0.0);

    const cs::Interval right = cs::wilson(3, 20000);
    REQUIRE(right.low > 0.0);
    REQUIRE(right.low < right.high);
    REQUIRE(right.high < 1.0);
}

TEST_CASE("Wilson at the boundaries", "[stats]") {
    SECTION("zero successes gives a one-sided interval that starts at zero") {
        const cs::Interval interval = cs::wilson(0, 1000);
        // margin, not Approx's default relative epsilon: the algebra cancels to
        // exactly zero and floating point lands a few 1e-19 either side, which
        // a relative comparison against zero can never accept.
        REQUIRE(interval.low == Approx(0.0).margin(1e-12));
        REQUIRE(interval.high > 0.0);
        REQUIRE(interval.high < 0.01);
    }
    SECTION("every success gives one that ends at one") {
        const cs::Interval interval = cs::wilson(1000, 1000);
        REQUIRE(interval.high == Approx(1.0).margin(1e-12));
        REQUIRE(interval.low < 1.0);
        REQUIRE(interval.low > 0.99);
    }
    SECTION("no trials is no information, not a point estimate") {
        const cs::Interval interval = cs::wilson(0, 0);
        REQUIRE(interval.low == Approx(0.0).margin(1e-12));
        REQUIRE(interval.high == Approx(1.0).margin(1e-12));
    }
    SECTION("a textbook value") {
        // 10 of 100: Wilson 95% is approximately [0.0554, 0.1739].
        const cs::Interval interval = cs::wilson(10, 100);
        REQUIRE(interval.low == Approx(0.05545).margin(0.0005));
        REQUIRE(interval.high == Approx(0.17395).margin(0.0005));
    }
}

TEST_CASE("the binomial CDF matches values computed by hand", "[stats]") {
    // P(Bin(10, 0.5) <= 5) = 638/1024.
    REQUIRE(cs::binomial_cdf(5, 10, 0.5) == Approx(638.0 / 1024.0).margin(1e-9));
    // P(Bin(10, 0.5) <= 0) = 1/1024.
    REQUIRE(cs::binomial_cdf(0, 10, 0.5) == Approx(1.0 / 1024.0).margin(1e-12));
    // P(Bin(20, 0.25) <= 4) = 0.414843...
    REQUIRE(cs::binomial_cdf(4, 20, 0.25) == Approx(0.4148415).margin(1e-6));
    REQUIRE(cs::binomial_cdf(-1, 10, 0.5) == Approx(0.0));
    REQUIRE(cs::binomial_cdf(10, 10, 0.5) == Approx(1.0));
}

TEST_CASE("a percentile past the censored fraction does not exist", "[stats][censoring]") {
    // Section 10.3, and the rule that matters most in this file. 40 of 100
    // games never assembled, so P75 is not in the data - three quarters of the
    // games would have to have a value and only 60% do.
    //
    // Computing it anyway over the 60 winners gives turn 6, which is a
    // plausible-looking number, is what a naive implementation returns, and is
    // wrong in the OPTIMISTIC direction.
    const cs::RunSummary run = summary_from({0, 5, 10, 15, 20, 10}, 40);
    REQUIRE(run.games == 100);

    SECTION("P50 exists: half the games assembled") {
        const cs::Percentile p50 = cs::percentile(run, 0.50);
        REQUIRE(p50.turn.has_value());
        // Cumulative by turn: 0, 5, 15, 30, 50, 60. The 50th game assembles on
        // turn 5, which is the smallest turn whose cumulative reaches 50.
        REQUIRE(*p50.turn == 5);
    }
    SECTION("P75 does not, and says so rather than returning turn 6") {
        const cs::Percentile p75 = cs::percentile(run, 0.75);
        REQUIRE_FALSE(p75.turn.has_value());
        REQUIRE_FALSE(p75.low.has_value());
        REQUIRE_FALSE(p75.high.has_value());
    }
    SECTION("exactly at the boundary the percentile still exists") {
        // 40% censored and p = 0.60: censored is not GREATER than 1 - p.
        const cs::Percentile p60 = cs::percentile(run, 0.60);
        REQUIRE(p60.turn.has_value());
    }
}

TEST_CASE("percentile bounds bracket the point estimate and tighten with n", "[stats]") {
    const cs::RunSummary small = summary_from({0, 10, 20, 30, 25, 15}, 0);
    const cs::Percentile p50_small = cs::percentile(small, 0.50);
    REQUIRE(p50_small.turn.has_value());
    REQUIRE(p50_small.low.has_value());
    REQUIRE(p50_small.high.has_value());
    REQUIRE(*p50_small.low <= *p50_small.turn);
    REQUIRE(*p50_small.turn <= *p50_small.high);

    // The same shape 100x larger: the rank bounds move proportionally less, so
    // the interval cannot be wider in turns.
    cs::RunSummary large = summary_from({0, 1000, 2000, 3000, 2500, 1500}, 0);
    const cs::Percentile p50_large = cs::percentile(large, 0.50);
    REQUIRE(*p50_large.high - *p50_large.low <= *p50_small.high - *p50_small.low);
}

TEST_CASE("a censored upper bound is reported as censored, not clipped to the cap",
          "[stats][censoring]") {
    // P50 exists here - 55% assembled - but its UPPER rank lands past the last
    // uncensored game. Clipping that to turn 5 would report a bound the data
    // does not support, and in the flattering direction.
    const cs::RunSummary run = summary_from({0, 0, 0, 0, 55}, 45);
    const cs::Percentile p50 = cs::percentile(run, 0.50);
    REQUIRE(p50.turn.has_value());
    REQUIRE(*p50.turn == 5);
    REQUIRE(p50.low.has_value());
    REQUIRE_FALSE(p50.high.has_value());
}

TEST_CASE("merging summaries is addition, and order-independent", "[stats][S1]") {
    // What makes the parallel driver a decomposition rather than a second code
    // path: two disjoint ranges merged in either order equal the whole range.
    cs::RunSummary a = summary_from({1, 2, 3}, 4);
    cs::RunSummary b = summary_from({5, 6, 7}, 8);
    a.digest_xor = 0xAAAA;
    b.digest_xor = 0x5555;

    cs::RunSummary forwards = a;
    cs::merge(forwards, b);
    cs::RunSummary backwards = b;
    cs::merge(backwards, a);

    REQUIRE(forwards.games == backwards.games);
    REQUIRE(forwards.games == a.games + b.games);
    REQUIRE(forwards.censored == backwards.censored);
    REQUIRE(forwards.assembled_on == backwards.assembled_on);
    // XOR rather than a running hash, precisely so this holds.
    REQUIRE(forwards.digest_xor == backwards.digest_xor);
    REQUIRE(forwards.digest_xor == (0xAAAAULL ^ 0x5555ULL));
}

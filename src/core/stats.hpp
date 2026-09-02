#pragma once

// Turning counts into reportable numbers.
//
// Everything the CLI prints is a function of a RunSummary, so there is exactly
// one place a reported number can be wrong, and it is testable without running
// a game (SIM_PLAN.md section 10).
//
// Standard library only, like the rest of core - <cmath> and nothing else.

#include <cstdint>
#include <optional>
#include <vector>

namespace cs {

// A two-sided interval on a proportion. Both ends are in [0, 1].
struct Interval {
    double low = 0.0;
    double high = 0.0;
};

// 95% by default. Named so a call site reads as the confidence it asks for
// rather than as a number someone has to recognise.
inline constexpr double kZ95 = 1.959963984540054;

// WILSON SCORE, not the normal approximation (section 10.2).
//
// The normal approximation fails where this project cares most: at turn 2 and
// turn 3, p is near zero, and the approximation returns intervals that extend
// below zero. Wilson is a closed form, costs nothing, and is correct at the
// extremes. `trials == 0` gives [0, 1] - no information, stated as no
// information rather than as a point.
[[nodiscard]] Interval wilson(int successes, int trials, double z = kZ95) noexcept;

// P(Binomial(n, p) <= k), via the regularized incomplete beta.
//
// Exists for the percentile intervals below, which invert it. Exact rather than
// approximate, because a percentile of turn-to-assembly is an integer over a
// twelve-point support and a normal approximation there is meaningless.
[[nodiscard]] double binomial_cdf(int k, int n, double p) noexcept;

// A percentile of turn-to-assembly.
//
// CENSORING IS A FIRST-CLASS OUTCOME (section 10.3). If more than (1 - p) of
// games never assembled, the p-th percentile DOES NOT EXIST, and this says so
// rather than computing one over the winners - which would be a lie that looks
// like data and biases optimistically, the direction nobody catches.
//
// nullopt means censored, at either the point estimate or an interval end.
struct Percentile {
    double p = 0.5;
    std::optional<int> turn;   // nullopt: does not exist, too much censoring
    std::optional<int> low;    // order-statistic bounds, in TURNS
    std::optional<int> high;
};

// Everything one run of N games produced.
//
// Counts, not derived numbers. Two summaries merge by adding, which is what
// makes the parallel driver a decomposition rather than a second code path.
struct RunSummary {
    int games = 0;
    int censored = 0;

    // Index is the TURN, so [0] is unused and the vector is turn_cap + 1 long.
    // Games that assembled ON that turn, not cumulatively.
    std::vector<int> assembled_on;

    // Index is the pattern id. `fired` is the pattern that WON; `satisfied` is
    // every pattern whose state also held on the assembling turn (section 5.3).
    std::vector<int> fired;
    std::vector<int> satisfied;

    std::uint64_t can_pay_calls = 0;
    std::uint64_t turns_total = 0;
    std::uint64_t cards_drawn = 0;
    std::uint64_t cards_drawn_by_effect = 0;
    std::uint64_t spells_cast = 0;
    std::uint64_t tutors_used = 0;
    std::uint64_t clones_made = 0;
    std::uint64_t convoked = 0;

    // XOR of every game's state digest. Two runs that played the same games
    // agree here; two that did not, do not. Order-independent on purpose, so a
    // parallel run and a serial one produce the same value.
    std::uint64_t digest_xor = 0;
};

// Adds `other` into `into`. Requires the same turn_cap and pattern count.
void merge(RunSummary& into, const RunSummary& other);

// Games that assembled on or before `turn`.
[[nodiscard]] int cumulative(const RunSummary& summary, int turn) noexcept;

// The p-th percentile of turn-to-assembly, with order-statistic bounds.
//
// The bounds are the standard nonparametric construction: the number of
// observations at or below the true p-quantile is Binomial(n, p), so inverting
// that CDF gives a pair of RANKS, and each rank maps back to a turn through the
// cumulative histogram. A rank past the last uncensored game has no turn, which
// is how a one-sided answer ("median is 9, upper bound censored") stays honest
// instead of being clipped to the cap.
[[nodiscard]] Percentile percentile(const RunSummary& summary, double p,
                                    double alpha = 0.05) noexcept;

}  // namespace cs

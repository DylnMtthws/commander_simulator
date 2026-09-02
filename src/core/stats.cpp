#include "core/stats.hpp"

#include <cmath>

namespace cs {
namespace {

// Regularized incomplete beta I_x(a, b), by the standard continued fraction
// (Lentz's method) with the symmetry transform that keeps it converging.
//
// This is the only special function in the project, and it is here so that the
// percentile intervals are EXACT rather than approximated. A percentile of
// turn-to-assembly has a twelve-point integer support; a normal approximation
// over that is not an approximation of anything.
double beta_continued_fraction(double a, double b, double x) noexcept {
    constexpr int kMaxIterations = 300;
    constexpr double kEpsilon = 3.0e-16;
    constexpr double kTiny = 1.0e-300;

    const double qab = a + b;
    const double qap = a + 1.0;
    const double qam = a - 1.0;
    double c = 1.0;
    double d = 1.0 - qab * x / qap;
    if (std::fabs(d) < kTiny) {
        d = kTiny;
    }
    d = 1.0 / d;
    double h = d;

    for (int m = 1; m <= kMaxIterations; ++m) {
        const auto md = static_cast<double>(m);
        const double m2 = 2.0 * md;

        double numerator = md * (b - md) * x / ((qam + m2) * (a + m2));
        d = 1.0 + numerator * d;
        if (std::fabs(d) < kTiny) {
            d = kTiny;
        }
        c = 1.0 + numerator / c;
        if (std::fabs(c) < kTiny) {
            c = kTiny;
        }
        d = 1.0 / d;
        h *= d * c;

        numerator = -(a + md) * (qab + md) * x / ((a + m2) * (qap + m2));
        d = 1.0 + numerator * d;
        if (std::fabs(d) < kTiny) {
            d = kTiny;
        }
        c = 1.0 + numerator / c;
        if (std::fabs(c) < kTiny) {
            c = kTiny;
        }
        d = 1.0 / d;
        const double delta = d * c;
        h *= delta;

        if (std::fabs(delta - 1.0) < kEpsilon) {
            break;
        }
    }
    return h;
}

double incomplete_beta(double a, double b, double x) noexcept {
    if (x <= 0.0) {
        return 0.0;
    }
    if (x >= 1.0) {
        return 1.0;
    }
    const double front = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
                                  a * std::log(x) + b * std::log1p(-x));
    // The continued fraction converges quickly only on one side of the mean;
    // the symmetry I_x(a,b) = 1 - I_{1-x}(b,a) covers the other.
    if (x < (a + 1.0) / (a + b + 2.0)) {
        return front * beta_continued_fraction(a, b, x) / a;
    }
    return 1.0 - std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
                          b * std::log1p(-x) + a * std::log(x)) *
                     beta_continued_fraction(b, a, 1.0 - x) / b;
}

}  // namespace

Interval wilson(int successes, int trials, double z) noexcept {
    if (trials <= 0) {
        // No games, no information. Saying [0, 1] is saying that; saying 0
        // would be reporting a measurement that was never taken.
        return Interval{0.0, 1.0};
    }
    const auto n = static_cast<double>(trials);
    const double p = static_cast<double>(successes) / n;
    const double z2 = z * z;
    const double denominator = 1.0 + z2 / n;
    const double centre = (p + z2 / (2.0 * n)) / denominator;
    const double spread = z * std::sqrt(p * (1.0 - p) / n + z2 / (4.0 * n * n)) / denominator;
    double low = centre - spread;
    double high = centre + spread;
    // Wilson cannot leave [0,1] mathematically; clamp only against rounding.
    low = low < 0.0 ? 0.0 : low;
    high = high > 1.0 ? 1.0 : high;
    return Interval{low, high};
}

double binomial_cdf(int k, int n, double p) noexcept {
    if (k < 0) {
        return 0.0;
    }
    if (k >= n) {
        return 1.0;
    }
    // P(X <= k) = I_{1-p}(n-k, k+1). The identity, not a sum: at n = 50,000 a
    // sum of pmf terms both costs more and loses precision in the tail, which
    // is the only part a percentile bound ever reads.
    return incomplete_beta(static_cast<double>(n - k), static_cast<double>(k) + 1.0, 1.0 - p);
}

void merge(RunSummary& into, const RunSummary& other) {
    into.games += other.games;
    into.censored += other.censored;
    if (into.assembled_on.size() < other.assembled_on.size()) {
        into.assembled_on.resize(other.assembled_on.size(), 0);
    }
    for (std::size_t i = 0; i < other.assembled_on.size(); ++i) {
        into.assembled_on[i] += other.assembled_on[i];
    }
    if (into.fired.size() < other.fired.size()) {
        into.fired.resize(other.fired.size(), 0);
        into.satisfied.resize(other.satisfied.size(), 0);
    }
    for (std::size_t i = 0; i < other.fired.size(); ++i) {
        into.fired[i] += other.fired[i];
        into.satisfied[i] += other.satisfied[i];
    }
    into.can_pay_calls += other.can_pay_calls;
    into.turns_total += other.turns_total;
    into.cards_drawn += other.cards_drawn;
    into.cards_drawn_by_effect += other.cards_drawn_by_effect;
    into.spells_cast += other.spells_cast;
    into.tutors_used += other.tutors_used;
    into.clones_made += other.clones_made;
    into.convoked += other.convoked;
    into.selects_used += other.selects_used;
    into.selects_hit += other.selects_hit;
    // XOR, so a parallel run and a serial one agree regardless of merge order.
    into.digest_xor ^= other.digest_xor;
}

int cumulative(const RunSummary& summary, int turn) noexcept {
    int total = 0;
    for (int i = 1; i <= turn && static_cast<std::size_t>(i) < summary.assembled_on.size(); ++i) {
        total += summary.assembled_on[static_cast<std::size_t>(i)];
    }
    return total;
}

namespace {

// The turn of the r-th smallest assembly time, 1-indexed. nullopt when r falls
// past the last uncensored game - which is exactly the censored case, and the
// reason this returns an optional rather than the turn cap.
std::optional<int> turn_at_rank(const RunSummary& summary, int rank) noexcept {
    if (rank < 1) {
        return std::nullopt;
    }
    int seen = 0;
    for (std::size_t turn = 1; turn < summary.assembled_on.size(); ++turn) {
        seen += summary.assembled_on[turn];
        if (seen >= rank) {
            return static_cast<int>(turn);
        }
    }
    return std::nullopt;  // this rank is a censored game
}

}  // namespace

Percentile percentile(const RunSummary& summary, double p, double alpha) noexcept {
    Percentile result;
    result.p = p;
    const int n = summary.games;
    if (n <= 0) {
        return result;
    }

    // Section 10.3's rule, applied before anything is computed: if the censored
    // fraction exceeds 1 - p, the p-th percentile is not in the data at all.
    // Everything below would still produce a number; that number would be a
    // percentile of the WINNERS, which biases optimistically.
    const double censored_fraction = static_cast<double>(summary.censored) / n;
    if (censored_fraction > 1.0 - p) {
        return result;  // every field stays nullopt
    }

    // Point estimate: the ceil(np)-th order statistic.
    const auto point_rank = static_cast<int>(std::ceil(p * n));
    result.turn = turn_at_rank(summary, point_rank < 1 ? 1 : point_rank);

    // Bounds by inverting Binomial(n, p): the count of observations at or below
    // the true quantile is binomial, so the ranks that bracket it at 1 - alpha
    // are read straight off that CDF. Conservative by construction, which is
    // the right direction for an interval.
    int lower_rank = 0;
    while (lower_rank < n && binomial_cdf(lower_rank, n, p) <= alpha / 2.0) {
        ++lower_rank;
    }
    int upper_rank = lower_rank;
    while (upper_rank < n && binomial_cdf(upper_rank, n, p) < 1.0 - alpha / 2.0) {
        ++upper_rank;
    }
    result.low = turn_at_rank(summary, lower_rank < 1 ? 1 : lower_rank);
    result.high = turn_at_rank(summary, upper_rank < 1 ? 1 : upper_rank);
    return result;
}

}  // namespace cs

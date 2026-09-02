#include "core/sweep.hpp"

#include <cmath>

namespace cs {

void merge(PairedRun& into, const PairedRun& other) {
    if (into.by_turn.size() < other.by_turn.size()) {
        into.by_turn.resize(other.by_turn.size());
    }
    for (std::size_t i = 0; i < other.by_turn.size(); ++i) {
        into.by_turn[i].both += other.by_turn[i].both;
        into.by_turn[i].baseline_only += other.by_turn[i].baseline_only;
        into.by_turn[i].ablated_only += other.by_turn[i].ablated_only;
        into.by_turn[i].neither += other.by_turn[i].neither;
    }
    into.games += other.games;
    into.baseline_assembled_ever += other.baseline_assembled_ever;
    into.ablated_assembled_ever += other.ablated_assembled_ever;
}

PairedRun run_paired(const CardDb& baseline_db, const EffectDb& baseline_effects,
                     const PolicyWeights& baseline_weights, const AblatedDeck& ablated,
                     const PatternSet& patterns, const GameConfig& config,
                     std::uint64_t base_seed, int first_game, int games) {
    PairedRun run;
    run.by_turn.assign(static_cast<std::size_t>(config.turn_cap) + 1, PairedCounts{});

    const AuthoredPolicy baseline_policy(baseline_weights);
    const AuthoredPolicy ablated_policy(ablated.weights);

    for (int i = 0; i < games; ++i) {
        const auto index = static_cast<std::uint64_t>(first_game + i);
        // THE SAME SEED IN BOTH ARMS. This one line is common random numbers.
        const std::uint64_t seed = seed_for_game(base_seed, index);

        const GameResult a =
            run_game(baseline_db, baseline_effects, patterns, config, baseline_policy, seed);
        // The ABLATED arm's own patterns, not the baseline's: a requirement
        // naming the ablated card has to be unsatisfiable rather than rebound
        // to whatever took the slot (core/ablation.hpp).
        const GameResult b = run_game(ablated.db, ablated.effects, ablated.patterns, config,
                                      ablated_policy, seed);

        ++run.games;
        run.baseline_assembled_ever += a.outcome.censored() ? 0 : 1;
        run.ablated_assembled_ever += b.outcome.censored() ? 0 : 1;

        for (int turn = 1; turn <= config.turn_cap; ++turn) {
            const bool a_by = !a.outcome.censored() && *a.outcome.assembled_turn <= turn;
            const bool b_by = !b.outcome.censored() && *b.outcome.assembled_turn <= turn;
            PairedCounts& cell = run.by_turn[static_cast<std::size_t>(turn)];
            if (a_by && b_by) {
                ++cell.both;
            } else if (a_by) {
                ++cell.baseline_only;
            } else if (b_by) {
                ++cell.ablated_only;
            } else {
                ++cell.neither;
            }
        }
    }
    return run;
}

Difference paired_difference(const PairedCounts& counts, double z) {
    Difference result;
    const int n = counts.games();
    if (n <= 0) {
        result.low = -1.0;
        result.high = 1.0;
        return result;
    }
    const auto total = static_cast<double>(n);
    const auto b = static_cast<double>(counts.baseline_only);
    const auto c = static_cast<double>(counts.ablated_only);
    result.delta = (b - c) / total;
    // Var(d) = (b + c - (b - c)^2 / n) / n^2. The discordant pairs are the only
    // term: a game that came out the same in both arms tells you nothing about
    // the difference, which is exactly why pairing helps.
    const double variance = (b + c - (b - c) * (b - c) / total) / (total * total);
    result.standard_error = variance > 0.0 ? std::sqrt(variance) : 0.0;
    result.low = result.delta - z * result.standard_error;
    result.high = result.delta + z * result.standard_error;
    return result;
}

Difference unpaired_difference(int baseline_successes, int baseline_games, int ablated_successes,
                               int ablated_games, double z) {
    Difference result;
    if (baseline_games <= 0 || ablated_games <= 0) {
        result.low = -1.0;
        result.high = 1.0;
        return result;
    }
    const auto n1 = static_cast<double>(baseline_games);
    const auto n2 = static_cast<double>(ablated_games);
    const double p1 = static_cast<double>(baseline_successes) / n1;
    const double p2 = static_cast<double>(ablated_successes) / n2;
    result.delta = p1 - p2;
    result.standard_error = std::sqrt(p1 * (1.0 - p1) / n1 + p2 * (1.0 - p2) / n2);
    result.low = result.delta - z * result.standard_error;
    result.high = result.delta + z * result.standard_error;
    return result;
}

}  // namespace cs

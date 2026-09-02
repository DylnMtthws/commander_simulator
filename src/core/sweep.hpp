#pragma once

// Comparing two decks on the same games (SIM_PLAN.md section 10.4).
//
// COMMON RANDOM NUMBERS IS THE WHOLE POINT. Game i is played in both arms with
// the same seed, so the two runs are positively correlated and the variance of
// their DIFFERENCE collapses. It costs nothing at runtime and section 10.4
// claims it is worth more than every optimisation in section 11 combined - a
// claim this file exists to measure rather than repeat, since 10.4 also says
// "do not assume the 10x".
//
// CRN is available only because seeding is a pure function of game index
// (INVARIANT S1, section 7.3). S1 does not fail loudly when it breaks; variance
// reduction just quietly stops. That is why it is a tested invariant.

#include <cstdint>
#include <vector>

#include "core/ablation.hpp"
#include "core/sim.hpp"
#include "core/stats.hpp"

namespace cs {

// The 2x2 table for one objective turn, matched by game index.
//
// Counts of PAIRS, not of games in each arm separately, and that is the
// difference that makes CRN work: `both` and `neither` are games where the
// ablation changed nothing, and they carry no information about the effect.
// Only the discordant pairs do.
struct PairedCounts {
    int both = 0;           // assembled by turn N in both arms
    int baseline_only = 0;  // b: baseline assembled, ablated did not
    int ablated_only = 0;   // c: ablated assembled, baseline did not
    int neither = 0;

    [[nodiscard]] int games() const noexcept {
        return both + baseline_only + ablated_only + neither;
    }
    [[nodiscard]] int discordant() const noexcept { return baseline_only + ablated_only; }
};

// One paired run: a 2x2 table per turn, so any objective turn can be read off
// afterwards without re-running. Index is the turn; [0] is unused.
struct PairedRun {
    std::vector<PairedCounts> by_turn;
    int games = 0;
    // Kept so an unpaired interval can be formed from the same run.
    int baseline_assembled_ever = 0;
    int ablated_assembled_ever = 0;
};

void merge(PairedRun& into, const PairedRun& other);

// Plays games [first_game, first_game + games) in BOTH arms.
//
// Single-threaded and pure: indexed by game, never by thread, so a caller may
// split the range across threads and merge without changing the answer. The
// threading lives in cli/ because it is orchestration and core is sealed.
[[nodiscard]] PairedRun run_paired(const CardDb& baseline_db, const EffectDb& baseline_effects,
                                   const PolicyWeights& baseline_weights,
                                   const AblatedDeck& ablated, const PatternSet& patterns,
                                   const GameConfig& config, std::uint64_t base_seed,
                                   int first_game, int games);

// An effect size and its interval, in PROPORTION units.
//
// Named for what it is at every call site (section 9.5, rule 2). A column called
// `score` is a card-quality ranking no matter what a header said 400 rows
// earlier; one called goldfish_turn_to_assembly_delta resists that reading on
// its own.
struct Difference {
    double delta = 0.0;  // baseline - ablated: how much the card is worth
    double low = 0.0;
    double high = 0.0;
    double standard_error = 0.0;
};

// The PAIRED difference, from the discordant pairs only.
//
// delta = (b - c) / n, Var = (b + c - (b - c)^2 / n) / n^2. This is the Wald
// interval for a paired proportion difference, and the choice is deliberate
// rather than an oversight of section 10.2's insistence on Wilson:
//
//   Wilson exists because the normal approximation fails at p near 0 or 1. The
//   quantity here is a DIFFERENCE near 0, which is not that regime. Its
//   paired score analogue (Tango's interval) needs a constrained MLE solved
//   numerically, and a hand-rolled version of it would be a larger risk than a
//   well-understood Wald interval on thousands of discordant pairs.
//
// The report prints b and c so a reader can see whether the asymptotics hold
// rather than being asked to trust that they do.
[[nodiscard]] Difference paired_difference(const PairedCounts& counts, double z = kZ95);

// The UNPAIRED difference, for two independent runs. Exists to be compared
// against the paired one: the ratio of the two standard errors is the realised
// variance reduction, which section 10.4 requires be measured and not assumed.
[[nodiscard]] Difference unpaired_difference(int baseline_successes, int baseline_games,
                                             int ablated_successes, int ablated_games,
                                             double z = kZ95);

}  // namespace cs

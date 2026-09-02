#pragma once

// Deterministic randomness.
//
// INVARIANT S1 (SIM_PLAN.md section 7.3): the outcome of game i is a pure
// function of (deck, hand, base_seed, i). Not of thread count, not of
// scheduling, not of how many games ran before it.
//
// This is not tidiness. Common random numbers (section 10.4) is the single
// highest-value decision in the statistical design, and it depends entirely on
// S1. When S1 breaks, CRN does not error - it quietly stops reducing variance,
// and every ablation interval silently needs ten times the games. So the seed
// for a game is derived from its INDEX, never from a counter that advances as
// games are run.

#include <array>
#include <cstdint>

namespace cs {

// SplitMix64. Used only to turn one integer into well-distributed state; it is
// not the generator. Exposed because seed derivation must be reproducible and
// therefore testable.
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
    std::uint64_t z = x + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// The seed for game `index` of a run started with `base_seed`.
//
// Counter-based: a pure function of the two arguments. Game 7 has the same seed
// whether it is run alone, seventh in a batch, or on a different thread from
// games 0..6 - which is what makes the ablation sweep parallelise trivially and
// what makes CRN work at all.
//
// splitmix64 is applied to the index BEFORE the xor so that adjacent indices do
// not produce correlated streams; xoring a small index straight into a seed
// gives neighbouring games states differing in a couple of bits.
[[nodiscard]] constexpr std::uint64_t seed_for_game(std::uint64_t base_seed,
                                                    std::uint64_t index) noexcept {
    return splitmix64(base_seed ^ splitmix64(index));
}

// xoshiro256++. Fast, small state, good statistical quality, and - the property
// that matters here - fully specified, so the same seed gives the same stream
// on any platform and any compiler. std::mt19937 would also be reproducible but
// carries 2.5 KB of state, which is the opposite of what a 200-byte game state
// wants (section 11).
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept {
        // Seeding every word from splitmix64 rather than filling with the seed:
        // xoshiro is documented to need a well-distributed initial state, and
        // an all-but-one-zero state produces a visibly poor early stream.
        std::uint64_t x = seed;
        for (std::uint64_t& word : state_) {
            x = splitmix64(x);
            word = x;
        }
    }

    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(state_[0] + state_[3], 23) + state_[0];
        const std::uint64_t t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);
        return result;
    }

    // Uniform in [0, bound). Rejection-sampled, so it is genuinely unbiased
    // rather than `next() % bound` - which skews towards small values whenever
    // bound does not divide 2^64. With a 99-card library that skew would make
    // the top of the deck measurably likelier to be a low-index card, and the
    // whole point of this file is that nothing is measurably anything by
    // accident.
    [[nodiscard]] constexpr std::uint64_t below(std::uint64_t bound) noexcept {
        if (bound <= 1) {
            return 0;
        }
        // (0 - bound) % bound is 2^64 mod bound: the size of the short tail at
        // the top of the range that would bias the result.
        const std::uint64_t remainder = (std::uint64_t{0} - bound) % bound;
        std::uint64_t value = next();
        if (remainder != 0) {
            // The largest multiple of bound that fits in 64 bits. Guarded
            // because when bound is a POWER OF TWO the remainder is 0 and the
            // true limit is 2^64, which is unrepresentable and wraps to 0 -
            // making `value >= limit` always true and the loop infinite.
            //
            // Found by the first run of the seeding tests: an 8-card fixture
            // draws its library down to exactly 2 remaining, so bound == 2 on
            // the opening hand. A 99-card deck reaches it only after ~97
            // draws, which is rare, reachable, and would have hung a
            // long-running sweep with no diagnostic at all.
            const std::uint64_t limit = std::uint64_t{0} - remainder;
            while (value >= limit) {
                value = next();
            }
        }
        return value % bound;
    }

private:
    [[nodiscard]] static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }

    std::array<std::uint64_t, 4> state_{};
};

}  // namespace cs

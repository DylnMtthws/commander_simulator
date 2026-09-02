#pragma once

// Game state: zones as bitsets over deck slots.
//
// Cards are indexed by SLOT (export_index, 0..n-1), never by identity. That
// sidesteps the "basic lands are not unique" problem for free - each basic gets
// its own slot - and makes a zone a 128-bit mask. Whole state is a couple of
// hundred bytes, which fits in L1 and is the single biggest performance
// property of the design (SIM_PLAN.md section 11).

#include <array>
#include <bit>
#include <cstdint>

#include "core/rng.hpp"

namespace cs {

inline constexpr std::size_t kMaxDeckSlots = 128;

// A set of deck slots.
//
// Iteration is ASCENDING SLOT ORDER, always. Section 6.5 bans unordered
// containers from the core because a tiebreak resolved by bucket order is a
// reproducibility bug that looks like variance; a bitset has no such freedom,
// which is why zones are bitsets rather than hash sets.
class Zone {
public:
    constexpr void set(int slot) noexcept { words_[word(slot)] |= bit(slot); }
    constexpr void clear(int slot) noexcept { words_[word(slot)] &= ~bit(slot); }
    constexpr void reset() noexcept { words_ = {}; }

    [[nodiscard]] constexpr bool test(int slot) const noexcept {
        return (words_[word(slot)] & bit(slot)) != 0;
    }
    [[nodiscard]] constexpr int count() const noexcept {
        return std::popcount(words_[0]) + std::popcount(words_[1]);
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return (words_[0] | words_[1]) == 0;
    }

    // Calls f(slot) for every member, lowest slot first.
    template <typename F>
    constexpr void for_each(F&& f) const {
        for (std::size_t w = 0; w < words_.size(); ++w) {
            std::uint64_t bits = words_[w];
            while (bits != 0) {
                const int offset = std::countr_zero(bits);
                bits &= bits - 1;  // clear lowest set bit
                f(static_cast<int>(w * 64) + offset);
            }
        }
    }

    // Is every member of `other` also a member of this? The bitmask form of
    // "all these cards are on the battlefield", which is what a pattern term
    // compiles to (SIM_PLAN.md section 11).
    [[nodiscard]] constexpr bool contains(const Zone& other) const noexcept {
        return (words_[0] & other.words_[0]) == other.words_[0] &&
               (words_[1] & other.words_[1]) == other.words_[1];
    }

    // Do the two sets overlap at all? "at least one of these" as one AND.
    [[nodiscard]] constexpr bool intersects(const Zone& other) const noexcept {
        return ((words_[0] & other.words_[0]) | (words_[1] & other.words_[1])) != 0;
    }

    [[nodiscard]] constexpr Zone operator&(const Zone& other) const noexcept {
        Zone result;
        result.words_[0] = words_[0] & other.words_[0];
        result.words_[1] = words_[1] & other.words_[1];
        return result;
    }

    [[nodiscard]] constexpr Zone operator|(const Zone& other) const noexcept {
        Zone result;
        result.words_[0] = words_[0] | other.words_[0];
        result.words_[1] = words_[1] | other.words_[1];
        return result;
    }

    [[nodiscard]] friend constexpr bool operator==(const Zone&, const Zone&) = default;

private:
    [[nodiscard]] static constexpr std::size_t word(int slot) noexcept {
        return static_cast<std::size_t>(slot) / 64;
    }
    [[nodiscard]] static constexpr std::uint64_t bit(int slot) noexcept {
        return std::uint64_t{1} << (static_cast<std::size_t>(slot) % 64);
    }

    std::array<std::uint64_t, 2> words_{};
};

struct GameState {
    // The library, as slot indices. Cards at [0, drawn) have already been
    // drawn; [drawn, library_count) are still in it.
    //
    // NOT pre-shuffled. Each draw picks uniformly from the remaining range and
    // swaps the choice into place - an incremental Fisher-Yates, mathematically
    // identical to shuffling all 99 first, but paying only for the ~15-25 cards
    // a game actually draws (section 11).
    std::array<std::uint8_t, kMaxDeckSlots> library{};
    std::uint8_t library_count = 0;
    std::uint8_t drawn = 0;

    Zone hand;
    // The command zone. Holds the commander until it is cast, and is a separate
    // zone rather than a flag because the policy has to be able to choose it
    // alongside cards in hand.
    //
    // Commander tax (+{2} per recast) is NOT modelled: nothing in this model
    // destroys a permanent, so a commander is cast at most once per game and
    // the tax never applies. If removal is ever modelled, this becomes wrong.
    Zone command_zone;
    Zone battlefield;
    Zone graveyard;
    Zone tapped;

    std::uint8_t turn = 0;
    bool land_played_this_turn = false;
    int commander_slot = -1;

    [[nodiscard]] int library_size() const noexcept { return library_count - drawn; }
};

// Draws the top card, returning its slot, or -1 if the library is empty.
[[nodiscard]] int draw_one(GameState& state, Rng& rng) noexcept;

// Sets up a game: library from the deck's non-commander slots, opening hand of
// `hand_size`, commander in the command zone.
void begin_game(GameState& state, int deck_slots, int commander_slot, int hand_size,
                Rng& rng) noexcept;

}  // namespace cs

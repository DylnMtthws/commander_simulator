#include "core/state.hpp"

namespace cs {

int draw_one(GameState& state, Rng& rng) noexcept {
    if (state.drawn >= state.library_count) {
        return -1;
    }
    // Incremental Fisher-Yates. Choosing uniformly from the undrawn range and
    // swapping into the draw position gives exactly the distribution of a full
    // shuffle followed by sequential draws - the standard argument is that both
    // produce every permutation of the drawn prefix with equal probability.
    const auto remaining = static_cast<std::uint64_t>(state.library_count - state.drawn);
    const auto offset = static_cast<std::size_t>(rng.below(remaining));
    const std::size_t from = state.drawn + offset;

    const std::uint8_t slot = state.library[from];
    state.library[from] = state.library[state.drawn];
    state.library[state.drawn] = slot;
    ++state.drawn;

    state.hand.set(slot);
    return slot;
}

void begin_game(GameState& state, int deck_slots, int commander_slot, int hand_size,
                Rng& rng) noexcept {
    state = GameState{};
    state.commander_slot = commander_slot;

    // Every slot except the commander, which starts in the command zone rather
    // than the library. Ascending order: the starting arrangement must not
    // depend on anything but the deck, since the shuffle is the only thing
    // allowed to introduce randomness.
    for (int slot = 0; slot < deck_slots; ++slot) {
        if (slot == commander_slot) {
            continue;
        }
        state.library[state.library_count++] = static_cast<std::uint8_t>(slot);
    }

    for (int i = 0; i < hand_size; ++i) {
        // The slot is deliberately discarded: draw_one already put it in hand,
        // and the opening hand is read from the zone, not accumulated here.
        static_cast<void>(draw_one(state, rng));
    }
}

}  // namespace cs

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
    state.copy_of.fill(-1);
    state.commander_slot = commander_slot;
    if (commander_slot >= 0) {
        state.command_zone.set(commander_slot);
    }

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

void begin_game_with_hand(GameState& state, int deck_slots, int commander_slot,
                          const Zone& hand, Rng& rng) noexcept {
    state = GameState{};
    state.copy_of.fill(-1);
    state.commander_slot = commander_slot;
    if (commander_slot >= 0) {
        state.command_zone.set(commander_slot);
    }
    // Everything that is neither the commander nor in the given hand. Ascending
    // order, for the same reason begin_game uses it: the starting arrangement
    // must depend on nothing but the deck.
    for (int slot = 0; slot < deck_slots; ++slot) {
        if (slot == commander_slot || hand.test(slot)) {
            continue;
        }
        state.library[state.library_count++] = static_cast<std::uint8_t>(slot);
    }
    state.hand = hand;
    static_cast<void>(rng);  // the library shuffles lazily, in draw_one
}

Zone sample_hand(int deck_slots, int commander_slot, int size, Rng& rng) noexcept {
    // Drawn through the ordinary machinery rather than by a second sampler, so
    // "an opening hand of this deck" has one definition. A hand sampled a
    // different way from the way the turn loop draws is a different
    // distribution, and the difference would be invisible.
    GameState scratch;
    begin_game(scratch, deck_slots, commander_slot, size, rng);
    return scratch.hand;
}

}  // namespace cs

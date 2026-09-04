#include "core/state.hpp"

namespace cs {

int draw_one(GameState& state, Rng& rng) noexcept {
    if (state.drawable() <= 0) {
        return -1;
    }
    // Incremental Fisher-Yates. Choosing uniformly from the undrawn range and
    // swapping into the draw position gives exactly the distribution of a full
    // shuffle followed by sequential draws - the standard argument is that both
    // produce every permutation of the drawn prefix with equal probability.
    const auto remaining = static_cast<std::uint64_t>(state.drawable());
    const auto offset = static_cast<std::size_t>(rng.below(remaining));
    const std::size_t from = state.drawn + offset;

    const std::uint8_t slot = state.library[from];
    state.library[from] = state.library[state.drawn];
    state.library[state.drawn] = slot;
    ++state.drawn;

    state.hand.set(slot);
    return slot;
}

int mill_one(GameState& state, Rng& rng) noexcept {
    const int slot = draw_one(state, rng);
    if (slot >= 0) {
        state.hand.clear(slot);
        state.graveyard.set(slot);
    }
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

void peek_n(GameState& state, int count, Rng& rng, std::vector<int>& out) {
    out.clear();
    // The same swap draw_one performs, repeated, but WITHOUT taking the cards.
    // `drawn` is advanced so a second peek in the same activation cannot reveal
    // the same card twice, and rewound at the end so the cards are still in the
    // library - a look is not a draw.
    const std::uint8_t first = state.drawn;
    for (int i = 0; i < count; ++i) {
        if (state.drawable() <= 0) {
            break;
        }
        const auto remaining = static_cast<std::uint64_t>(state.drawable());
        const auto offset = static_cast<std::size_t>(rng.below(remaining));
        const std::size_t from = state.drawn + offset;
        const std::uint8_t slot = state.library[from];
        state.library[from] = state.library[state.drawn];
        state.library[state.drawn] = slot;
        ++state.drawn;
        out.push_back(slot);
    }
    state.drawn = first;  // looked at, not drawn
}

void take_peeked(GameState& state, int slot) noexcept {
    // The peeked cards sit at [drawn, drawn + peeked). Swap the taken one to the
    // draw position and advance past it; it has left the library.
    for (std::size_t i = state.drawn; i < state.library_count; ++i) {
        if (state.library[i] == slot) {
            state.library[i] = state.library[state.drawn];
            state.library[state.drawn] = static_cast<std::uint8_t>(slot);
            ++state.drawn;
            return;
        }
    }
}

void bottom_peeked(GameState& state, int slot) noexcept {
    // Swap it past the drawable range. The end region is undrawn and unordered,
    // so "in a random order" needs nothing further.
    const int last = state.library_count - state.bottomed - 1;
    if (last < state.drawn) {
        return;
    }
    for (std::size_t i = state.drawn; i <= static_cast<std::size_t>(last); ++i) {
        if (state.library[i] == slot) {
            state.library[i] = state.library[static_cast<std::size_t>(last)];
            state.library[static_cast<std::size_t>(last)] = static_cast<std::uint8_t>(slot);
            ++state.bottomed;
            return;
        }
    }
}

void exile_library_until(GameState& state, int leave) noexcept {
    if (leave < 0) {
        return;
    }
    while (state.library_size() > leave) {
        const int slot = state.library[state.drawn];
        state.exile.set(slot);
        ++state.drawn;
        if (state.bottomed > 0 && state.drawable() < 0) {
            --state.bottomed;
        }
    }
    state.bottomed = 0;
}

}  // namespace cs

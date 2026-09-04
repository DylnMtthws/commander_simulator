#include "core/pattern.hpp"

#include <algorithm>

namespace cs {

bool requirement_holds(const Requirement& requirement, const PatternSet& set,
                       const GameState& state, FlagMask active,
                       std::span<const Source> sources) noexcept {
    // An ablated card's requirement can never hold. Checked first, because every
    // term below reads a slot mask that no longer means what it was compiled
    // to mean (see Requirement::impossible).
    if (requirement.impossible) {
        return false;
    }
    if ((active & requirement.flags) != requirement.flags) {
        return false;
    }
    // Clones count as what they copied - a Copy Artifact on Basalt Monolith IS
    // a Basalt Monolith for a pattern naming one.
    const Zone board = state.effective_battlefield();
    if (!board.contains(requirement.in_play)) {
        return false;
    }
    if (!state.hand.contains(requirement.in_hand)) {
        return false;
    }
    // "in either zone" is not "in the union of the zones" for a single card,
    // but for a conjunction of cards it is exactly that.
    if (!(board | state.hand).contains(requirement.in_play_or_hand)) {
        return false;
    }
    if (requirement.has_any_of && !board.intersects(requirement.any_of)) {
        return false;
    }
    if (!state.resolved.contains(requirement.resolved)) {
        return false;
    }
    if (!state.graveyard.contains(requirement.in_graveyard)) {
        return false;
    }
    if (!requirement.untapped.empty()) {
        if (!board.contains(requirement.untapped)) {
            return false;
        }
        // Present is not enough: a tapped Kinnan is not an engine.
        if (state.tapped.intersects(requirement.untapped)) {
            return false;
        }
    }
    if (state.turn < requirement.turn_gte) {
        return false;
    }
    if (requirement.creature_count_gte > 0 &&
        (board & set.creature_slots).count() < requirement.creature_count_gte) {
        return false;
    }
    if (requirement.library_size_lte >= 0 &&
        state.library_size() > requirement.library_size_lte) {
        return false;
    }
    if (state.storm_count < requirement.storm_count_gte) {
        return false;
    }
    if (requirement.devotion_gte_library) {
        int devotion_blue = 0;
        state.battlefield.for_each([&](int slot) {
            devotion_blue += set.blue_devotion[
                static_cast<std::size_t>(state.effective(slot))];
        });
        if (devotion_blue < state.library_size()) {
            return false;
        }
    }
    if (requirement.loop_entry_cost >= 0) {
        Cost entry;
        entry.generic = static_cast<std::uint8_t>(requirement.loop_entry_cost *
                                                  requirement.activations);
        // Coloured pips scale with activations too: N casts need N of each.
        for (std::size_t c = 0; c < kColourCount; ++c) {
            entry.pips[c] = static_cast<std::uint8_t>(requirement.entry_pips[c] *
                                                      requirement.activations);
        }
        if (!can_pay(entry, sources, 0)) {
            return false;
        }
    }
    return true;
}

FlagMask active_flags(const PatternSet& set, const GameState& state,
                      std::span<const Source> sources) noexcept {
    FlagMask active = 0;
    // One pass, no fixpoint: an engine may not reference another engine's flag
    // (section 5.1), so a second pass could never set anything new. If engines
    // ever compose, this becomes a loop and needs cycle detection - which is
    // the cost the two-level design is avoiding.
    for (const Engine& engine : set.engines) {
        if (requirement_holds(engine.requires_, set, state, 0, sources)) {
            active |= engine.sets;
        }
    }
    return active;
}

FlagMask all_satisfied(const PatternSet& set, const GameState& state,
                       std::span<const Source> sources) noexcept {
    const FlagMask active = active_flags(set, state, sources);
    FlagMask satisfied = 0;
    const std::size_t limit = std::min(set.patterns.size(), kMaxFlags);
    for (std::size_t i = 0; i < limit; ++i) {
        if (requirement_holds(set.patterns[i].requires_, set, state, active, sources)) {
            satisfied |= FlagMask{1} << i;
        }
    }
    return satisfied;
}

int first_satisfied(const PatternSet& set, const GameState& state,
                    std::span<const Source> sources) noexcept {
    const FlagMask active = active_flags(set, state, sources);
    for (std::size_t i = 0; i < set.patterns.size(); ++i) {
        if (requirement_holds(set.patterns[i].requires_, set, state, active, sources)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace cs

#pragma once

// Printing a single game, turn by turn.
//
// Lives in io/ because core cannot print. The point of the format is that it
// shows candidates the policy REJECTED and what each term contributed, so the
// reader can disagree with a ranking rather than only with an outcome.

#include <cstdio>

#include "core/card.hpp"
#include "core/observer.hpp"
#include "core/pattern.hpp"

namespace cs::io {

class TraceWriter final : public Observer {
public:
    TraceWriter(const CardDb& db, const PatternSet& patterns, std::FILE* out)
        : db_(db), patterns_(patterns), out_(out) {}

    void turn_begin(int turn, const GameState& state) override;
    void drew(int slot, int hand_size) override;
    void mana(std::span<const Source> sources) override;
    void considering(std::span<const Consideration> candidates, const char* what) override;
    void played_land(int slot) override;
    void fetched(int from_slot, int to_slot) override;
    void tutored(int from_slot, int to_slot, bool to_hand) override;
    void cloned(int clone_slot, int copied_slot) override;
    void paid_with_card(int cost_slot, int given_up_slot) override;
    void cast_spell(int slot, int paid) override;
    void pattern_fired(int pattern, int turn) override;
    void engines_active(FlagMask flags) override;
    void loop_available(int engine, int entry_cost, int mana_available) override;
    void game_end(int turn, bool censored) override;

private:
    [[nodiscard]] const char* name_of(int slot) const;

    const CardDb& db_;
    const PatternSet& patterns_;
    std::FILE* out_;
};

}  // namespace cs::io

#pragma once

// The trace hook.
//
// SIM_PLAN.md section 6.6: reading a single game turn by turn is the only way
// to find policy bugs, so it is a v1 feature and was built BEFORE the scorer -
// the policy was developed against readable output rather than against
// aggregate numbers.
//
// Pure virtual, and empty by default so a caller overrides only what it wants.
// The printing implementation lives in io/; core cannot print (section 12.5),
// which is why this is an interface here and a writer there.
//
// In production the pointer is null and every call site is a predictable
// branch. This is the ONLY nullable thing in core, deliberately.

#include <span>

#include "core/mana.hpp"
#include "core/pattern.hpp"
#include "core/state.hpp"

namespace cs {

// Why a candidate scored what it did. The trace prints candidates it REJECTED
// alongside the one it chose - a log that shows only the chosen line cannot
// tell you the ranking was wrong (section 6.6).
struct Consideration {
    int slot = 0;
    int score = 0;
    bool castable = false;
    int rank_term = 0;
    int pattern_term = 0;
    int land_term = 0;
    int castable_term = 0;
};

class Observer {
public:
    virtual ~Observer() = default;

    virtual void turn_begin(int /*turn*/, const GameState&) {}
    virtual void drew(int /*slot*/, int /*hand_size*/) {}
    virtual void mana(std::span<const Source> /*sources*/) {}
    virtual void considering(std::span<const Consideration> /*candidates*/, const char* /*what*/) {}
    virtual void played_land(int /*slot*/) {}
    // The fetch's RESULT, not just its candidates. The first version printed
    // three options and never said which was taken - a reader could see the
    // ranking and not the decision, which is half a trace.
    virtual void fetched(int /*from_slot*/, int /*to_slot*/) {}
    virtual void tutored(int /*from_slot*/, int /*to_slot*/, bool /*to_hand*/) {}
    virtual void cloned(int /*clone_slot*/, int /*copied_slot*/) {}
    virtual void paid_with_card(int /*cost_slot*/, int /*given_up_slot*/) {}
    // The dig RESOLVED, with what it saw and what it kept. A trace that showed
    // only the result could not tell a miss from a bad choice.
    virtual void selected(int /*source_slot*/, std::span<const int> /*revealed*/,
                          int /*kept_slot*/) {}
    virtual void cast_spell(int /*slot*/, int /*paid*/) {}
    virtual void pattern_fired(int /*pattern*/, int /*turn*/) {}
    virtual void engines_active(FlagMask /*flags*/) {}
    // WHY an engine is available, not just that it is. Unbounded mana here is
    // DETECTED from a declared engine, never produced by simulation, and a
    // reader has to be able to see which they are looking at.
    virtual void loop_available(int /*engine*/, int /*entry_cost*/, int /*mana_available*/) {}
    virtual void game_end(int /*turn*/, bool /*censored*/) {}
};

}  // namespace cs

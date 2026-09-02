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
    virtual void cast_spell(int /*slot*/, int /*paid*/) {}
    virtual void pattern_fired(int /*pattern*/, int /*turn*/) {}
    virtual void engines_active(FlagMask /*flags*/) {}
    virtual void game_end(int /*turn*/, bool /*censored*/) {}
};

}  // namespace cs

#include "core/sim.hpp"

#include <vector>

#include "core/mana.hpp"
#include "core/rng.hpp"

namespace cs {
namespace {

// The first castable face of a card, or nullptr. A single-faced card has one
// face; an MDFC has a spell face and a land face (SIM_PLAN.md section 8.2).
const Face* castable_face(const Card& card) noexcept {
    for (const Face& face : card.faces) {
        if (face.is_castable()) {
            return &face;
        }
    }
    return nullptr;
}

bool has_land_face(const Card& card) noexcept {
    for (const Face& face : card.faces) {
        if (face.is_land) {
            return true;
        }
    }
    return false;
}

// Mana available from the battlefield.
//
// STUB-QUALITY, and wrong on purpose: every untapped permanent with a land face
// is treated as tapping for one mana of ANY colour. Real sources come from the
// authored effects in Phase 7, which do not exist yet.
//
// The error is in the GENEROUS direction - a Forest here can produce blue - so
// the loop actually casts things and exercises can_pay. Nothing measured with
// this in place says anything about the deck.
std::vector<Source> stub_sources(const CardDb& db, const GameState& state) {
    std::vector<Source> sources;
    state.battlefield.for_each([&](int slot) {
        if (state.tapped.test(slot)) {
            return;
        }
        const Card& card = db.cards[static_cast<std::size_t>(slot)];
        if (has_land_face(card)) {
            sources.push_back(Source{.produces = 0x1F, .amount = 1, .is_land = true,
                                     .is_creature = false});
        }
    });
    return sources;
}

// FNV-1a over the parts of the state that define what actually happened.
// Not cryptographic and does not need to be: it is a comparison key for "did
// these two runs play the same game", where the alternative is comparing
// nothing at all.
std::uint64_t digest_state(const GameState& state) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFF;
            hash *= 1099511628211ULL;
        }
    };
    // The library ORDER, not just its contents: two games that drew the same
    // cards in a different sequence are different games.
    for (std::size_t i = 0; i < state.library_count; ++i) {
        mix(state.library[i]);
    }
    mix(state.drawn);
    mix(state.turn);
    const auto mix_zone = [&](const Zone& zone) {
        zone.for_each([&](int slot) { mix(static_cast<std::uint64_t>(slot)); });
        mix(0xFFFF);  // separator, so adjacent zones cannot alias
    };
    mix_zone(state.hand);
    mix_zone(state.command_zone);
    mix_zone(state.battlefield);
    mix_zone(state.graveyard);
    mix_zone(state.tapped);
    return hash;
}

}  // namespace

int StubPolicyDoNotUseForResults::choose_land(const CardDb& db, const GameState& state) const {
    int chosen = -1;
    state.hand.for_each([&](int slot) {
        if (chosen == -1 && has_land_face(db.cards[static_cast<std::size_t>(slot)])) {
            chosen = slot;  // lowest slot wins: for_each ascends
        }
    });
    return chosen;
}

int StubPolicyDoNotUseForResults::choose_spell(const CardDb& db, const GameState& state,
                                              std::span<const Source> sources,
                                              GameStats& stats) const {
    int chosen = -1;
    // Hand and command zone together. The commander is castable from the
    // command zone, and without this every pattern naming Kinnan is unreachable
    // by construction - which is exactly what the never-fired report said when
    // this was missing.
    const Zone castable_from = state.hand | state.command_zone;
    castable_from.for_each([&](int slot) {
        if (chosen != -1) {
            return;
        }
        const Card& card = db.cards[static_cast<std::size_t>(slot)];
        const Face* face = castable_face(card);
        if (face == nullptr) {
            return;
        }
        // Every candidate costs one can_pay. This is the count that decides
        // whether can_pay needs optimising (section 11.1).
        ++stats.can_pay_calls;
        if (can_pay(*face->cost, sources, 0)) {
            chosen = slot;
        }
    });
    return chosen;
}

GameResult run_game(const CardDb& db, const PatternSet& patterns, const GameConfig& config,
                    const Policy& policy, std::uint64_t seed) {
    Rng rng(seed);
    GameState state;

    int commander_slot = -1;
    for (const Card& card : db.cards) {
        if (card.is_commander) {
            commander_slot = card.export_index;
        }
    }
    begin_game(state, static_cast<int>(db.size()), commander_slot, config.opening_hand, rng);

    GameResult result;
    result.stats.cards_drawn = config.opening_hand;

    for (std::uint8_t turn = 1; turn <= config.turn_cap; ++turn) {
        state.turn = turn;
        state.land_played_this_turn = false;

        // Untap. Cards that do not untap normally (Basalt Monolith, Grim
        // Monolith, Mana Vault) are a Phase 7 authoring concern; the stub
        // untaps everything.
        state.tapped.reset();

        // Draw. Skipped on turn 1 when on the play - which is itself a declared
        // assumption with a stated bias, since a real pod is on the play 25% of
        // the time (section 2.8).
        if (!(turn == 1 && config.on_the_play)) {
            if (draw_one(state, rng) >= 0) {
                ++result.stats.cards_drawn;
            }
        }

        // Land drop.
        const int land = policy.choose_land(db, state);
        if (land >= 0) {
            state.hand.clear(land);
            state.battlefield.set(land);
            state.land_played_this_turn = true;
            ++result.stats.lands_played;
        }

        // Main phase. Cast until the policy declines or nothing is affordable.
        for (;;) {
            const std::vector<Source> sources = stub_sources(db, state);
            const int spell = policy.choose_spell(db, state, sources, result.stats);
            if (spell < 0) {
                break;
            }
            state.hand.clear(spell);
            state.command_zone.clear(spell);
            state.battlefield.set(spell);
            ++result.stats.spells_cast;

            // Paying taps sources. The stub taps as many lands as the cost's
            // total, which is crude but keeps mana from being infinite - and
            // without it the loop would cast the whole hand every turn.
            const Face* face = castable_face(db.cards[static_cast<std::size_t>(spell)]);
            int to_tap = face != nullptr ? face->cost->mana_value_at_x_zero() : 0;
            state.battlefield.for_each([&](int slot) {
                if (to_tap > 0 && !state.tapped.test(slot) &&
                    has_land_face(db.cards[static_cast<std::size_t>(slot)])) {
                    state.tapped.set(slot);
                    --to_tap;
                }
            });
        }

        ++result.stats.turns;
        result.turns_simulated = turn;

        // Patterns are evaluated once per turn, after the main phase. Checking
        // after every individual cast would report the same TURN number, since
        // that is the reported quantity - so per-turn is exact for the metric,
        // not an approximation of it.
        const int fired = first_satisfied(patterns, state);
        if (fired >= 0) {
            result.outcome.assembled_turn = turn;
            result.outcome.pattern_id = static_cast<std::uint8_t>(fired);
            result.outcome.satisfied_mask = all_satisfied(patterns, state);
            break;  // first assembly is the answer; nothing after it is measured
        }
    }
    result.state_digest = digest_state(state);
    return result;
}

}  // namespace cs

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
    mix(static_cast<std::uint64_t>(state.life));
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

GameResult run_game(const CardDb& db, const EffectDb& effects, const PatternSet& patterns,
                    const GameConfig& config, const Policy& policy, std::uint64_t seed,
                    Observer* observer) {
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

        // Untap - except the cards whose text says they do not. Basalt
        // Monolith, Grim Monolith and Mana Vault all carry "doesn't untap
        // during your untap step", which is the whole reason the Kinnan engine
        // needs an untap ability to iterate at all.
        Zone still_tapped;
        state.tapped.for_each([&](int slot) {
            const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
            if (entry.has_mana_source && !entry.mana_source.untaps_normally) {
                still_tapped.set(slot);
            }
        });
        state.tapped = still_tapped;

        // Draw. Skipped on turn 1 when on the play - which is itself a declared
        // assumption with a stated bias, since a real pod is on the play 25% of
        // the time (section 2.8).
        if (observer != nullptr) {
            observer->turn_begin(turn, state);
        }
        if (!(turn == 1 && config.on_the_play)) {
            const int slot = draw_one(state, rng);
            if (slot >= 0) {
                ++result.stats.cards_drawn;
                if (observer != nullptr) {
                    observer->drew(slot, state.hand.count());
                }
            }
        }

        // Land drop.
        std::vector<Source> sources;
        collect_sources(db, effects, state, config.table, sources);
        const Context land_context{db, patterns, state, sources, observer};
        const int land = policy.choose_land(land_context, result.stats);
        if (land >= 0) {
            state.hand.clear(land);
            state.battlefield.set(land);
            state.land_played_this_turn = true;
            ++result.stats.lands_played;
            // Announced BEFORE the fetch it triggers. The first version printed
            // the fetch candidates first, so a trace read "considering (fetch
            // target)" and then "PLAY LAND: Windswept Heath" - the third
            // output-ordering bug in this project found by reading output
            // rather than by any test.
            if (observer != nullptr) {
                observer->played_land(land);
            }

            // A fetch resolves immediately: sacrifice, find, shuffle. Modelled
            // as removing the found land from the undrawn library and swapping
            // it onto the battlefield, which is what "search, then shuffle"
            // amounts to when the library order is already random.
            const CardEffects& fetch_entry = effects.by_slot[static_cast<std::size_t>(land)];
            if (fetch_entry.has_fetch) {
                std::vector<int> candidates;
                fetch_candidates(fetch_entry.fetch, db, state, candidates);
                const int target = policy.choose_fetch(land_context, candidates, result.stats);
                if (observer != nullptr) {
                    observer->fetched(land, target);
                }
                state.battlefield.clear(land);
                state.graveyard.set(land);
                state.life -= fetch_entry.fetch.life_cost;
                ++result.stats.fetches_used;
                if (target >= 0) {
                    for (std::size_t i = state.drawn; i < state.library_count; ++i) {
                        if (state.library[i] == target) {
                            state.library[i] = state.library[state.library_count - 1];
                            --state.library_count;
                            break;
                        }
                    }
                    state.battlefield.set(target);
                    const CardEffects& found = effects.by_slot[static_cast<std::size_t>(target)];
                    if (found.has_mana_source &&
                        enters_tapped(found.mana_source, db, effects, state, config.table)) {
                        state.tapped.set(target);
                    }
                }
            }
            // A land that enters tapped produces nothing this turn. Getting
            // this wrong would silently give the deck a turn it did not have.
            const CardEffects& entry = effects.by_slot[static_cast<std::size_t>(land)];
            if (entry.has_mana_source) {
                if (enters_tapped(entry.mana_source, db, effects, state, config.table)) {
                    state.tapped.set(land);
                } else if (entry.mana_source.enters_tapped_unless == EntersTappedUnless::PayLife) {
                    // Entering untapped was a choice and it was paid for.
                    state.life -= entry.mana_source.enters_tapped_param;
                }
            }
        }

        // Main phase. Cast until the policy declines or nothing is affordable.
        // Recomputed after the land drop, and printed HERE rather than before
        // it. An earlier version printed mana at the top of the turn, so a
        // trace showed one source and then a two-mana cast on the next line -
        // technically consistent, unreadable, and exactly the kind of thing
        // this output exists to make obvious.
        bool announced = false;
        for (;;) {
            collect_sources(db, effects, state, config.table, sources);
            if (observer != nullptr && !announced) {
                observer->mana(sources);
                announced = true;
            }
            const Context context{db, patterns, state, sources, observer};
            const int spell = policy.choose_spell(context, result.stats);
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
            if (observer != nullptr) {
                observer->cast_spell(spell, to_tap);
            }
            // Paying taps sources. Crude - it taps in slot order rather than
            // solving which sources to spend, which is a policy question and a
            // later one (section 6.4). Without it mana would be infinite.
            state.battlefield.for_each([&](int slot) {
                const CardEffects& source_entry = effects.by_slot[static_cast<std::size_t>(slot)];
                // A ritual is consumed rather than tapped: Lotus Petal
                // sacrifices itself.
                if (to_tap > 0 && !state.tapped.test(slot) && source_entry.has_ritual &&
                    source_entry.ritual.from_zone == RitualZone::Battlefield) {
                    state.battlefield.clear(slot);
                    state.graveyard.set(slot);
                    to_tap -= source_entry.ritual.amount;
                    return;
                }
                if (to_tap > 0 && !state.tapped.test(slot) && source_entry.has_mana_source) {
                    state.tapped.set(slot);
                    state.life -= life_cost_of_tapping(effects, state, slot);
                    to_tap -= source_entry.mana_source.amount;
                }
            });
        }

        ++result.stats.turns;
        result.turns_simulated = turn;

        // Patterns are evaluated once per turn, after the main phase.
        //
        // CORRECTION to an earlier claim in this file: per-turn evaluation is
        // exact for the TURN NUMBER, but it is NOT exact for WHICH pattern
        // fires. Any state the policy creates and resolves inside a single turn
        // is invisible here.
        //
        // Observed: `infinite_C_outlet_in_hand` (engine online, outlet still in
        // hand) went from firing 145 times under the stub to never being
        // satisfied at all once the authored policy landed - because the policy
        // casts a rank-88 Thrasios the moment it is affordable, so the outlet
        // is never still in hand when the check runs. The pattern is dead GIVEN
        // THIS POLICY, which is a fourth cause of a never-fired pattern beyond
        // dead, buggy and shadowed.
        if (observer != nullptr) {
            observer->engines_active(active_flags(patterns, state));
        }
        const int fired = first_satisfied(patterns, state);
        if (fired >= 0) {
            result.outcome.assembled_turn = turn;
            result.outcome.pattern_id = static_cast<std::uint8_t>(fired);
            result.outcome.satisfied_mask = all_satisfied(patterns, state);
            if (observer != nullptr) {
                observer->pattern_fired(fired, turn);
            }
            break;  // first assembly is the answer; nothing after it is measured
        }
    }
    result.state_digest = digest_state(state);
    if (observer != nullptr) {
        observer->game_end(result.turns_simulated, result.outcome.censored());
    }
    return result;
}

}  // namespace cs

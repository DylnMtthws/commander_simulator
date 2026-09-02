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
    for (std::size_t i = 0; i < kMaxDeckSlots; ++i) {
        mix(static_cast<std::uint64_t>(state.copy_of[i] + 1));
    }
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
        const Context land_context{db, patterns, state, sources, observer, &effects};
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
        // Mana is printed before EVERY cast decision, not once per turn.
        //
        // Once per turn was readable and wrong, in the way section 11.0's ninth
        // rule describes: a turn's own casts change the board, so a trace read
        // "mana: 2x{G} 1x{UG}" and then paid 2, then 4, then 1 out of it. Every
        // number was correct and the log argued with itself, because the line a
        // reader compares against was three casts stale. The reader cannot tell
        // a payment bug from a legal mid-turn ramp - which is exactly the
        // distinction they are reading the trace to make.
        for (;;) {
            collect_sources(db, effects, state, config.table, sources);
            if (observer != nullptr) {
                observer->mana(sources);
            }
            const Context context{db, patterns, state, sources, observer, &effects};
            const int spell = policy.choose_spell(context, result.stats);
            if (spell < 0) {
                break;
            }
            state.hand.clear(spell);
            state.command_zone.clear(spell);
            state.battlefield.set(spell);
            ++result.stats.spells_cast;

            // Announced BEFORE its consequences. The fetch had this bug and so
            // did this: a trace read "TUTOR: Nature's Rhythm -> ..." and then
            // "CAST: Nature's Rhythm" - the effect before the cause. Fifth
            // output-ordering issue in this project, all five found by reading
            // output and none by a test (upstream PLAN.md 11.0).
            const Face* cast_face = castable_face(db.cards[static_cast<std::size_t>(spell)]);
            int to_tap = cast_face != nullptr ? cast_face->cost->mana_value_at_x_zero() : 0;
            if (observer != nullptr) {
                observer->cast_spell(spell, to_tap);
            }

            const CardEffects& cast_entry = effects.by_slot[static_cast<std::size_t>(spell)];

            // CONVOKE, before anything reads the mana available. Each creature
            // tapped pays {1} or one pip of its colour, and only creatures with
            // no mana ability of their own are offered - under Kinnan a mana
            // dork produces two and convoking it produces one, so tapping it
            // for mana dominates (core/effects.hpp).
            int convoke_available = 0;
            std::vector<int> convokable;
            if (cast_entry.convoke) {
                convoke_slots(db, effects, state, convokable);
                convoke_available = static_cast<int>(convokable.size());
            }

            if (cast_entry.has_clone) {
                std::vector<int> targets;
                clone_candidates(cast_entry.clone, db, state, total_mana(sources), targets);
                const Context clone_context{db, patterns, state, sources, observer, &effects};
                const int copied = policy.choose_clone(clone_context, targets, result.stats);
                if (observer != nullptr) {
                    observer->cloned(spell, copied);
                }
                if (copied >= 0) {
                    state.copy_of[static_cast<std::size_t>(spell)] =
                        static_cast<std::int8_t>(copied);
                    ++result.stats.clones_made;
                }
            }

            if (cast_entry.has_tutor) {
                // The cap for an X tutor is the mana LEFT after paying the
                // coloured part, approximated as everything currently
                // available. Generous, and stated as such.
                std::vector<int> targets;
                tutor_candidates(cast_entry.tutor, db, state,
                                 total_mana(sources) + convoke_available, targets);
                const bool to_hand = cast_entry.tutor.destination == TutorDestination::Hand;
                const Context tutor_context{db, patterns, state, sources, observer, &effects};
                const int found =
                    policy.choose_tutor(tutor_context, targets, to_hand, result.stats);
                if (observer != nullptr) {
                    observer->tutored(spell, found, to_hand);
                }
                if (found >= 0) {
                    for (std::size_t i = state.drawn; i < state.library_count; ++i) {
                        if (state.library[i] == found) {
                            state.library[i] = state.library[state.library_count - 1];
                            --state.library_count;
                            break;
                        }
                    }
                    if (to_hand) {
                        state.hand.set(found);
                    } else {
                        state.battlefield.set(found);
                    }
                    ++result.stats.tutors_used;
                }
            }

            // DRAW, on resolution. Borne Upon a Wind's second line - see
            // data/effects.toml for why its first line nearly buried it.
            if (cast_entry.has_draw) {
                for (int i = 0; i < cast_entry.draw.cards; ++i) {
                    const int drawn = draw_one(state, rng);
                    if (drawn < 0) {
                        break;  // decked; the turn cap ends the game either way
                    }
                    ++result.stats.cards_drawn;
                    ++result.stats.cards_drawn_by_effect;
                    if (observer != nullptr) {
                        observer->drew(drawn, state.hand.count());
                    }
                }
            }

            if (cast_entry.has_mass_untap) {
                Zone keep_tapped;
                state.tapped.for_each([&](int slot) {
                    const CardEffects& tapped_entry =
                        effects.by_slot[static_cast<std::size_t>(slot)];
                    const bool is_land =
                        tapped_entry.has_mana_source && tapped_entry.mana_source.is_land;
                    if (cast_entry.mass_untap.nonland_only && is_land) {
                        keep_tapped.set(slot);
                    }
                });
                state.tapped = keep_tapped;
                ++result.stats.mass_untaps;
            }

            // Convoke is paid FIRST, because a convoked creature is not a
            // source and cannot be spent twice. Slot order, like the rest of
            // payment - the set was already filtered to creatures whose tap is
            // worth nothing else.
            for (const int convoked : convokable) {
                if (to_tap <= 0) {
                    break;
                }
                if (!state.tapped.test(convoked)) {
                    state.tapped.set(convoked);
                    --to_tap;
                    ++result.stats.convoked;
                }
            }

            // Paying SPENDS THE SOURCES collect_sources produced.
            //
            // It used to walk the battlefield and re-derive what a source was,
            // and re-derived it differently: it required `has_mana_source`, so
            // a creature that is a source only because Enduring Vitality grants
            // it one was never tapped, and a clone was looked up by its own slot
            // rather than the card it copies, so it was never tapped either.
            // It also subtracted the UNMULTIPLIED amount, ignoring Kinnan. Mana
            // from Vitality, from every clone, and from Elvish Spirit Guide in
            // hand was therefore free and unlimited - the deck could spend
            // twenty-four mana off a board of two lands, which is what reading
            // one trace showed and no counter did.
            //
            // One function decides what a source is; this one spends what that
            // function returned. Any future disagreement between them is now
            // impossible rather than unlikely.
            //
            // Still crude in the DECLARED way (section 6.4): it spends in a
            // fixed order rather than solving which sources to keep. The order
            // is cheapest resource first - tap a permanent, then sacrifice one,
            // then exile a card from hand - so Elvish Spirit Guide is the last
            // thing spent rather than the first.
            const auto spend = [&](const Source& source, int pass) {
                if (to_tap <= 0 || source.slot < 0) {
                    return;
                }
                const auto index = static_cast<std::size_t>(source.slot);
                const CardEffects& source_entry =
                    effects.by_slot[static_cast<std::size_t>(state.effective(source.slot))];
                const bool from_hand =
                    source_entry.has_ritual && source_entry.ritual.from_zone == RitualZone::Hand;
                const bool sacrifices = source_entry.has_ritual && !from_hand;
                const int want = from_hand ? 2 : (sacrifices ? 1 : 0);
                if (want != pass) {
                    return;
                }
                if (from_hand) {
                    state.hand.clear(source.slot);
                } else {
                    if (state.tapped.test(source.slot)) {
                        return;
                    }
                    if (sacrifices) {
                        state.battlefield.clear(source.slot);
                    } else {
                        state.tapped.set(source.slot);
                        state.life -= life_cost_of_tapping(effects, state, source.slot);
                    }
                }
                if (from_hand || sacrifices) {
                    state.graveyard.set(source.slot);
                }
                static_cast<void>(index);
                to_tap -= source.amount;
            };
            for (int pass = 0; pass < 3 && to_tap > 0; ++pass) {
                for (const Source& source : sources) {
                    spend(source, pass);
                }
            }

            // An instant or sorcery does not stay on the battlefield.
            //
            // This used to be a clause inside the TUTOR block reading "if it is
            // not a mana source", which was right for the nine tutors and wrong
            // for everything else: Dramatic Reversal and Borne Upon a Wind are
            // both instants, and under the old rule they sat on the battlefield
            // for the rest of the game as blank permanents - counted by Gene
            // Pollinator's untapped-permanent condition and offered to every
            // clone as a legal target. The rule belongs to the CARD TYPE, not
            // to one effect kind.
            //
            // The exception is a clone: Flash Photography is a sorcery that
            // leaves a token copy behind, and the copy IS this slot.
            if (!is_permanent(db.cards[static_cast<std::size_t>(spell)]) &&
                state.copy_of[static_cast<std::size_t>(spell)] < 0) {
                state.battlefield.clear(spell);
                state.graveyard.set(spell);
            }
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
        // Sources are recomputed for the pattern check, because an engine's
        // entry cost is checked against what is available RIGHT NOW - after the
        // turn's casting has tapped things. That is the whole point of
        // loop_entry_cost: an unbounded engine you cannot pay to enter is not
        // unbounded.
        collect_sources(db, effects, state, config.table, sources);
        if (observer != nullptr) {
            observer->engines_active(active_flags(patterns, state, sources));
            for (std::size_t e = 0; e < patterns.engines.size(); ++e) {
                const Engine& engine = patterns.engines[e];
                if (engine.requires_.loop_entry_cost >= 0 &&
                    requirement_holds(engine.requires_, patterns, state, 0, sources)) {
                    observer->loop_available(static_cast<int>(e),
                                             engine.requires_.loop_entry_cost,
                                             total_mana(sources));
                }
            }
        }
        const int fired = first_satisfied(patterns, state, sources);
        if (fired >= 0) {
            result.outcome.assembled_turn = turn;
            result.outcome.pattern_id = static_cast<std::uint8_t>(fired);
            result.outcome.satisfied_mask = all_satisfied(patterns, state, sources);
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

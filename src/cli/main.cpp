// Loads a card database and prints what it found.
//
// The first end-to-end path: exporter -> cards.json -> C++ -> a summary a human
// can check against the database. Argument parsing, the metric banner and the
// statistics arrive in Phase 6 (SIM_PLAN.md section 9.5).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "core/card.hpp"
#include "core/rng.hpp"
#include "core/sim.hpp"
#include "core/ablation.hpp"
#include "core/stats.hpp"
#include "core/sweep.hpp"
#include "core/version.hpp"
#include "io/card_db_load.hpp"
#include "io/deck_load.hpp"
#include "io/effects_load.hpp"
#include "io/service.hpp"
#include "io/trace.hpp"

namespace {

// std::string_view is not guaranteed null-terminated - it is a pointer and a
// length, and may point into the middle of a buffer. "%s" would read until it
// happened to find a zero byte. Print one by passing its length.
void print_view(const char* format, std::string_view value) {
    std::printf(format, static_cast<int>(value.size()), value.data());
}

int summarise(const std::filesystem::path& path) {
    cs::CardDb db;
    try {
        db = cs::io::load_card_db(path);
    } catch (const cs::io::LoadError& error) {
        // stdout is buffered and stderr is not, so without this the error can
        // appear before output already written above it.
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    const cs::Manifest& m = db.manifest;
    std::printf("%s\n", path.c_str());
    std::printf("  %zu cards from %s%s\n", db.size(), m.source_view.c_str(),
                m.is_fixture ? "  [FIXTURE]" : "");
    std::printf("  data as of %s\n", m.max_content_updated_at.c_str());
    std::printf("  cards sha256 %s\n", m.cards_sha256.substr(0, 12).c_str());
    if (m.corpus_row_count > 0) {
        std::printf("  corpus %d rows\n", m.corpus_row_count);
    }

    std::size_t lands = 0;
    std::size_t castable = 0;
    std::size_t variable = 0;
    std::size_t multi_faced = 0;
    for (const cs::Card& card : db.cards) {
        bool has_land = false;
        bool has_castable = false;
        bool has_variable = false;
        for (const cs::Face& face : card.faces) {
            has_land = has_land || face.is_land;
            has_castable = has_castable || face.is_castable();
            has_variable = has_variable || (face.cost && face.cost->variable > 0);
        }
        lands += has_land ? 1 : 0;
        castable += has_castable ? 1 : 0;
        variable += has_variable ? 1 : 0;
        multi_faced += card.faces.size() > 1 ? 1 : 0;
    }

    std::printf("  %zu with a land face, %zu castable, %zu X spells, %zu multi-faced\n",
                lands, castable, variable, multi_faced);

    if (!m.does_not_measure.empty()) {
        std::printf("  metric %s; NOT %s\n", m.metric.c_str(), m.does_not_measure.c_str());
    }
    return 0;
}

}  // namespace

// Runs `games` games and reports aggregate counters.
//
// The number this exists for is CALLS PER GAME. Section 11.1 measured can_pay
// at ~65 ns and noted that per-game cost is calls x 65 ns, with the call count
// a property of the policy. This is the first time that number can be observed
// at all - against the stub, so it is a floor rather than an estimate.
// Plays ONE game and prints it turn by turn.
//
// A v1 feature, not a debugging afterthought (SIM_PLAN.md section 6.6). It is
// how a policy bug is found at all: aggregate numbers can tell you the deck is
// slow and never that it kept a Forest over Basalt Monolith on turn three.
// [table] flows from the deck file into the simulation. Every field is
// required there and none has a default, so this cannot silently pick one.
cs::GameConfig make_config(const cs::io::DeckFile& deck) {
    cs::GameConfig config;
    config.table.opponents = deck.table.opponents;
    config.table.opponent_colors = deck.table.opponent_colors;
    config.table.on_the_play = deck.table.on_the_play;
    config.on_the_play = deck.table.on_the_play;
    config.table.life_floor = deck.life_floor;
    return config;
}

cs::EffectDb load_effects_for_deck(const std::filesystem::path& path, const cs::CardDb& db,
                                   cs::io::DeckFile& deck) {
    cs::EffectDb effects = cs::io::load_effects(path, db, &deck.patterns);
    cs::derive_policy_ranks(db, effects, deck.patterns, deck.rank_overrides, deck.weights);
    return effects;
}

int trace_one(const std::filesystem::path& path, const std::filesystem::path& deck_path,
              const std::filesystem::path& effects_path, std::uint64_t seed) {
    cs::CardDb db;
    cs::io::DeckFile deck;
    cs::EffectDb effects;
    try {
        db = cs::io::load_card_db(path);
        deck = cs::io::load_deck(deck_path, db);
        effects = load_effects_for_deck(effects_path, db, deck);
    } catch (const std::runtime_error& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    const cs::AuthoredPolicy policy(deck.weights);
    cs::GameConfig config = make_config(deck);
    cs::io::TraceWriter writer(db, deck.patterns, stdout);

    // The banner is unconditional, on a trace too (section 9.5, rule 1).
    std::printf(
        "METRIC: goldfish turns-to-assembly. NOT deck strength, win rate, or card quality.\n"
        "        A faster number is not a better deck.\n");
    std::printf("\ntrace: seed %llu, policy %s\n",
                static_cast<unsigned long long>(seed), policy.name());
    std::printf("  scores shown are rank*1000 plus state-dependent terms; rejected\n");
    std::printf("  candidates are listed so the ranking can be disagreed with.\n");
    std::printf("  mana is printed before EVERY cast, because a turn's own casts change\n");
    std::printf("  it - a once-per-turn line goes stale mid-turn and the log then argues\n");
    std::printf("  with itself.\n");
    static_cast<void>(cs::run_game(db, effects, deck.patterns, config, policy, seed, &writer));
    return 0;
}

// The honesty header (SIM_PLAN.md section 9.5).
//
// Printed before any number, on every run, unconditionally - not a footer and
// not behind a flag. Three rules keep it from becoming boilerplate people skip:
// the metric banner is first; the label lives in the column name and not only
// here; and every assumption line is GENERATED by the thing that owns the
// assumption, so changing the assumption changes the text and a stale caveat
// is not expressible.
void print_header(const cs::CardDb& db, const cs::io::DeckFile& deck, const cs::EffectDb& effects,
                  const std::filesystem::path& deck_path, int games, std::uint64_t seed) {
    std::printf(
        "METRIC: goldfish turns-to-assembly. NOT deck strength, win rate, or card quality.\n"
        "        Measures how fast this deck assembles a declared pattern with nobody\n"
        "        interacting. A faster number is not a better deck.\n\n");
    std::printf(
        "HOW COMBOS ARE COUNTED: unbounded mana is DETECTED from a declared engine in the\n"
        "        deck file, never produced by simulating the loop. An engine fires when its\n"
        "        pieces are present AND its entry cost is payable from the mana actually\n"
        "        available. This model RECOGNISES a combo; it does not claim to have\n"
        "        played one.\n\n");

    std::printf("deck: %s   cards: %zu   modelled: %d   inert: %d   patterns: %zu\n",
                deck_path.filename().c_str(), db.size(), effects.modeled, effects.inert,
                deck.patterns.patterns.size());
    if (!deck.patterns.inherited_pattern_names.empty()) {
        std::printf("inherited patterns:");
        for (const std::string& name : deck.patterns.inherited_pattern_names) {
            std::printf(" %s", name.c_str());
        }
        std::printf("\n");
    }
    std::printf("policy ranks: derived from roles; overrides: %zu",
                deck.rank_override_names.size());
    for (const std::string& name : deck.rank_override_names) std::printf(" %s", name.c_str());
    std::printf("\n");
    std::printf("data: manifest %s (%s)\n", db.manifest.cards_sha256.substr(0, 8).c_str(),
                db.manifest.max_content_updated_at.substr(0, 10).c_str());
    // The DECKLIST's provenance, separately from the card data's. They answer
    // different questions: the manifest says which card texts were used, this
    // says which list. A number is attributable to neither without both.
    std::printf("list: %s snapshot %s, 99 sha256 %s\n", deck.provenance.source.c_str(),
                deck.provenance.snapshot_date.c_str(),
                deck.provenance.cards_sha256.substr(0, 8).c_str());
    if (deck.provenance.source_url.empty()) {
        std::printf("      SOURCE URL NOT RECORDED. This is a snapshot of a living list and\n");
        std::printf("      the list has since changed; without the URL the snapshot cannot be\n");
        std::printf("      re-derived or diffed against its origin.\n");
    } else {
        std::printf("      %s\n", deck.provenance.source_url.c_str());
    }

    std::string colours;
    static constexpr char kOrder[] = "WUBRG";
    for (int i = 0; i < 5; ++i) {
        if ((deck.table.opponent_colors & (1U << i)) != 0) {
            colours += kOrder[i];
        }
    }
    std::printf("table context: opponents=%d, opponent_colors=%s, on_the_play=%s\n",
                deck.table.opponents, colours.empty() ? "(none)" : colours.c_str(),
                deck.table.on_the_play ? "true" : "false");
    std::printf("ablation baseline: replacement = %s\n", deck.ablation_replacement.c_str());
    std::printf("games: %d, seed %llu\n", games,
                static_cast<unsigned long long>(seed));

    // The grouped table, not the bare count. A count says how much the model
    // cannot see; the categories say WHAT, and if `interaction` dominates the
    // fix is opposition profiles rather than a bigger card model (section 4.4).
    std::printf("\nWHAT THE MODEL CANNOT SEE  (%d inert cards, by reason)\n", effects.inert);
    for (std::size_t i = 0; i < effects.inert_categories.size(); ++i) {
        const std::string& category = effects.inert_categories[i];
        std::printf("  %-20s %3d   %s\n", category.c_str(), effects.inert_counts[i],
                    cs::reason_category_meaning(category));
    }

    // A DISPUTED INERT ENTRY IS PRINTED WITH THE TABLE IT SITS IN, not left to a
    // document. The inert table is the model's statement of its own limits, so a
    // reader is entitled to know that the author believes one of its entries is
    // wrong - and to know it here rather than by reading the effects file.
    if (effects.disputed > 0) {
        std::printf("\n  OF THOSE %d, %d %s CLASSIFIED WRONG - by the author, and left in place:\n",
                    effects.inert, effects.disputed, effects.disputed == 1 ? "IS" : "ARE");
        for (const cs::Card& card : db.cards) {
            const cs::CardEffects& entry =
                effects.by_slot[static_cast<std::size_t>(card.export_index)];
            if (entry.disputed.empty()) {
                continue;
            }
            std::printf("    %s\n", card.listed_name.c_str());
            // Wrapped at a width a terminal keeps, because an unwrapped
            // paragraph here is a caveat nobody finishes reading.
            std::string line = "      ";
            for (std::size_t i = 0; i <= entry.disputed.size(); ++i) {
                if (i == entry.disputed.size() ||
                    (entry.disputed[i] == ' ' && line.size() > 68)) {
                    std::printf("%s\n", line.c_str());
                    line = "      ";
                    continue;
                }
                line += entry.disputed[i];
            }
        }
        std::printf("    This is the LARGEST KNOWN GAP: a card the model has, in the pattern\n");
        std::printf("    layer's own subject matter, filed wrong. Distinct from a missing\n");
        std::printf("    MECHANIC, which the model could not express at all.\n");
    }

    std::printf("\nASSUMPTIONS THIS RESULT DEPENDS ON\n");
    int interaction = 0;
    for (std::size_t i = 0; i < effects.inert_categories.size(); ++i) {
        if (effects.inert_categories[i] == "interaction") {
            interaction = effects.inert_counts[i];
        }
    }
    std::printf("  - No opponents. %d inert cards; %d of them are interaction.\n", effects.inert,
                interaction);

    // Every line below is built by walking the loaded data, never typed. A
    // hand-written list of card names is one authoring pass from being wrong.
    const auto names_where = [&](const auto& predicate) {
        std::string list;
        for (const cs::Card& card : db.cards) {
            const cs::CardEffects& entry =
                effects.by_slot[static_cast<std::size_t>(card.export_index)];
            if (!predicate(entry)) {
                continue;
            }
            if (!list.empty()) {
                list += ", ";
            }
            list += card.listed_name;
        }
        return list;
    };

    const std::string draws_zero = names_where([](const cs::CardEffects& entry) {
        return entry.status == cs::AuthorStatus::Inert &&
               entry.reason_category == "opponent_trigger";
    });
    if (!draws_zero.empty()) {
        // ONE PER LINE, not a comma-joined list. "Wan Shi Tong, Librarian" is a
        // single card whose name contains a comma, and in a comma-joined list it
        // reads as two - a caveat block that miscounts the cards it is warning
        // about is worse than no caveat block.
        std::printf("  - These do nothing at all, because no opponent acts:\n");
        for (const cs::Card& card : db.cards) {
            const cs::CardEffects& entry =
                effects.by_slot[static_cast<std::size_t>(card.export_index)];
            if (entry.status == cs::AuthorStatus::Inert &&
                entry.reason_category == "opponent_trigger") {
                std::printf("      %s\n", card.listed_name.c_str());
            }
        }
    }

    const std::string from_table = names_where([](const cs::CardEffects& entry) {
        return entry.has_mana_source && entry.mana_source.colours_from_table;
    });
    if (!from_table.empty()) {
        std::printf("  - %s produce mana only because opponent_colors is set;\n"
                    "    with opponent_colors=[] they produce nothing.\n",
                    from_table.c_str());
    }

    const std::string needs_opponents = names_where([](const cs::CardEffects& entry) {
        return entry.has_mana_source &&
               entry.mana_source.enters_tapped_unless == cs::EntersTappedUnless::OpponentCount;
    });
    if (!needs_opponents.empty()) {
        std::printf("  - %s enters untapped only because opponents=%d.\n", needs_opponents.c_str(),
                    deck.table.opponents);
    }

    int life_payers = 0;
    for (const cs::CardEffects& entry : effects.by_slot) {
        life_payers += (entry.has_mana_source && entry.mana_source.life_cost > 0) ? 1 : 0;
        life_payers += entry.has_fetch && entry.fetch.life_cost > 0 ? 1 : 0;
    }
    std::printf("  - %d sources cost life to use, against a floor of %d. A source that would\n"
                "    take life below the floor is not offered at all, so life gates WHICH\n"
                "    SOURCES EXIST and never enters the scorer.\n",
                life_payers, deck.life_floor);

    std::printf("  - on_the_play=%s. This matches a real four-player pod 25%% of the time.\n"
                "    Re-run with the other value to bound it.\n",
                deck.table.on_the_play ? "true" : "false");
    std::printf("  - Ablation replaces with %s: ablating a nonland raises land count by one\n"
                "    and flatters that result; ablating a land holds it constant.\n",
                deck.ablation_replacement.c_str());
    std::printf("  - Patterns are ASSEMBLY states, not wins. This deck has no \"you win the\n"
                "    game\" card; its real kills are opponent-facing (section 5.4).\n");

    // ACTIVATIONS, beside opponents and on_the_play, because it is the same kind
    // of thing: a required declaration with no default that moves the headline.
    // Section 16.7 measured 2.97 points of P(assembled by turn 3) between one
    // activation and two - more than every card in the sweep except three - so
    // it is printed next to the number it moves rather than left in a file.
    for (const cs::WinPattern& pattern : deck.patterns.patterns) {
        if (pattern.requires_.loop_entry_cost < 0) {
            continue;
        }
        std::printf("  - %s counts as assembled at %d activation%s of a %d-mana\n"
                    "    ability. That is a judgement about Magic, not a fact about the card,\n"
                    "    and it moves this result by about %.1f points per activation.\n",
                    pattern.name.c_str(), pattern.requires_.activations,
                    pattern.requires_.activations == 1 ? "" : "s",
                    pattern.requires_.loop_entry_cost, 3.0);
    }
    for (const cs::Engine& engine : deck.patterns.engines) {
        if (engine.requires_.loop_entry_cost < 0) {
            continue;
        }
        std::printf("  - %s is UNBOUNDED once entered, so one activation is all of\n"
                    "    them: entry costs %d and the loop untaps itself.\n",
                    engine.name.c_str(), engine.requires_.loop_entry_cost);
    }

    if (effects.unauthored > 0) {
        std::printf("  - %d of %zu cards are UNAUTHORED. They are drawn and dilute every draw,\n"
                    "    but do nothing when cast, which UNDERSTATES the deck.\n",
                    effects.unauthored, db.size());
    }
}

void print_report(const cs::CardDb& db, const cs::io::DeckFile& deck, const cs::RunSummary& run,
                  const cs::GameConfig& config, const cs::Policy& policy) {
    std::printf("\npolicy: %s\n", policy.name());
    if (run.selects_used > 0) {
        // THE HIT RATE, printed because it is the one number here with an
        // INDEPENDENTLY MEASURED expected value: 73.6%, measured from the
        // library composition at the moment the old detection fired, BEFORE the
        // ability was implemented (SIM_PLAN.md 16.7b). Every other assertion in
        // this project checks the code against a number the same code produced.
        std::printf("  dig: %.2f activations/game, %.1f%% found a non-Human creature\n",
                    static_cast<double>(run.selects_used) / run.games,
                    100.0 * static_cast<double>(run.selects_hit) /
                        static_cast<double>(run.selects_used));
    }
    std::printf("  can_pay calls/game %.1f   cards drawn/game %.1f (%.1f of them by an effect)\n",
                static_cast<double>(run.can_pay_calls) / run.games,
                static_cast<double>(run.cards_drawn) / run.games,
                static_cast<double>(run.cards_drawn_by_effect) / run.games);

    // THE PRIMARY OUTPUT (section 10.1): the CDF, with an interval on every
    // point. Wilson, not the normal approximation - the turn-2 and turn-3 rows
    // are where p is near zero and the approximation returns intervals that
    // extend below zero, and those are the rows this deck is read for.
    std::printf("\nP(assembled by turn N), Wilson 95%%\n");
    std::printf("  %-5s %8s   %-16s %s\n", "turn", "P", "95% interval", "width");
    for (int turn = 1; turn <= config.turn_cap; ++turn) {
        const int reached = cs::cumulative(run, turn);
        if (reached == 0) {
            continue;
        }
        const cs::Interval interval = cs::wilson(reached, run.games);
        std::printf("  %-5d %7.2f%%   [%5.2f%%, %5.2f%%]  %+.2f\n", turn,
                    100.0 * reached / run.games, 100.0 * interval.low, 100.0 * interval.high,
                    100.0 * (interval.high - interval.low));
    }
    if (cs::cumulative(run, config.turn_cap) == 0) {
        std::printf("  (nothing assembled in any game)\n");
    }
    const cs::Interval censored_interval = cs::wilson(run.censored, run.games);
    std::printf("  %-5s %7.2f%%   [%5.2f%%, %5.2f%%]   (%d games never assembled)\n", "never",
                100.0 * run.censored / run.games, 100.0 * censored_interval.low,
                100.0 * censored_interval.high, run.censored);

    // Percentiles, and the censoring rule (section 10.3). A percentile computed
    // over only the winners is a lie that looks like data and biases
    // OPTIMISTICALLY, which is the direction nobody catches - so when censoring
    // exceeds 1 - p this prints that fact and never a number.
    std::printf("\nturn-to-assembly percentiles, order-statistic 95%%\n");
    for (const double p : {0.10, 0.25, 0.50, 0.75, 0.90}) {
        const cs::Percentile percentile = cs::percentile(run, p);
        std::printf("  P%-3.0f  ", p * 100.0);
        if (!percentile.turn.has_value()) {
            // Never a number (section 10.3). A percentile over only the
            // winners is a lie that looks like data, and it biases
            // optimistically - the direction nobody catches.
            std::printf(">%d       does not exist: %.1f%% never assembled, and this\n"
                        "                  percentile needs the censored fraction under %.0f%%\n",
                        config.turn_cap, 100.0 * run.censored / run.games, 100.0 * (1.0 - p));
            continue;
        }
        std::printf("turn %-3d", *percentile.turn);
        if (percentile.low.has_value() && percentile.high.has_value()) {
            std::printf("  [%d, %d]\n", *percentile.low, *percentile.high);
        } else if (percentile.low.has_value()) {
            std::printf("  [%d, censored]\n", *percentile.low);
        } else {
            std::printf("  [censored, censored]\n");
        }
    }

    // Zero-count patterns FIRST. They are the interesting ones: a declared line
    // that never fires is dead, buggy, shadowed, or resolved intra-turn, and
    // printing it last is how it gets scrolled past (section 5.3).
    std::printf("\npattern mix (never-fired first)\n");
    // Rule 2 of section 9.5 is about a VALUE column - an ablation's
    // goldfish_turn_to_assembly_delta, never "score". This column holds
    // pattern names, so naming it for the metric would be cargo cult.
    std::printf("  %-34s %8s %8s\n", "pattern", "fired", "also-sat");
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < run.fired.size(); ++i) {
            const bool never = run.fired[i] == 0;
            if (never != (pass == 0)) {
                continue;
            }
            const char* note = "";
            if (never) {
                // Satisfied on assembling turns but never winning means SHADOWED
                // by an earlier declaration, which is a pattern-design problem.
                // Never satisfied at all is dead, a modelling bug, or a state
                // the policy creates and resolves inside one turn. Those call
                // for opposite responses, so the report distinguishes them.
                note = run.satisfied[i] > 0 ? "  <- SHADOWED by an earlier pattern"
                                            : "  <- NEVER SATISFIED: dead line, modelling bug,\n"
                                              "                                                "
                                              "         or resolved inside a turn";
            }
            std::printf("  %-34s %8d %8d%s\n", deck.patterns.patterns[i].name.c_str(),
                        run.fired[i], run.satisfied[i], note);
        }
    }

    // Last, and clearly subordinate to the CDF (section 10.5). A mean over a
    // censored distribution is not the mean of anything, which is why it is
    // reported as the mean of the games that finished and labelled that way.
    const int finished = run.games - run.censored;
    if (finished > 0) {
        double total = 0.0;
        for (std::size_t turn = 1; turn < run.assembled_on.size(); ++turn) {
            total += static_cast<double>(turn) * run.assembled_on[turn];
        }
        std::printf("\nsubordinate to the CDF above, and not a substitute for it:\n");
        std::printf("  mean assembling turn %.2f, over the %d games that assembled ONLY.\n",
                    total / finished, finished);
        std::printf("  Not the mean turn-to-assembly: the %d censored games have no value\n",
                    run.censored);
        std::printf("  and dropping them biases this number optimistically.\n");
    }

    std::printf("\n  digest xor %016llx  (identical runs agree here)\n",
                static_cast<unsigned long long>(run.digest_xor));
    static_cast<void>(db);
}

// The parallel driver (SIM_PLAN.md section 15, item 22).
//
// Hands each worker a DISJOINT RANGE OF GAME INDICES and merges the summaries.
// That is the whole of it, and it works only because of INVARIANT S1: game i is
// a pure function of (deck, config, base_seed, i), so which thread runs it and
// in what order cannot matter. Tested directly in tests/unit/test_seeding.cpp
// against both simulate_batch and run_paired, because S1 breaking would not
// error - it would just make these results depend on the machine.
//
// It lives in cli/ rather than core/ because it is orchestration: core stays a
// set of pure functions over ranges, which is what keeps it callable from
// Python later without dragging a thread pool along.
template <typename Run, typename Work>
Run drive(int games, int threads, Work work) {
    if (threads <= 1 || games < threads) {
        return work(0, games);
    }
    std::vector<Run> parts(static_cast<std::size_t>(threads));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    int first = 0;
    for (int t = 0; t < threads; ++t) {
        // The remainder is spread over the first few workers rather than piled
        // on the last one, so no worker ever has more than one extra game.
        const int count = games / threads + (t < games % threads ? 1 : 0);
        workers.emplace_back([&parts, &work, t, first, count] {
            parts[static_cast<std::size_t>(t)] = work(first, count);
        });
        first += count;
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    Run merged;
    for (const Run& part : parts) {
        cs::merge(merged, part);
    }
    return merged;
}

// One ablation, measured both ways (section 10.4).
//
// The coupled arm reuses the baseline's seed sequence; the uncoupled one is
// given a different base seed, so the two runs are independent. Comparing the
// two standard errors is the realised variance reduction - the number section
// 10.4 explicitly refuses to let anyone assume.
struct AblationResult {
    std::string card;
    bool inert = false;
    cs::Difference paired;
    cs::Difference unpaired;
    int baseline_only = 0;   // b: baseline assembled by turn N, ablated did not
    int ablated_only = 0;    // c: the reverse
    double baseline_p = 0.0;
    double ablated_p = 0.0;
};

AblationResult measure_ablation(const cs::CardDb& db, const cs::EffectDb& effects,
                                const cs::io::DeckFile& deck, const cs::GameConfig& config,
                                int slot, int replacement, int games, int threads,
                                std::uint64_t base_seed, int turn) {
    const cs::AblatedDeck arm =
        cs::ablate(db, effects, deck.weights, deck.patterns, slot, replacement);

    const cs::PairedRun coupled = drive<cs::PairedRun>(games, threads, [&](int first, int count) {
        return cs::run_paired(db, effects, deck.weights, arm, deck.patterns, config, base_seed,
                              first, count);
    });

    AblationResult result;
    result.card = db.cards[static_cast<std::size_t>(slot)].listed_name;
    result.inert = effects.by_slot[static_cast<std::size_t>(slot)].status == cs::AuthorStatus::Inert;
    const cs::PairedCounts& cell = coupled.by_turn[static_cast<std::size_t>(turn)];
    result.paired = cs::paired_difference(cell);
    result.baseline_only = cell.baseline_only;
    result.ablated_only = cell.ablated_only;
    const auto n = static_cast<double>(coupled.games);
    result.baseline_p = (cell.both + cell.baseline_only) / n;
    result.ablated_p = (cell.both + cell.ablated_only) / n;

    // The uncoupled arm: same deck, DIFFERENT seed sequence. Nothing else
    // changes, so the only difference between the two intervals below is the
    // coupling.
    const cs::AuthoredPolicy ablated_policy(arm.weights);
    const cs::RunSummary independent =
        drive<cs::RunSummary>(games, threads, [&](int first, int count) {
            return cs::simulate_batch(arm.db, arm.effects, arm.patterns, config, ablated_policy,
                                      base_seed ^ 0x9E3779B97F4A7C15ULL, first, count);
        });
    result.unpaired = cs::unpaired_difference(cell.both + cell.baseline_only, coupled.games,
                                              cs::cumulative(independent, turn), independent.games);
    return result;
}

int simulate(const std::filesystem::path& path, const std::filesystem::path& deck_path,
             const std::filesystem::path& effects_path, int games, std::uint64_t base_seed) {
    cs::CardDb db;
    cs::io::DeckFile deck;
    cs::EffectDb effects;
    try {
        db = cs::io::load_card_db(path);
        deck = cs::io::load_deck(deck_path, db);
        effects = load_effects_for_deck(effects_path, db, deck);
    } catch (const std::runtime_error& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    const cs::AuthoredPolicy policy(deck.weights);
    const cs::GameConfig config = make_config(deck);

    print_header(db, deck, effects, deck_path, games, base_seed);
    const cs::RunSummary run =
        cs::simulate_batch(db, effects, deck.patterns, config, policy, base_seed, 0, games);
    print_report(db, deck, run, config, policy);
    return 0;
}

// Versioned service boundary. Unlike the human report, stdout contains exactly
// one JSON document; all diagnostics go to stderr so a subprocess can parse it
// without stripping banners or progress text.
int simulate_request(const std::filesystem::path& request_path,
                     const std::filesystem::path& cards_path,
                     const std::filesystem::path& pack_path,
                     const std::filesystem::path& effects_path,
                     const std::filesystem::path& output_path, int games,
                     std::uint64_t seed, int objective_turn,
                     const std::string& scenario, bool do_ablation,
                     const std::string& only, int threads) {
    if (games <= 0) {
        std::fprintf(stderr, "error: --request requires --games greater than zero\n");
        return 2;
    }
    if (objective_turn < 1) {
        std::fprintf(stderr, "error: --turn must be at least 1\n");
        return 2;
    }
    if (scenario != "goldfish_assembly.v1") {
        std::fprintf(stderr,
                     "error: unsupported scenario '%s'; supported: goldfish_assembly.v1. "
                     "Controlled disruption is reserved for a future version.\n",
                     scenario.c_str());
        return 2;
    }

    try {
        const cs::io::ServiceRunMetadata started{
            .games = games,
            .seed = seed,
            .objective_turn = objective_turn,
            .scenario_id = cs::io::kGoldfishScenarioId,
            .scenario_version = cs::io::kGoldfishScenarioVersion,
            .started_at = cs::io::utc_timestamp_now(),
            .completed_at = {}};
        const cs::CardDb db = cs::io::load_card_db(cards_path);
        const cs::io::CandidateRequest request = cs::io::load_candidate_request(request_path);
        const std::filesystem::path registry = std::filesystem::is_directory(pack_path)
                                                   ? pack_path
                                                   : pack_path.parent_path();
        const cs::io::StrategyPackSelection selected =
            cs::io::select_strategy_pack(request, registry);
        cs::io::DeckFile pack;
        if (selected.derived) {
            pack = cs::io::load_deck(registry / "derived-generic.defaults.toml", db, true);
            pack.derived = true;
            pack.strategy_pack.supported_commander_oracle_ids = request.commander_oracle_ids;
            pack.strategy_pack.assembly_objectives.clear();
            for (const cs::WinPattern& pattern : pack.patterns.patterns) {
                pack.strategy_pack.assembly_objectives.push_back(pattern.name);
            }
            pack.provenance.cards_sha256 = db.manifest.cards_sha256;
            cs::io::validate_candidate_snapshot(request, db);
        } else {
            pack = cs::io::load_deck(selected.path, db);
            cs::io::validate_candidate_for_pack(request, db, pack);
        }
        cs::EffectDb effects = load_effects_for_deck(effects_path, db, pack);

        const cs::AuthoredPolicy policy(pack.weights);
        const cs::GameConfig config = make_config(pack);
        if (objective_turn > config.turn_cap) {
            std::fprintf(stderr, "error: --turn %d exceeds this pack's turn cap %d\n",
                         objective_turn, config.turn_cap);
            return 2;
        }
        const cs::RunSummary run =
            cs::simulate_batch(db, effects, pack.patterns, config, policy, seed, 0, games);
        std::vector<cs::io::ServiceAblationResult> ablations;
        if (do_ablation) {
            const int replacement = cs::slot_of_listed(db, pack.ablation_replacement);
            if (replacement < 0) {
                throw std::runtime_error("strategy pack's ablation replacement is absent");
            }
            for (const cs::Card& card : db.cards) {
                if (card.is_commander || card.export_index == replacement ||
                    (!only.empty() && card.listed_name != only)) {
                    continue;
                }
                const AblationResult measured = measure_ablation(
                    db, effects, pack, config, card.export_index, replacement, games,
                    threads, seed, objective_turn);
                ablations.push_back({
                    .oracle_id = card.oracle_id,
                    .replacement_oracle_id =
                        db.cards[static_cast<std::size_t>(replacement)].oracle_id,
                    .objective_turn = objective_turn,
                    .delta = measured.paired.delta,
                    .interval_low = measured.paired.low,
                    .interval_high = measured.paired.high});
            }
            if (ablations.empty()) {
                throw std::runtime_error("no ablation target matched the request");
            }
        }
        cs::io::ServiceRunMetadata finished = started;
        finished.completed_at = cs::io::utc_timestamp_now();
        const std::string result = cs::io::build_simulation_result_json(
            request, db, pack, effects, config, run, finished, ablations);

        if (output_path.empty() || output_path == "-") {
            std::fwrite(result.data(), 1, result.size(), stdout);
        } else {
            std::ofstream output(output_path);
            if (!output) {
                std::fprintf(stderr, "error: cannot open JSON output %s\n",
                             output_path.c_str());
                return 1;
            }
            output << result;
            std::fprintf(stderr, "wrote %s\n", output_path.c_str());
        }
        return 0;
    } catch (const std::runtime_error& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}

// The leave-one-out sweep, and the CRN measurement section 10.4 requires.
int sweep(const std::filesystem::path& path, const std::filesystem::path& deck_path,
          const std::filesystem::path& effects_path, int games, std::uint64_t base_seed,
          int threads, int turn, const std::string& only) {
    cs::CardDb db;
    cs::io::DeckFile deck;
    cs::EffectDb effects;
    try {
        db = cs::io::load_card_db(path);
        deck = cs::io::load_deck(deck_path, db);
        effects = load_effects_for_deck(effects_path, db, deck);
    } catch (const std::runtime_error& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    const int replacement = cs::slot_of_listed(db, deck.ablation_replacement);
    if (replacement < 0) {
        std::fprintf(stderr, "error: replacement '%s' is not in the deck\n",
                     deck.ablation_replacement.c_str());
        return 1;
    }
    const cs::GameConfig config = make_config(deck);

    print_header(db, deck, effects, deck_path, games, base_seed);
    std::printf("\nABLATION SWEEP, leave-one-out against %s\n", deck.ablation_replacement.c_str());
    std::printf("  objective: P(assembled by turn %d)  -  section 4.1's default, because the\n",
                turn);
    std::printf("  curve is steepest here and the tail cannot separate opening hands.\n");
    std::printf("  %d games per card, %d threads, common random numbers.\n", games, threads);

    std::vector<int> targets;
    for (const cs::Card& card : db.cards) {
        if (card.is_commander || card.export_index == replacement) {
            continue;  // both refused by ablate(), for reasons it states
        }
        if (!only.empty() && card.listed_name != only) {
            continue;
        }
        targets.push_back(card.export_index);
    }
    if (targets.empty()) {
        std::fprintf(stderr, "error: nothing to ablate%s\n",
                     only.empty() ? "" : (" matching '" + only + "'").c_str());
        return 1;
    }

    std::vector<AblationResult> results;
    results.reserve(targets.size());
    for (const int slot : targets) {
        results.push_back(measure_ablation(db, effects, deck, config, slot, replacement, games,
                                           threads, base_seed, turn));
    }

    // THE VARIANCE-REDUCTION MEASUREMENT (section 10.4), reported before the
    // table, because it is what says whether the intervals in that table are
    // worth reading. 10.4 predicts "often 10x or more" and then says to measure
    // it rather than assume it, so this is the measurement.
    double paired_total = 0.0;
    double unpaired_total = 0.0;
    double worst_ratio = 1e18;
    double best_ratio = 0.0;
    for (const AblationResult& result : results) {
        paired_total += result.paired.standard_error;
        unpaired_total += result.unpaired.standard_error;
        if (result.paired.standard_error > 0.0) {
            const double ratio = result.unpaired.standard_error / result.paired.standard_error;
            worst_ratio = ratio < worst_ratio ? ratio : worst_ratio;
            best_ratio = ratio > best_ratio ? ratio : best_ratio;
        }
    }
    const auto measured = static_cast<double>(results.size());
    std::printf("\nREALISED VARIANCE REDUCTION from common random numbers\n");
    std::printf("  mean standard error, coupled    %.5f\n", paired_total / measured);
    std::printf("  mean standard error, uncoupled  %.5f\n", unpaired_total / measured);
    if (paired_total > 0.0) {
        std::printf("  ratio of standard errors        %.2fx   (variance %.1fx)\n",
                    unpaired_total / paired_total,
                    (unpaired_total / paired_total) * (unpaired_total / paired_total));
    }
    if (results.size() > 1) {
        std::printf("  range across %zu ablations       %.2fx to %.2fx\n", results.size(),
                    worst_ratio, best_ratio);
    }
    std::printf("  A ratio of 1.0 would mean the coupling bought nothing. Getting this\n");
    std::printf("  number at all requires INVARIANT S1, which fails silently.\n");

    // Effect sizes with intervals, never a significance verdict (section 10.4,
    // decision 1). A ranked list of "significant" findings at this many
    // comparisons is mostly false positives dressed as discoveries, in a format
    // that hides which is which. An interval carries its own uncertainty.
    std::sort(results.begin(), results.end(),
              [](const AblationResult& a, const AblationResult& b) {
                  if (a.paired.delta != b.paired.delta) {
                      return a.paired.delta > b.paired.delta;
                  }
                  return a.card < b.card;
              });

    // THE MEASURED NULL, and it is measurable only because this deck declares 31
    // cards that do nothing (section 4.4).
    //
    // Section 9.4 says the replacement bias is "inherent in choosing any
    // replacement" and "not removable", and that is true - but it is not
    // unmeasurable. Ablating an INERT card swaps a card that does nothing for a
    // Forest, so its delta is exactly the value of that swap and nothing else.
    // The inert set is therefore a control group of 31, and its mean is the
    // baseline bias in the units of the table below.
    //
    // Without it the table is unreadable: at turn 3 a Forest beats most of this
    // deck, so nearly every nonland reads negative and a reader has no way to
    // tell "worse than a land" from "worse than nothing".
    double inert_total = 0.0;
    int inert_count = 0;
    double inert_low = 1e18;
    double inert_high = -1e18;
    for (const AblationResult& result : results) {
        if (!result.inert) {
            continue;
        }
        ++inert_count;
        inert_total += result.paired.delta;
        inert_low = result.paired.delta < inert_low ? result.paired.delta : inert_low;
        inert_high = result.paired.delta > inert_high ? result.paired.delta : inert_high;
    }
    // Fewer than a handful of inert cards is not a null, it is a coincidence.
    // With --ablate <one card> there are usually zero, and a `vs blank` column
    // computed from a null of 0.0 would print the raw delta under a heading
    // saying it was something else - the ninth rule's shape again, in a column
    // header this time.
    const bool have_null = inert_count >= 5;
    const double null_delta = have_null ? inert_total / inert_count : 0.0;
    if (have_null) {
        std::printf("\nTHE MEASURED NULL, from the %d inert cards\n", inert_count);
        std::printf("  mean delta of a card declared to do nothing  %+.3f%%\n",
                    100.0 * null_delta);
        std::printf("  range across those %d                        %+.3f%% to %+.3f%%\n",
                    inert_count, 100.0 * inert_low, 100.0 * inert_high);
        std::printf("  This IS section 9.4's replacement bias, in the units below: what a\n");
        std::printf("  %s is worth over a blank card at turn %d. The `vs blank` column\n",
                    deck.ablation_replacement.c_str(), turn);
        std::printf("  subtracts it, and a card sitting at 0.000 there is doing nothing this\n");
        std::printf("  deck can measure by turn %d.\n", turn);
        // The spread matters more than the mean's own precision, and saying so
        // is what stops the re-centred column reading as exact.
        std::printf("\n  READ `vs blank` WITH THAT RANGE, NOT WITH THE INTERVAL. The 31 nulls\n");
        std::printf("  spread %.3f points, which is wider than any single interval below:\n",
                    100.0 * (inert_high - inert_low));
        std::printf("  a cheap blank gets cast and wastes mana, an expensive one never does,\n");
        std::printf("  so \"a blank card\" is not one number. The interval is on `delta`.\n");
        std::printf("  Treat anything inside +/-%.3f of zero on `vs blank` as not\n",
                    100.0 * (inert_high - inert_low) / 2.0);
        std::printf("  distinguished from doing nothing.\n");
    }

    std::printf("\ngoldfish_turn_to_assembly_delta at turn %d, paired 95%%\n", turn);
    if (!have_null) {
        std::printf("  No `vs blank` column: it needs the inert cards as a null and this run\n");
        std::printf("  ablated %d card%s. Run --sweep for it. The delta below is against %s,\n",
                    static_cast<int>(results.size()), results.size() == 1 ? "" : "s",
                    deck.ablation_replacement.c_str());
        std::printf("  which is worth roughly 0.4 points over a blank card at turn 3.\n");
    }
    std::printf("  the column is named for the metric on purpose: a sorted table headed\n");
    std::printf("  'score' IS a card-quality ranking whatever the banner said (section 9.5).\n\n");
    // b and c printed separately, not summed: the Wald interval above rests on
    // the discordant pairs being numerous enough for the normal approximation,
    // and a reader cannot check that from a total. It is also where a suspicious
    // result shows itself - a delta built from b=3, c=0 is not a measurement.
    std::printf("  %-28s %9s %9s  %-18s %7s %7s\n", "card", "delta",
                have_null ? "vs blank" : "", "95% interval", "b", "c");
    for (const AblationResult& result : results) {
        const bool excludes_zero = result.paired.low > 0.0 || result.paired.high < 0.0;
        char centred[16] = "        -";
        if (have_null) {
            std::snprintf(centred, sizeof(centred), "%+8.3f%%",
                          100.0 * (result.paired.delta - null_delta));
        }
        std::printf("  %-28s %+8.3f%% %9s  [%+6.3f, %+6.3f] %7d %7d%s%s\n",
                    result.card.c_str(), 100.0 * result.paired.delta, centred,
                    100.0 * result.paired.low, 100.0 * result.paired.high, result.baseline_only,
                    result.ablated_only, excludes_zero ? "  *" : "",
                    result.inert ? "  (inert)" : "");
    }
    std::printf("\n  * interval excludes zero. NOT a significance verdict: at %zu comparisons\n",
                results.size());
    std::printf("  some of these are noise, and section 10.4 says Benjamini-Hochberg if a\n");
    std::printf("  threshold is ever needed. The intervals are the output; the star is a\n");
    std::printf("  reading aid.\n");
    return 0;
}

// OPTION B (SIM_PLAN.md §13.1): sampled hands, reported raw.
//
// Deliberately NOT a chart. It is a list of sampled opening hands with a value
// each, and its entire purpose is to answer the question that decides whether
// option A is worth building: **does anything separate at all?** If the values
// of randomly sampled hands are indistinguishable from each other, a feature
// grid over them is a grid of noise and the feature design was wasted.
//
// So it prints the hands and gets out of the way. Any structure a reader sees
// here is structure that exists; any they do not see is not there to be gridded.
int hands(const std::filesystem::path& path, const std::filesystem::path& deck_path,
          const std::filesystem::path& effects_path, int sample, int games,
          std::uint64_t base_seed, int threads, int turn) {
    cs::CardDb db;
    cs::io::DeckFile deck;
    cs::EffectDb effects;
    try {
        db = cs::io::load_card_db(path);
        deck = cs::io::load_deck(deck_path, db);
        effects = load_effects_for_deck(effects_path, db, deck);
    } catch (const std::runtime_error& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    const cs::AuthoredPolicy policy(deck.weights);
    const cs::GameConfig config = make_config(deck);

    print_header(db, deck, effects, deck_path, games, base_seed);
    std::printf("\nSAMPLED OPENING HANDS - raw, not a chart (section 13.1, option B)\n");
    std::printf("  %d hands, %d games each, keeping every hand. The question this answers\n",
                sample, games);
    std::printf("  is whether anything separates: if these values are all alike, a feature\n");
    std::printf("  grid over them would be a grid of noise.\n");
    std::printf("  value = P(assembled by turn %d | this hand), Wilson 95%%.\n", turn);
    std::printf("\n  NOT a keep/mull recommendation. A mulligan decision compares a hand\n");
    std::printf("  against the EXPECTATION OVER MULLIGANING, which is a different number\n");
    std::printf("  and is not computed here (section 13.1).\n");

    int commander = -1;
    for (const cs::Card& card : db.cards) {
        if (card.is_commander) {
            commander = card.export_index;
        }
    }

    // Hands are sampled from a stream INDEPENDENT of the per-game stream, so
    // hand h is the same hand whatever `games` is set to. Without that, changing
    // the games-per-hand would silently resample the hands and two runs would
    // not be comparable.
    cs::Rng hand_rng(cs::seed_for_game(base_seed, 0xADDED0ULL));
    struct Sampled {
        cs::Zone hand;
        int lands = 0;
        int reached = 0;
        int games = 0;
        std::string pattern;  // the one that fired most for this hand
    };
    std::vector<Sampled> results;
    results.reserve(static_cast<std::size_t>(sample));

    for (int h = 0; h < sample; ++h) {
        Sampled row;
        row.hand = cs::sample_hand(static_cast<int>(db.size()), commander,
                                   config.opening_hand, hand_rng);
        row.hand.for_each([&](int slot) {
            row.lands += db.cards[static_cast<std::size_t>(slot)].plays_as_land() ? 1 : 0;
        });
        // Every hand gets the SAME game-index range, so the library shuffles are
        // common random numbers across hands and the comparison between two rows
        // is paired for free (section 10.4).
        const cs::Zone fixed = row.hand;
        const cs::RunSummary run =
            drive<cs::RunSummary>(games, threads, [&](int first, int count) {
                return cs::simulate_batch(db, effects, deck.patterns, config, policy, base_seed,
                                          first, count, &fixed);
            });
        row.reached = cs::cumulative(run, turn);
        row.games = run.games;
        // WHICH pattern, not just how often. A hand at 97% is a claim worth
        // being able to check, and "it assembles" does not say what it
        // assembled - which is the difference between a fast hand and a
        // modelling bug.
        int most = -1;
        for (std::size_t p = 0; p < run.fired.size(); ++p) {
            if (most < 0 || run.fired[p] > run.fired[static_cast<std::size_t>(most)]) {
                most = static_cast<int>(p);
            }
        }
        row.pattern = most >= 0 && run.fired[static_cast<std::size_t>(most)] > 0
                          ? deck.patterns.patterns[static_cast<std::size_t>(most)].name
                          : "-";
        results.push_back(row);
    }

    std::sort(results.begin(), results.end(), [](const Sampled& a, const Sampled& b) {
        return static_cast<double>(a.reached) / a.games > static_cast<double>(b.reached) / b.games;
    });

    std::printf("\n  %5s %6s  %-14s %-28s %s\n", "value", "lands", "95% interval",
                "mostly assembles", "hand");
    for (const Sampled& row : results) {
        const cs::Interval interval = cs::wilson(row.reached, row.games);
        std::printf("  %5.1f%% %6d  [%4.1f, %4.1f] %-28s ", 100.0 * row.reached / row.games,
                    row.lands, 100.0 * interval.low, 100.0 * interval.high,
                    row.pattern.c_str());
        bool first = true;
        row.hand.for_each([&](int slot) {
            std::printf("%s%s", first ? "" : ", ",
                        db.cards[static_cast<std::size_t>(slot)].listed_name.c_str());
            first = false;
        });
        std::printf("\n");
    }

    // The separation question, answered numerically rather than left to the eye.
    double best = 0.0;
    double worst = 1.0;
    double total = 0.0;
    for (const Sampled& row : results) {
        const double value = static_cast<double>(row.reached) / row.games;
        best = value > best ? value : best;
        worst = value < worst ? value : worst;
        total += value;
    }
    const double mean = total / static_cast<double>(results.size());
    const cs::Interval typical = cs::wilson(static_cast<int>(mean * games), games);
    std::printf("\n  DOES ANYTHING SEPARATE?\n");
    std::printf("    best hand      %5.1f%%\n", 100.0 * best);
    std::printf("    worst hand     %5.1f%%\n", 100.0 * worst);
    std::printf("    spread         %5.1f points\n", 100.0 * (best - worst));
    std::printf("    one hand's own 95%% interval is about %.1f points wide\n",
                100.0 * (typical.high - typical.low));
    if ((best - worst) > 3.0 * (typical.high - typical.low)) {
        std::printf("    -> the spread is several times a single hand's interval, so hands\n");
        std::printf("       DO separate and a feature grid (option A) has something to fit.\n");
    } else {
        std::printf("    -> the spread is comparable to one hand's interval, so this sample\n");
        std::printf("       shows NO separation and option A would be gridding noise.\n");
    }
    return 0;
}

// OPTION A (SIM_PLAN.md §13.1): the keep/mull feature grid.
//
// THE FEATURES ARE THE PRIMER'S, NOT MINE. §13.1 says the feature set is the
// whole difficulty and that a chart over the wrong features is unreadable. The
// deck's published primer states its own mulligan heuristics in plain language,
// and using them means the grid answers a question its readers already ask,
// rather than one the model found convenient:
//
//   "our number 1 priority is definitely mana production"
//   "preferably ramp that we can play on turn 1"
//   "we are a deck focused around producing 7 mana quickly"
//   "this is NOT a deck where you want to keep the 'all interaction hand'"
//   "we love a hand that gives a game plan ... a creature tutor, a threat to
//    ramp into, or a tutor for Basalt Monolith"
//
// Four features, each observable BEFORE the mulligan (§13.1's first condition -
// a feature a player cannot evaluate while holding the cards is not a decision
// rule), and each derived from declared data rather than from authored ranks:
//
//   sources     mana sources in hand: lands, rocks, dorks, rituals
//   t1_ramp     a NONLAND source castable on turn one (mana value <= 1)
//   colour      the hand's own sources can pay {G}{U} - asked through can_pay,
//               so "can this hand cast the commander" has one definition
//   payoff      a tutor, or a card named by a declared pattern or engine
//
// THE HANDS FROM THE PRIMER ARE HELD OUT. They are worked examples with stated
// reasoning and they are the only external validation set this project will get;
// scoring them against a grid fitted with them in view would answer nothing.
struct HandFeatures {
    int sources = 0;
    bool t1_ramp = false;
    bool colour = false;
    bool payoff = false;

    [[nodiscard]] int source_bucket() const noexcept {
        return sources <= 1 ? 0 : (sources >= 5 ? 4 : sources - 1);
    }
    [[nodiscard]] std::size_t cell() const noexcept {
        return static_cast<std::size_t>(((source_bucket() * 2 + (t1_ramp ? 1 : 0)) * 2 +
                                         (colour ? 1 : 0)) * 2 + (payoff ? 1 : 0));
    }
};

HandFeatures features_of(const cs::Zone& hand, const cs::CardDb& db, const cs::EffectDb& effects,
                         const cs::PatternSet& patterns) {
    HandFeatures f;
    // Cards named by any declared pattern or engine. Declared structure, not
    // policy: using authored ranks here would make the grid a picture of the
    // scorer rather than of the deck.
    cs::Zone named;
    const auto add = [&named](const cs::Requirement& r) {
        named = named | r.in_play | r.in_hand | r.in_play_or_hand | r.untapped;
        if (r.has_any_of) {
            named = named | r.any_of;
        }
    };
    for (const cs::Engine& e : patterns.engines) {
        add(e.requires_);
    }
    for (const cs::WinPattern& w : patterns.patterns) {
        add(w.requires_);
    }

    std::vector<cs::Source> from_hand;
    hand.for_each([&](int slot) {
        const cs::Card& card = db.cards[static_cast<std::size_t>(slot)];
        const cs::CardEffects& entry = effects.by_slot[static_cast<std::size_t>(slot)];
        const bool land = card.plays_as_land();
        const bool produces = entry.has_mana_source || entry.has_ritual;
        if (land || produces) {
            ++f.sources;
            // What this hand could produce with everything deployed. An
            // approximation of a real curve - lands need drops - but it is a
            // property of the HAND, which is what a mulligan decision has.
            cs::Source source;
            if (entry.has_ritual) {
                source.produces = entry.ritual.produces;
                source.amount = entry.ritual.amount;
            } else if (entry.has_mana_source) {
                source.produces = entry.mana_source.colours_from_table ? 0x1F
                                                                       : entry.mana_source.produces;
                source.amount = entry.mana_source.amount;
                source.is_land = entry.mana_source.is_land;
            }
            from_hand.push_back(source);
        }
        if (produces && !land && card.mana_value <= 1) {
            f.t1_ramp = true;
        }
        if (entry.has_tutor || named.test(slot)) {
            f.payoff = true;
        }
    });
    // {G}{U} through the deck's own mana system, not a colour-counting shortcut:
    // a source produces `amount` mana all of ONE colour (§2.6), so one dual is
    // not {G}{U} and a shortcut would say it is.
    cs::Cost kinnan;
    kinnan.pips[static_cast<std::size_t>(cs::Colour::Green)] = 1;
    kinnan.pips[static_cast<std::size_t>(cs::Colour::Blue)] = 1;
    f.colour = cs::can_pay(kinnan, from_hand, 0);
    return f;
}

int grid(const std::filesystem::path& path, const std::filesystem::path& deck_path,
         const std::filesystem::path& effects_path, int sample, int games,
         std::uint64_t base_seed, int threads, int turn) {
    cs::CardDb db;
    cs::io::DeckFile deck;
    cs::EffectDb effects;
    try {
        db = cs::io::load_card_db(path);
        deck = cs::io::load_deck(deck_path, db);
        effects = load_effects_for_deck(effects_path, db, deck);
    } catch (const std::runtime_error& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    const cs::AuthoredPolicy policy(deck.weights);
    const cs::GameConfig config = make_config(deck);
    print_header(db, deck, effects, deck_path, games, base_seed);

    int commander = -1;
    for (const cs::Card& card : db.cards) {
        if (card.is_commander) {
            commander = card.export_index;
        }
    }

    constexpr std::size_t kCells = 5 * 2 * 2 * 2;
    struct Cell {
        int hands = 0;
        int games = 0;
        int reached = 0;      // by the objective turn
        int reached_late = 0; // by the turn cap
    };
    std::vector<Cell> cells(kCells);

    cs::Rng hand_rng(cs::seed_for_game(base_seed, 0xADDED0ULL));
    for (int h = 0; h < sample; ++h) {
        const cs::Zone hand =
            cs::sample_hand(static_cast<int>(db.size()), commander, config.opening_hand, hand_rng);
        const HandFeatures f = features_of(hand, db, effects, deck.patterns);
        const cs::RunSummary run = drive<cs::RunSummary>(games, threads, [&](int first, int count) {
            return cs::simulate_batch(db, effects, deck.patterns, config, policy, base_seed, first,
                                      count, &hand);
        });
        Cell& cell = cells[f.cell()];
        ++cell.hands;
        cell.games += run.games;
        cell.reached += cs::cumulative(run, turn);
        cell.reached_late += cs::cumulative(run, config.turn_cap);
    }

    std::printf("\nKEEP/MULL FEATURE GRID (section 13.1, option A)\n");
    std::printf("  %d sampled hands, %d games each, features taken from the deck's own primer.\n",
                sample, games);
    std::printf("\n  READ THE TURN ON EVERY CELL. A LOW NUMBER MEANS \"DOES NOT ACT BY THAT\n");
    std::printf("  TURN\", NOT \"IS BAD\". Section 4.1: an early-turn objective is what\n");
    std::printf("  separates opening hands, and it is structurally unable to see a mid-game\n");
    std::printf("  engine piece - Enduring Vitality is +0.5 at turn 3 and +29 at turn 12.\n");
    std::printf("  The ordering below is by turn %d alone; turn %d is printed beside it.\n", turn,
                config.turn_cap);
    std::printf("\n  NOT a keep/mull recommendation. A mulligan decision compares a hand to the\n");
    std::printf("  EXPECTATION OVER MULLIGANING, which is not computed here (section 13.1).\n");

    std::printf("\n  %-5s %-3s %-3s %-3s %6s %6s   %-16s %8s\n", "src", "t1", "GU", "pay", "hands",
                "T", "95% interval", "T12");
    // Ordered by the objective, most valuable first - but the ordering key is
    // printed in the header so it cannot be mistaken for a ranking of cards.
    std::vector<std::size_t> order(kCells);
    for (std::size_t i = 0; i < kCells; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const double va = cells[a].games > 0
                              ? static_cast<double>(cells[a].reached) / cells[a].games : -1.0;
        const double vb = cells[b].games > 0
                              ? static_cast<double>(cells[b].reached) / cells[b].games : -1.0;
        return va > vb;
    });
    static constexpr const char* kBuckets[] = {"0-1", "2", "3", "4", "5+"};
    for (const std::size_t i : order) {
        const Cell& cell = cells[i];
        if (cell.hands == 0) {
            continue;
        }
        const std::size_t payoff = i % 2;
        const std::size_t colour = (i / 2) % 2;
        const std::size_t ramp = (i / 4) % 2;
        const std::size_t bucket = i / 8;
        const cs::Interval interval = cs::wilson(cell.reached, cell.games);
        // CELL COUNTS ARE PRINTED (§13.1's second condition). A cell holding four
        // sampled hands is noise with a number on it, and a reader cannot tell
        // that from the value alone.
        std::printf("  %-5s %-3s %-3s %-3s %6d %5.1f%%   [%5.1f, %5.1f]  %6.1f%%%s\n",
                    kBuckets[bucket], ramp ? "yes" : "-", colour ? "yes" : "-",
                    payoff ? "yes" : "-", cell.hands, 100.0 * cell.reached / cell.games,
                    100.0 * interval.low, 100.0 * interval.high,
                    100.0 * cell.reached_late / cell.games,
                    cell.hands < 20 ? "   <- thin" : "");
    }
    // THE INVERSION COUNT, computed rather than asserted, and printed with the
    // table rather than in a document. If the two columns order the same cells
    // differently then "which hand is better" is not a question the chart
    // answers - it answers "better by turn N", and a reader comparing two rows
    // has to see that before drawing a conclusion from the comparison.
    {
        std::size_t inversions = 0;
        std::size_t comparable = 0;
        std::size_t worst_a = kCells;
        std::size_t worst_b = kCells;
        double worst_gap = 0.0;
        for (std::size_t a = 0; a < kCells; ++a) {
            for (std::size_t b = a + 1; b < kCells; ++b) {
                // Thin cells are excluded: an inversion between two cells of
                // four hands each is noise, and counting it would inflate the
                // very finding this is here to state honestly.
                if (cells[a].hands < 20 || cells[b].hands < 20) {
                    continue;
                }
                ++comparable;
                const double early_a = static_cast<double>(cells[a].reached) / cells[a].games;
                const double early_b = static_cast<double>(cells[b].reached) / cells[b].games;
                const double late_a = static_cast<double>(cells[a].reached_late) / cells[a].games;
                const double late_b = static_cast<double>(cells[b].reached_late) / cells[b].games;
                if ((early_a > early_b) == (late_a > late_b)) {
                    continue;
                }
                ++inversions;
                const double gap = std::abs(late_a - late_b);
                if (gap > worst_gap) {
                    worst_gap = gap;
                    worst_a = early_a > early_b ? a : b;
                    worst_b = early_a > early_b ? b : a;
                }
            }
        }
        const auto describe = [&](std::size_t i) {
            std::printf("%-4s t1=%-3s GU=%-3s pay=%-3s", kBuckets[i / 8], ((i / 4) % 2) ? "y" : "n",
                        ((i / 2) % 2) ? "y" : "n", (i % 2) ? "y" : "n");
        };
        if (comparable > 0) {
            std::printf("\n  THE TWO COLUMNS DISAGREE ABOUT WHICH HAND IS BETTER, in %zu of %zu\n",
                        inversions, comparable);
            std::printf("  cell pairs (%.0f%%). THIS IS A PROPERTY OF THE CHART, NOT A CAVEAT:\n",
                        100.0 * static_cast<double>(inversions) / static_cast<double>(comparable));
            std::printf("  \"which hand is better\" HAS NO ANSWER HERE without naming the turn.\n");
            std::printf("  Before you compare two rows, check that the comparison survives both\n");
            std::printf("  columns; where it does not, the ranking you take away is the one you\n");
            std::printf("  chose an objective for.\n");
        }
        if (worst_a < kCells) {
            std::printf("\n  The widest disagreement:\n    ");
            describe(worst_a);
            std::printf("   turn %-2d %5.1f%%  ->  turn %d %5.1f%%\n", turn,
                        100.0 * cells[worst_a].reached / cells[worst_a].games, config.turn_cap,
                        100.0 * cells[worst_a].reached_late / cells[worst_a].games);
            std::printf("    ");
            describe(worst_b);
            std::printf("   turn %-2d %5.1f%%  ->  turn %d %5.1f%%\n", turn,
                        100.0 * cells[worst_b].reached / cells[worst_b].games, config.turn_cap,
                        100.0 * cells[worst_b].reached_late / cells[worst_b].games);
            std::printf("    The first is ahead by turn %d and behind by %.0f points at turn %d.\n",
                        turn, 100.0 * worst_gap, config.turn_cap);
        }
    }

    std::printf("\n  src = mana sources in hand (lands, rocks, dorks, rituals)\n");
    std::printf("  t1  = a NONLAND source castable turn one\n");
    std::printf("  GU  = the hand's own sources can pay {G}{U}, asked through can_pay\n");
    std::printf("  pay = a tutor, or a card named by a declared pattern or engine\n");
    std::printf("  T   = P(assembled by turn %d | this hand). T12 = by turn %d, for context.\n",
                turn, config.turn_cap);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        print_view("%.*s", cs::version());
        std::printf(" %.*s\n", static_cast<int>(cs::build_flavour().size()),
                    cs::build_flavour().data());
        return 0;
    }

    std::filesystem::path path{"data/cards.json"};
    std::filesystem::path deck_path{"data/kinnan.deck.toml"};
    std::filesystem::path effects_path{"data/effects.toml"};
    std::filesystem::path request_path;
    std::filesystem::path output_json{"-"};
    std::string scenario{"goldfish_assembly.v1"};
    int games = 0;
    long long trace_seed = -1;
    std::uint64_t seed = 1;
    bool do_sweep = false;
    int sampled_hands = 0;
    int grid_hands = 0;
    std::string only;
    // Section 4.1: the default objective is an early-turn CDF point, because the
    // tail cannot separate opening hands and this tool is a mulligan solver's
    // value function.
    int objective_turn = 3;
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    threads = threads > 0 ? threads : 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--games") == 0 && i + 1 < argc) {
            games = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_seed = std::atoll(argv[++i]);
        } else if (std::strcmp(argv[i], "--deck") == 0 && i + 1 < argc) {
            deck_path = argv[++i];
        } else if (std::strcmp(argv[i], "--request") == 0 && i + 1 < argc) {
            request_path = argv[++i];
        } else if (std::strcmp(argv[i], "--cards") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (std::strcmp(argv[i], "--output-json") == 0 && i + 1 < argc) {
            output_json = argv[++i];
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (std::strcmp(argv[i], "--effects") == 0 && i + 1 < argc) {
            effects_path = argv[++i];
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--grid") == 0 && i + 1 < argc) {
            grid_hands = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--hands") == 0 && i + 1 < argc) {
            sampled_hands = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--sweep") == 0) {
            do_sweep = true;
        } else if (std::strcmp(argv[i], "--ablate") == 0 && i + 1 < argc) {
            do_sweep = true;
            only = argv[++i];
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--turn") == 0 && i + 1 < argc) {
            objective_turn = std::atoi(argv[++i]);
        } else {
            path = argv[i];
        }
    }

    if (!request_path.empty()) {
        return simulate_request(request_path, path, deck_path, effects_path, output_json,
                                games, seed, objective_turn, scenario, do_sweep, only,
                                threads);
    }

    const auto v = cs::version();
    const auto flavour = cs::build_flavour();
    std::printf("commander_simulator ");
    print_view("%.*s", v);
    std::printf(" (");
    print_view("%.*s", flavour);
    std::printf(" build)\n\n");

    // The metric banner is FIRST and unconditional (section 9.5, rule 1) - so
    // the card-database summary, which is numbers, cannot precede it. It used
    // to, for three phases: every run opened with "100 cards, 25 with a land
    // face, 76 castable" and only then said what was being measured.
    if (trace_seed >= 0) {
        return trace_one(path, deck_path, effects_path, static_cast<std::uint64_t>(trace_seed));
    }
    if (grid_hands > 0) {
        return grid(path, deck_path, effects_path, grid_hands, games > 0 ? games : 300,
                    seed, threads, objective_turn);
    }
    if (sampled_hands > 0) {
        return hands(path, deck_path, effects_path, sampled_hands, games > 0 ? games : 4000, seed,
                     threads, objective_turn);
    }
    if (do_sweep) {
        return sweep(path, deck_path, effects_path, games > 0 ? games : 20000, seed, threads,
                     objective_turn, only);
    }
    if (games > 0) {
        return simulate(path, deck_path, effects_path, games, seed);
    }
    return summarise(path);
}

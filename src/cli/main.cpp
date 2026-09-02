// Loads a card database and prints what it found.
//
// The first end-to-end path: exporter -> cards.json -> C++ -> a summary a human
// can check against the database. Argument parsing, the metric banner and the
// statistics arrive in Phase 6 (SIM_PLAN.md section 9.5).

#include <cstdio>
#include <filesystem>
#include <string>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/card.hpp"
#include "core/rng.hpp"
#include "core/sim.hpp"
#include "core/stats.hpp"
#include "core/version.hpp"
#include "io/card_db_load.hpp"
#include "io/deck_load.hpp"
#include "io/effects_load.hpp"
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

int trace_one(const std::filesystem::path& path, const std::filesystem::path& deck_path,
              const std::filesystem::path& effects_path, std::uint64_t seed) {
    cs::CardDb db;
    cs::io::DeckFile deck;
    cs::EffectDb effects;
    try {
        db = cs::io::load_card_db(path);
        deck = cs::io::load_deck(deck_path, db);
        effects = cs::io::load_effects(effects_path, db);
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
    std::printf("data: manifest %s (%s)\n", db.manifest.cards_sha256.substr(0, 8).c_str(),
                db.manifest.max_content_updated_at.substr(0, 10).c_str());

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

    if (effects.unauthored > 0) {
        std::printf("  - %d of %zu cards are UNAUTHORED. They are drawn and dilute every draw,\n"
                    "    but do nothing when cast, which UNDERSTATES the deck.\n",
                    effects.unauthored, db.size());
    }
}

void print_report(const cs::CardDb& db, const cs::io::DeckFile& deck, const cs::RunSummary& run,
                  const cs::GameConfig& config, const cs::Policy& policy) {
    std::printf("\npolicy: %s\n", policy.name());
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

int simulate(const std::filesystem::path& path, const std::filesystem::path& deck_path,
             const std::filesystem::path& effects_path, int games, std::uint64_t base_seed) {
    cs::CardDb db;
    cs::io::DeckFile deck;
    cs::EffectDb effects;
    try {
        db = cs::io::load_card_db(path);
        deck = cs::io::load_deck(deck_path, db);
        effects = cs::io::load_effects(effects_path, db);
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

int main(int argc, char** argv) {
    const auto v = cs::version();
    const auto flavour = cs::build_flavour();
    std::printf("commander_simulator ");
    print_view("%.*s", v);
    std::printf(" (");
    print_view("%.*s", flavour);
    std::printf(" build)\n\n");

    std::filesystem::path path{"data/cards.json"};
    std::filesystem::path deck_path{"data/kinnan.deck.toml"};
    std::filesystem::path effects_path{"data/effects.toml"};
    int games = 0;
    long long trace_seed = -1;
    std::uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--games") == 0 && i + 1 < argc) {
            games = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_seed = std::atoll(argv[++i]);
        } else if (std::strcmp(argv[i], "--deck") == 0 && i + 1 < argc) {
            deck_path = argv[++i];
        } else if (std::strcmp(argv[i], "--effects") == 0 && i + 1 < argc) {
            effects_path = argv[++i];
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else {
            path = argv[i];
        }
    }

    // The metric banner is FIRST and unconditional (section 9.5, rule 1) - so
    // the card-database summary, which is numbers, cannot precede it. It used
    // to, for three phases: every run opened with "100 cards, 25 with a land
    // face, 76 castable" and only then said what was being measured.
    if (trace_seed >= 0) {
        return trace_one(path, deck_path, effects_path, static_cast<std::uint64_t>(trace_seed));
    }
    if (games > 0) {
        return simulate(path, deck_path, effects_path, games, seed);
    }
    return summarise(path);
}

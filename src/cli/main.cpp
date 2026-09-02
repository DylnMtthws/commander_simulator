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

    std::printf("\ntrace: seed %llu, policy %s\n",
                static_cast<unsigned long long>(seed), policy.name());
    std::printf("  scores shown are rank*1000 plus state-dependent terms; rejected\n");
    std::printf("  candidates are listed so the ranking can be disagreed with.\n");
    std::printf("\n  WARNING: mana sources are STUB-QUALITY until Phase 7. Only cards with a\n");
    std::printf("  LAND face produce mana, so every rock and dork on the board produces\n");
    std::printf("  nothing, and each land wrongly taps for any colour. The POLICY's\n");
    std::printf("  decisions below are real; the mana it decides against is not.\n");
    static_cast<void>(cs::run_game(db, effects, deck.patterns, config, policy, seed, &writer));
    return 0;
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

    std::uint64_t can_pay_calls = 0;
    std::uint64_t digest_mix = 0;
    int censored = 0;
    const std::size_t pattern_count = deck.patterns.patterns.size();
    std::vector<int> fired(pattern_count, 0);
    std::vector<int> satisfied(pattern_count, 0);
    std::vector<int> by_turn(config.turn_cap + 1, 0);

    for (int i = 0; i < games; ++i) {
        const cs::GameResult result =
            cs::run_game(db, effects, deck.patterns, config, policy,
                         cs::seed_for_game(base_seed, static_cast<std::uint64_t>(i)));
        can_pay_calls += result.stats.can_pay_calls;
        digest_mix ^= result.state_digest;
        if (result.outcome.censored()) {
            ++censored;
        } else {
            ++fired[result.outcome.pattern_id];
            ++by_turn[*result.outcome.assembled_turn];
            for (std::size_t p = 0; p < pattern_count; ++p) {
                if ((result.outcome.satisfied_mask & (cs::FlagMask{1} << p)) != 0) {
                    ++satisfied[p];
                }
            }
        }
    }

    std::printf("\nMETRIC: goldfish turns-to-assembly. NOT deck strength, win rate, or card\n");
    std::printf("        quality. A faster number is not a better deck.\n");
    std::printf("\n%d games, seed %llu\n  policy: %s\n", games,
                static_cast<unsigned long long>(base_seed), policy.name());
    std::printf("  can_pay calls/game %.1f\n", static_cast<double>(can_pay_calls) / games);

    // The grouped table, not the bare count. A count says how much the model
    // cannot see; the categories say WHAT, and if `interaction` dominates the
    // fix is opposition profiles rather than a bigger card model (section 4.4).
    std::printf("\nWHAT THE MODEL CANNOT SEE  (%d inert cards, by reason)\n", effects.inert);
    for (std::size_t i = 0; i < effects.inert_categories.size(); ++i) {
        std::printf("  %-22s %4d\n", effects.inert_categories[i].c_str(), effects.inert_counts[i]);
    }
    if (effects.unauthored > 0) {
        std::printf("\n  %d of %zu cards are UNAUTHORED (Phase 7 incomplete). They are drawn\n",
                    effects.unauthored, db.size());
        std::printf("  and dilute every draw, but do nothing when cast.\n");
    }

    std::printf("\nP(assembled by turn N)\n");
    int cumulative = 0;
    for (int turn = 1; turn <= config.turn_cap; ++turn) {
        cumulative += by_turn[static_cast<std::size_t>(turn)];
        if (cumulative > 0) {
            std::printf("  turn %-3d %6.2f%%\n", turn, 100.0 * cumulative / games);
        }
    }
    if (cumulative == 0) {
        std::printf("  (nothing assembled in any game)\n");
    }
    std::printf("  censored %.2f%% (%d games never assembled)\n", 100.0 * censored / games,
                censored);

    // Zero-count patterns FIRST. They are the interesting ones: a declared line
    // that never fires is either dead or a modelling bug, and printing it last
    // is how it gets scrolled past (SIM_PLAN.md section 5.3).
    std::printf("\npattern mix (never-fired first)\n");
    std::printf("  %-34s %8s %8s\n", "", "fired", "also-sat");
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < pattern_count; ++i) {
            const bool never = fired[i] == 0;
            if (never != (pass == 0)) {
                continue;
            }
            const char* note = "";
            if (never) {
                // A pattern satisfied on assembling turns but never winning is
                // SHADOWED by an earlier declaration, which is a pattern-design
                // problem. One never satisfied at all is dead or a bug. Those
                // call for opposite responses, so the report distinguishes them.
                note = satisfied[i] > 0 ? "  <- SHADOWED by an earlier pattern"
                                        : "  <- NEVER SATISFIED: dead line or modelling bug";
            }
            std::printf("  %-34s %8d %8d%s\n", deck.patterns.patterns[i].name.c_str(), fired[i],
                        satisfied[i], note);
        }
    }
    std::printf("\n  digest xor %016llx  (identical runs agree here)\n",
                static_cast<unsigned long long>(digest_mix));
    std::printf("\n  NOTE: the stub policy is not the real one, and effects are not\n");
    std::printf("        authored until Phase 7. These numbers are about the LOOP.\n");
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

    const int status = summarise(path);
    if (status != 0) {
        return status;
    }
    if (trace_seed >= 0) {
        return trace_one(path, deck_path, effects_path, static_cast<std::uint64_t>(trace_seed));
    }
    if (games <= 0) {
        return 0;
    }
    return simulate(path, deck_path, effects_path, games, seed);
}

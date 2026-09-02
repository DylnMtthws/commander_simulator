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

#include "core/card.hpp"
#include "core/rng.hpp"
#include "core/sim.hpp"
#include "core/version.hpp"
#include "io/card_db_load.hpp"

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
int simulate(const std::filesystem::path& path, int games, std::uint64_t base_seed) {
    cs::CardDb db;
    try {
        db = cs::io::load_card_db(path);
    } catch (const cs::io::LoadError& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    const cs::StubPolicyDoNotUseForResults policy;
    const cs::GameConfig config;

    std::uint64_t can_pay_calls = 0;
    std::uint64_t turns = 0;
    std::uint64_t drawn = 0;
    std::uint64_t lands = 0;
    std::uint64_t spells = 0;
    std::uint64_t digest_mix = 0;

    for (int i = 0; i < games; ++i) {
        const cs::GameResult result = cs::run_game(
            db, config, policy, cs::seed_for_game(base_seed, static_cast<std::uint64_t>(i)));
        can_pay_calls += result.stats.can_pay_calls;
        turns += result.stats.turns;
        drawn += result.stats.cards_drawn;
        lands += result.stats.lands_played;
        spells += result.stats.spells_cast;
        digest_mix ^= result.state_digest;
    }

    const auto per_game = [games](std::uint64_t total) {
        return static_cast<double>(total) / games;
    };
    std::printf("\n%d games, seed %llu, policy: %s\n", games,
                static_cast<unsigned long long>(base_seed), policy.name());
    std::printf("  can_pay calls/game %8.1f   <- the number that decides section 11.1\n",
                per_game(can_pay_calls));
    std::printf("  turns/game         %8.1f\n", per_game(turns));
    std::printf("  cards drawn/game   %8.1f\n", per_game(drawn));
    std::printf("  lands played/game  %8.1f\n", per_game(lands));
    std::printf("  spells cast/game   %8.1f\n", per_game(spells));
    std::printf("  digest xor         %016llx  (identical runs agree here)\n",
                static_cast<unsigned long long>(digest_mix));
    std::printf("\n  NOTE: the stub policy is not the real one. These counts are a\n");
    std::printf("        floor on can_pay traffic, not a prediction of it.\n");
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
    int games = 0;
    std::uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--games") == 0 && i + 1 < argc) {
            games = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else {
            path = argv[i];
        }
    }

    const int status = summarise(path);
    if (status != 0 || games <= 0) {
        return status;
    }
    return simulate(path, games, seed);
}

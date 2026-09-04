#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/pattern.hpp"
#include "core/state.hpp"
#include "io/card_db_load.hpp"
#include "io/effects_load.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

class TempEffects {
public:
    explicit TempEffects(const std::string& contents) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("cs_mill_" + std::to_string(++counter) + ".toml");
        std::ofstream(path_) << contents;
    }
    ~TempEffects() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

const cs::CardDb& fixture_db() {
    static const cs::CardDb db =
        cs::io::load_card_db(std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");
    return db;
}

}  // namespace

TEST_CASE("MILL moves a card to the readable graveyard zone", "[effects][mill]") {
    cs::GameState state;
    state.library_count = 2;
    state.library[0] = 3;
    state.library[1] = 4;
    cs::Rng rng(7);

    const int milled = cs::mill_one(state, rng);
    REQUIRE(milled >= 0);
    REQUIRE(state.graveyard.test(milled));
    REQUIRE_FALSE(state.hand.test(milled));
    REQUIRE(state.library_size() == 1);
}

TEST_CASE("graveyard and storm pattern terms are conjunctive", "[pattern][storm]") {
    cs::PatternSet patterns;
    cs::Requirement requirement;
    requirement.in_graveyard.set(2);
    requirement.storm_count_gte = 3;
    cs::GameState state;
    std::vector<cs::Source> sources;

    state.graveyard.set(2);
    state.storm_count = 2;
    REQUIRE_FALSE(cs::requirement_holds(requirement, patterns, state, 0, sources));
    state.storm_count = 3;
    REQUIRE(cs::requirement_holds(requirement, patterns, state, 0, sources));
    state.graveyard.clear(2);
    REQUIRE_FALSE(cs::requirement_holds(requirement, patterns, state, 0, sources));
}

TEST_CASE("MILL requires both quantity fields", "[effects][mill][load]") {
    const TempEffects missing(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "MILL", cards = 3 } ]
)");
    REQUIRE_THROWS_MATCHES(cs::io::load_effects(missing.path(), fixture_db()),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("times_storm") &&
                                          ContainsSubstring("silent default")));
}

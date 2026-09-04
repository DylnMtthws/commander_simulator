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
                ("cs_exile_library_" + std::to_string(++counter) + ".toml");
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

TEST_CASE("EXILE_LIBRARY moves the library to exile and honors its explicit remainder",
          "[effects][exile_library]") {
    cs::GameState state;
    state.library_count = 4;
    state.library[0] = 0;
    state.library[1] = 1;
    state.library[2] = 2;
    state.library[3] = 3;

    cs::exile_library_until(state, 1);

    REQUIRE(state.library_size() == 1);
    REQUIRE(state.exile.count() == 3);
}

TEST_CASE("EXILE_LIBRARY has no silent leave default", "[effects][exile_library][load]") {
    const TempEffects missing(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "EXILE_LIBRARY" } ]
)");
    REQUIRE_THROWS_MATCHES(cs::io::load_effects(missing.path(), fixture_db()),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("requires") &&
                                          ContainsSubstring("leave")));

    const TempEffects valid(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "EXILE_LIBRARY", leave = 0 } ]
)");
    const cs::EffectDb effects = cs::io::load_effects(valid.path(), fixture_db());
    REQUIRE(effects.by_slot[1].has_exile_library);
    REQUIRE(effects.by_slot[1].exile_library.leave == 0);
}

TEST_CASE("Oracle state requires both resolution and blue devotion reaching library size",
          "[pattern][oracle]") {
    cs::PatternSet patterns;
    patterns.blue_devotion[1] = 2;
    cs::Requirement requirement;
    requirement.resolved.set(0);
    requirement.devotion_gte_library = true;

    cs::GameState state;
    state.library_count = 2;
    state.battlefield.set(1);
    std::vector<cs::Source> sources;

    REQUIRE_FALSE(cs::requirement_holds(requirement, patterns, state, 0, sources));
    state.resolved.set(0);
    REQUIRE(cs::requirement_holds(requirement, patterns, state, 0, sources));
    state.library_count = 3;
    REQUIRE_FALSE(cs::requirement_holds(requirement, patterns, state, 0, sources));
}

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/effects.hpp"
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
                ("cs_untap_target_" + std::to_string(++counter) + ".toml");
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

TEST_CASE("UNTAP_TARGET offers only tapped legal permanents", "[effects][untap_target]") {
    cs::CardDb db;
    db.cards.resize(2);
    db.cards[0].export_index = 0;
    db.cards[0].faces.emplace_back();
    db.cards[0].faces.back().is_land = true;
    db.cards[1].export_index = 1;
    cs::GameState state;
    state.battlefield.set(0);
    state.battlefield.set(1);
    state.tapped.set(0);
    state.tapped.set(1);
    cs::UntapTargetEffect effect{.targets = 1, .nonland_only = true};
    std::vector<int> candidates;

    cs::untap_target_candidates(effect, db, state, candidates);
    REQUIRE(candidates == std::vector<int>{1});
}

TEST_CASE("UNTAP_TARGET has no silent target-shape defaults",
          "[effects][untap_target][load]") {
    const TempEffects missing(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "UNTAP_TARGET", targets = 1 } ]
)");
    REQUIRE_THROWS_MATCHES(cs::io::load_effects(missing.path(), fixture_db()),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("nonland_only") &&
                                          ContainsSubstring("silent default")));
}

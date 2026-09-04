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
                ("cs_escape_" + std::to_string(++counter) + ".toml");
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

TEST_CASE("an active ESCAPE effect grants nonland graveyard casting with an explicit cost",
          "[effects][escape]") {
    cs::CardDb db;
    db.cards.resize(3);
    db.cards[0].export_index = 0;  // Underworld Breach stand-in
    db.cards[1].export_index = 1;  // spell
    db.cards[2].export_index = 2;  // land
    db.cards[2].faces.push_back(cs::Face{.is_land = true});

    cs::EffectDb effects;
    effects.by_slot.resize(3);
    effects.by_slot[0].has_escape = true;
    effects.by_slot[0].escape.exile_cards = 3;

    cs::GameState state;
    state.battlefield.set(0);
    state.graveyard.set(1);
    state.graveyard.set(2);

    REQUIRE(cs::escape_exile_cost(db, effects, state, 1) == 3);
    REQUIRE(cs::escape_exile_cost(db, effects, state, 2) == -1);
    state.battlefield.clear(0);
    REQUIRE(cs::escape_exile_cost(db, effects, state, 1) == -1);
}

TEST_CASE("ESCAPE requires its graveyard payment", "[effects][escape][load]") {
    const TempEffects missing(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "ESCAPE" } ]
)");
    REQUIRE_THROWS_MATCHES(cs::io::load_effects(missing.path(), fixture_db()),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("exile_cards") &&
                                          ContainsSubstring("silent default")));
}

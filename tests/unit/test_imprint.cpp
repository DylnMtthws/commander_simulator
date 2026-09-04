#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/pattern.hpp"
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
                ("cs_imprint_" + std::to_string(++counter) + ".toml");
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

TEST_CASE("imprinted and available_mana terms are conjunctive", "[pattern][imprint]") {
    cs::PatternSet patterns;
    cs::Requirement requirement;
    requirement.imprinted_permanent = 0;
    requirement.imprinted_card = 1;
    requirement.has_available_mana = true;
    requirement.available_mana.generic = 2;
    cs::GameState state;
    state.imprinted[0] = 1;
    std::vector<cs::Source> sources{{.amount = 2, .slot = 2}};

    REQUIRE(cs::requirement_holds(requirement, patterns, state, 0, sources));
    sources[0].amount = 1;
    REQUIRE_FALSE(cs::requirement_holds(requirement, patterns, state, 0, sources));
    sources[0].amount = 2;
    state.imprinted[0] = -1;
    REQUIRE_FALSE(cs::requirement_holds(requirement, patterns, state, 0, sources));
}

TEST_CASE("CARD_COST imprint state is explicit at load", "[effects][imprint][load]") {
    const TempEffects missing(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "CARD_COST", cards = 1, filter = "instant" } ]
)");
    REQUIRE_THROWS_MATCHES(cs::io::load_effects(missing.path(), fixture_db()),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("destination") &&
                                          ContainsSubstring("silent default")));

    const TempEffects valid(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "CARD_COST", cards = 1, filter = "instant", mana_value_lte = 2, destination = "exile", remember_imprint = true } ]
)");
    const cs::EffectDb effects = cs::io::load_effects(valid.path(), fixture_db());
    REQUIRE(effects.by_slot[1].card_cost.destination == cs::CardCostDestination::Exile);
    REQUIRE(effects.by_slot[1].card_cost.remember_imprint);
    REQUIRE(effects.by_slot[1].card_cost.mana_value_lte == 2);
}

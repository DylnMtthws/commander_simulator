#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
                ("cs_effect_boundary_" + std::to_string(++counter) + ".toml");
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

TEST_CASE("an unlisted non-pattern card is counted and named as unauthored",
          "[effects][coverage]") {
    const TempEffects file("[cards]\n");
    const cs::EffectDb effects = cs::io::load_effects(file.path(), fixture_db());
    REQUIRE(effects.unauthored == static_cast<int>(fixture_db().size()));
    REQUIRE(effects.unauthored_names.size() == fixture_db().size());
}

TEST_CASE("an inherited pattern may not silently depend on an unlisted effect",
          "[effects][coverage][pattern]") {
    const TempEffects file("[cards]\n");
    cs::PatternSet patterns;
    patterns.named_slots.set(0);
    REQUIRE_THROWS_MATCHES(cs::io::load_effects(file.path(), fixture_db(), &patterns),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("inherited pattern") &&
                                          ContainsSubstring("explicitly modeled")));
}

TEST_CASE("an explicit unauthored declaration is allowed for a pattern card",
          "[effects][coverage][pattern]") {
    const std::string name = fixture_db().cards[0].listed_name;
    const TempEffects file("[cards.\"" + name +
                           "\"]\nstatus = \"unauthored\"\nreason = \"fixture gap\"\n");
    cs::PatternSet patterns;
    patterns.named_slots.set(0);
    const cs::EffectDb effects = cs::io::load_effects(file.path(), fixture_db(), &patterns);
    REQUIRE(effects.unauthored == static_cast<int>(fixture_db().size()));
}

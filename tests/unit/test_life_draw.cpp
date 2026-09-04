#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/policy.hpp"
#include "core/sim.hpp"
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
                ("cs_life_draw_" + std::to_string(++counter) + ".toml");
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

TEST_CASE("life_floor is a policy input for life-paid draws", "[policy][life]") {
    cs::CardDb db;
    cs::PatternSet patterns;
    cs::GameState state;
    state.life = 12;
    std::vector<cs::Source> sources;
    cs::Context context{db, patterns, state, sources};
    cs::GameStats stats;
    cs::PolicyWeights weights;
    weights.life_floor = 10;
    cs::AuthoredPolicy policy(weights);

    REQUIRE(policy.choose_life_payment(context, 2, stats));
    REQUIRE_FALSE(policy.choose_life_payment(context, 3, stats));
    state.life = 10;
    REQUIRE_FALSE(policy.choose_life_payment(context, 0, stats));
}

TEST_CASE("life-paid DRAW requires every timing and repetition declaration",
          "[effects][draw][life]") {
    const TempEffects missing(R"(
[cards."Sol Ring"]
status = "modeled"
effects = [ { kind = "DRAW", cards = 1, life_loss = "fixed", life_per_card = 1 } ]
)");
    REQUIRE_THROWS_MATCHES(cs::io::load_effects(missing.path(), fixture_db()),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("repeat") &&
                                          ContainsSubstring("silent default")));
}

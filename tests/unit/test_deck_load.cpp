// Deck-file validation. Almost entirely about what it REFUSES.
//
// The failure this guards against is specific: a pattern that can never fire
// looks identical to a line the deck simply does not assemble. Both report
// zero. So every way a pattern can be unsatisfiable by construction has to be
// caught at load, where it is a typo with a name attached, rather than at run
// time where it is a plausible statistic.

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "io/card_db_load.hpp"
#include "io/deck_load.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

const cs::CardDb& fixture_db() {
    static const cs::CardDb db =
        cs::io::load_card_db(std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");
    return db;
}

class TempToml {
public:
    explicit TempToml(const std::string& contents) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("cs_deck_" + std::to_string(++counter) + ".toml");
        std::ofstream(path_) << contents;
    }
    ~TempToml() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempToml(const TempToml&) = delete;
    TempToml& operator=(const TempToml&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// A minimal valid deck over cards that exist in the fixture.
constexpr const char* kHeader = R"(
[deck]
commander = "Kinnan, Bonder Prodigy"
[strategy_pack]
id = "test-pack"
version = "1.0.0"
supported_commander_oracle_ids = ["8d11aa49-d4cd-48b1-aa0f-8548fa733416"]
supported_effect_definitions = ["MANA_SOURCE"]
assembly_objectives = ["state_a"]
play_policy_implementation = "test-policy.v1"
declared_table_assumptions = ["none"]
known_blind_spots = ["test fixture"]
[table]
opponents = 3
opponent_colors = ["G","U"]
on_the_play = true
[ablation]
replacement = "Forest"
[provenance]
source = "test"
source_url = ""
snapshot_date = "1970-01-01"
cards_sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
[policy]
land_floor = 3
land_ceiling = 6
life_floor = 10
)";

cs::io::DeckFile load(const std::string& body) {
    const TempToml file(std::string(kHeader) + body);
    return cs::io::load_deck(file.path(), fixture_db());
}

}  // namespace

TEST_CASE("loads a valid deck with engines and patterns", "[deck]") {
    const cs::io::DeckFile deck = load(R"(
[[engine]]
name = "engine_a"
sets = ["INFINITE:C"]
[engine.requires]
in_play = ["Kinnan, Bonder Prodigy", "Sol Ring"]

[[win]]
name = "infinite_C_into_something"
terminal_state = "assembly_proxy"
[win.requires]
flag = "INFINITE:C"
in_play = ["Sol Ring"]
)");
    REQUIRE(deck.commander == "Kinnan, Bonder Prodigy");
    REQUIRE(deck.strategy_pack.id == "test-pack");
    REQUIRE(deck.table.opponents == 3);
    REQUIRE(deck.patterns.engines.size() == 1);
    REQUIRE(deck.patterns.patterns.size() == 1);
    REQUIRE(deck.patterns.patterns[0].name == "infinite_C_into_something");
}

TEST_CASE("a deck inherits exactly the library patterns whose card keys it contains",
          "[deck][pattern-library]") {
    static int counter = 0;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cs_pattern_library_" + std::to_string(++counter));
    std::filesystem::create_directories(directory / "patterns");
    const std::filesystem::path deck_path = directory / "fixture.deck.toml";
    const std::filesystem::path library_path = directory / "patterns" / "shared.toml";
    std::ofstream(deck_path) << kHeader;
    std::ofstream(library_path) << R"(
[[win]]
name = "sol_ring_available"
cards = ["Sol Ring"]
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Sol Ring"]

[[win]]
name = "absent_card_state"
cards = ["Definitely Absent"]
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Definitely Absent"]
)";

    const cs::io::DeckFile deck = cs::io::load_deck(deck_path, fixture_db());
    REQUIRE(deck.patterns.patterns.size() == 1);
    REQUIRE(deck.patterns.inherited_pattern_names ==
            std::vector<std::string>{"sol_ring_available"});
    REQUIRE(deck.patterns.patterns[0].terminal_state == "assembly_proxy");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("refuses a pattern naming a card that is not in the deck", "[deck]") {
    // The case asked for by name. A misspelled or absent card produces a
    // requirement that can never hold, and the pattern reports zero for the
    // rest of the project's life while looking exactly like a line that never
    // assembles.
    REQUIRE_THROWS_MATCHES(load(R"(
[[win]]
name = "state_with_a_typo"
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Basalt Monolithh"]
)"),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("Basalt Monolithh") &&
                                          ContainsSubstring("not in the deck")));
}

TEST_CASE("refuses a card that exists in Magic but not in this deck", "[deck]") {
    // Sharper than a typo: a real card, correctly spelled, simply not in the
    // 99. Nothing but the deck list can catch this.
    REQUIRE_THROWS_MATCHES(
        load(R"(
[[win]]
name = "state_naming_an_absent_card"
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Black Lotus"]
)"),
        cs::io::DeckError, MessageMatches(ContainsSubstring("Black Lotus")));
}

TEST_CASE("refuses an unknown pattern term", "[deck]") {
    // The vocabulary is closed. A term silently ignored would make its pattern
    // strictly EASIER to satisfy, so it would fire early and the deck would
    // look fast - the error direction that flatters.
    REQUIRE_THROWS_MATCHES(load(R"(
[[win]]
name = "state_with_a_bad_term"
terminal_state = "assembly_proxy"
[win.requires]
in_grabeyard = ["Sol Ring"]
)"),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("unknown pattern term") &&
                                          ContainsSubstring("in_grabeyard")));
}

TEST_CASE("refuses a term that is planned but not implemented", "[deck]") {
    // Distinct message from "unknown": this is the difference between a typo
    // and a missing feature, and silently ignoring it would make the pattern
    // never fire rather than fire early.
    REQUIRE_THROWS_MATCHES(load(R"(
[[win]]
name = "state_needing_mana"
terminal_state = "assembly_proxy"
[win.requires]
attached = ["Sol Ring", "Forest"]
)"),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("NOT IMPLEMENTED")));
}

TEST_CASE("refuses a pattern requiring a flag no engine sets", "[deck]") {
    REQUIRE_THROWS_MATCHES(load(R"(
[[win]]
name = "state_needing_a_missing_flag"
terminal_state = "assembly_proxy"
[win.requires]
flag = "INFINITE:C"
)"),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("no engine sets") &&
                                          ContainsSubstring("could never fire")));
}

TEST_CASE("refuses an engine that requires a flag", "[deck]") {
    // Exactly one level of indirection: engines set flags, wins consume them.
    // Allowing an engine to read a flag would make engines compose, which needs
    // cycle detection and a fixpoint - the cost the two-level design avoids.
    REQUIRE_THROWS_MATCHES(load(R"(
[[engine]]
name = "engine_a"
sets = ["A"]
[engine.requires]
flag = "B"

[[win]]
name = "state_a"
terminal_state = "assembly_proxy"
[win.requires]
flag = "A"
)"),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("may not require a flag")));
}

TEST_CASE("refuses a pattern named for an outcome", "[deck]") {
    // Section 5.4: the name is where a caveat survives. A pattern called
    // win_thrasios is read six months later as "the deck won", which this deck
    // has no card capable of doing.
    for (const char* name : {"win_thrasios", "thrasios_wins", "lethal_line", "KILL_TURN"}) {
        const std::string body =
            std::string("[[win]]\nname = \"") + name + "\"\n[win.requires]\nin_play = [\"Sol Ring\"]\n";
        REQUIRE_THROWS_MATCHES(load(body), cs::io::DeckError,
                               MessageMatches(ContainsSubstring("named for an outcome")));
    }
}

TEST_CASE("accepts a state-shaped name that merely contains the letters", "[deck]") {
    // Guards the guard: "windfall" and "winding" contain "win" and must not be
    // rejected. A word-boundary check, not a substring search.
    REQUIRE_NOTHROW(load(R"(
[[win]]
name = "windfall_engine_online"
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Sol Ring"]
)"));
}

TEST_CASE("refuses a deck with no patterns at all", "[deck]") {
    REQUIRE_THROWS_MATCHES(load(""), cs::io::DeckError,
                           MessageMatches(ContainsSubstring("no [[win]] patterns")));
}

TEST_CASE("refuses an engine that sets nothing", "[deck]") {
    REQUIRE_THROWS_MATCHES(load(R"(
[[engine]]
name = "engine_a"
sets = []
[engine.requires]
in_play = ["Sol Ring"]

[[win]]
name = "state_a"
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Sol Ring"]
)"),
                           cs::io::DeckError,
                           MessageMatches(ContainsSubstring("at least one flag")));
}

TEST_CASE("refuses a missing required [table] field", "[deck]") {
    const TempToml file(R"(
[deck]
commander = "Kinnan, Bonder Prodigy"
[strategy_pack]
id = "test-pack"
version = "1.0.0"
supported_commander_oracle_ids = ["8d11aa49-d4cd-48b1-aa0f-8548fa733416"]
supported_effect_definitions = ["MANA_SOURCE"]
assembly_objectives = ["state_a"]
play_policy_implementation = "test-policy.v1"
declared_table_assumptions = ["none"]
known_blind_spots = ["test fixture"]
[table]
opponents = 3
opponent_colors = ["G","U"]
[ablation]
replacement = "Forest"
[[win]]
name = "state_a"
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Sol Ring"]
)");
    REQUIRE_THROWS_MATCHES(cs::io::load_deck(file.path(), fixture_db()), cs::io::DeckError,
                           MessageMatches(ContainsSubstring("on_the_play")));
}

TEST_CASE("a deck without provenance is refused", "[deck][provenance]") {
    // A decklist is a snapshot of something that CHANGES. This list is a
    // snapshot of a living Moxfield list and the list has since changed by 29
    // cards; without a date and a hash, a number cannot be attributed to a list.
    //
    // Same discipline as `authored_from` on an effect, one level up: that pins
    // the oracle text an author read, this pins the 99 they read it for.
    const std::string no_block = R"(
[deck]
commander = "Kinnan, Bonder Prodigy"
[strategy_pack]
id = "test-pack"
version = "1.0.0"
supported_commander_oracle_ids = ["8d11aa49-d4cd-48b1-aa0f-8548fa733416"]
supported_effect_definitions = ["MANA_SOURCE"]
assembly_objectives = ["state_a"]
play_policy_implementation = "test-policy.v1"
declared_table_assumptions = ["none"]
known_blind_spots = ["test fixture"]
[table]
opponents = 3
opponent_colors = ["G","U"]
on_the_play = true
[ablation]
replacement = "Forest"
[cards]
mainboard = ["Sol Ring"]
[[win]]
name = "has_sol_ring"
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Sol Ring"]
)";
    const TempToml file(no_block);
    REQUIRE_THROWS_AS(cs::io::load_deck(file.path(), fixture_db()), cs::io::DeckError);
}

TEST_CASE("an unrecorded source_url must be written down, not left off",
          "[deck][provenance]") {
    // The asymmetry is deliberate. source_url MAY be empty, because the URL for
    // this snapshot was never recorded and a reconstructed one would be worse
    // than none - it would resolve, to the wrong thing. It may not be ABSENT,
    // so "we did not record it" is a statement in the file rather than a gap.
    const std::string body = R"(
[cards]
mainboard = ["Sol Ring"]
[[win]]
name = "has_sol_ring"
terminal_state = "assembly_proxy"
[win.requires]
in_play = ["Sol Ring"]
)";
    SECTION("empty is accepted and preserved") {
        const cs::io::DeckFile deck = load(body);
        REQUIRE(deck.provenance.source_url.empty());
        REQUIRE(deck.provenance.source == "test");
        REQUIRE(deck.provenance.snapshot_date == "1970-01-01");
    }
    SECTION("absent is refused") {
        const std::string header = R"(
[deck]
commander = "Kinnan, Bonder Prodigy"
[strategy_pack]
id = "test-pack"
version = "1.0.0"
supported_commander_oracle_ids = ["8d11aa49-d4cd-48b1-aa0f-8548fa733416"]
supported_effect_definitions = ["MANA_SOURCE"]
assembly_objectives = ["state_a"]
play_policy_implementation = "test-policy.v1"
declared_table_assumptions = ["none"]
known_blind_spots = ["test fixture"]
[table]
opponents = 3
opponent_colors = ["G","U"]
on_the_play = true
[ablation]
replacement = "Forest"
[provenance]
source = "test"
snapshot_date = "1970-01-01"
cards_sha256 = "abc"
)";
        const TempToml file(header + body);
        REQUIRE_THROWS_AS(cs::io::load_deck(file.path(), fixture_db()), cs::io::DeckError);
    }
}

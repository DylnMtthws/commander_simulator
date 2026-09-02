// Loading a card database, weighted towards what it REFUSES.
//
// The valuable tests here are the malformed ones. A loader that accepts a
// truncated or edited file does not fail - it simulates a different deck and
// reports a plausible number, which is the failure mode this project keeps
// finding (SIM_PLAN.md sections 2.7 and 9.3).

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "io/card_db_load.hpp"

using Catch::Matchers::ContainsSubstring;

namespace {

const std::filesystem::path kFixture = std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json";

// Writes a temporary file and removes it, so a failing test cannot leave state
// that makes the next run behave differently.
class TempJson {
public:
    explicit TempJson(const std::string& contents) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("cs_test_" + std::to_string(++counter) + ".json");
        std::ofstream(path_) << contents;
    }
    ~TempJson() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempJson(const TempJson&) = delete;
    TempJson& operator=(const TempJson&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string minimal(const std::string& cards, int card_count = 1) {
    return R"({"manifest":{"source_view":"v","card_count":)" + std::to_string(card_count) +
           R"(,"cards_sha256":"abc"},"cards":[)" + cards + "]}";
}

constexpr const char* kFace =
    R"({"index":0,"name":"Sol Ring","type_line":"Artifact","is_land":false,)"
    R"("mana_value":1,"cost":{"generic":1,"pips":[],"variable":0,"phyrexian":[]}})";

std::string one_card(const std::string& faces = std::string("[") + kFace + "]") {
    return R"({"export_index":0,"listed_name":"Sol Ring","name":"Sol Ring","layout":"normal",)"
           R"("mana_value":1,"castable_cmcs":[1],"all_types":["Artifact"],"color_identity":[],)"
           R"("has_land_face":false,"is_commander":false,"faces":)" +
           faces + "}";
}

// Exact-string substitution. An earlier version of this file did the same job
// with find() plus a hardcoded length, miscounted by one, ate a comma, and
// turned every "wrong field" case into a JSON syntax error - so the tests
// passed for the wrong reason until the messages were read. Offsets are not
// worth the risk when the substring is right there.
std::string replacing(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);  // the mutation must actually apply
    text.replace(at, from.size(), to);
    return text;
}

}  // namespace

TEST_CASE("loads the committed fixture", "[load]") {
    const cs::CardDb db = cs::io::load_card_db(kFixture);

    REQUIRE(db.size() == 8);
    REQUIRE(db.manifest.card_count == 8);
    REQUIRE(db.manifest.is_fixture);
    REQUIRE(db.manifest.source_view == "mtg_v1.card_any_medium");
    REQUIRE_FALSE(db.manifest.cards_sha256.empty());
    REQUIRE_FALSE(db.manifest.max_content_updated_at.empty());
}

TEST_CASE("fixture covers the shapes it claims to", "[load]") {
    const cs::CardDb db = cs::io::load_card_db(kFixture);

    auto find = [&](std::string_view listed) -> const cs::Card& {
        for (const cs::Card& c : db.cards) {
            if (c.listed_name == listed) return c;
        }
        FAIL("fixture is missing " << listed);
        return db.cards.front();
    };

    SECTION("a land has no cost, which is not a cost of zero") {
        const cs::Card& forest = find("Forest");
        REQUIRE(forest.faces.size() == 1);
        REQUIRE(forest.faces[0].is_land);
        REQUIRE_FALSE(forest.faces[0].is_castable());
    }

    SECTION("a zero-cost spell is castable, unlike a land") {
        const cs::Card& sol = find("Sol Ring");
        REQUIRE(sol.faces[0].is_castable());
        REQUIRE(sol.faces[0].cost->generic == 1);
    }

    SECTION("the commander is flagged and carries its pips") {
        const cs::Card& kinnan = find("Kinnan, Bonder Prodigy");
        REQUIRE(kinnan.is_commander);
        REQUIRE(kinnan.faces[0].cost->pips[static_cast<std::size_t>(cs::Colour::Blue)] == 1);
        REQUIRE(kinnan.faces[0].cost->pips[static_cast<std::size_t>(cs::Colour::Green)] == 1);
    }

    SECTION("a modal_dfc has two faces, one castable and one a land") {
        const cs::Card& sink = find("Sink into Stupor");
        REQUIRE(sink.faces.size() == 2);
        REQUIRE(sink.name == "Sink into Stupor // Soporific Springs");
        REQUIRE(sink.faces[0].is_castable());
        REQUIRE(sink.faces[1].is_land);
        REQUIRE_FALSE(sink.faces[1].is_castable());
    }

    SECTION("an X spell is castable at a cost that does nothing") {
        // Finale of Devastation is {X}{G}{G}: variable 1, and a mana value of 2
        // that is real, legal, and useless (SIM_PLAN.md section 2.2).
        const cs::Card& finale = find("Finale of Devastation");
        REQUIRE(finale.faces[0].cost->variable == 1);
        REQUIRE(finale.mana_value == 2);
        REQUIRE(finale.faces[0].cost->pips[static_cast<std::size_t>(cs::Colour::Green)] == 2);
    }

    SECTION("a Phyrexian pip is not a coloured pip") {
        const cs::Card& misstep = find("Mental Misstep");
        const cs::Cost& cost = *misstep.faces[0].cost;
        REQUIRE(cost.phyrexian[static_cast<std::size_t>(cs::Colour::Blue)] == 1);
        REQUIRE(cost.pips[static_cast<std::size_t>(cs::Colour::Blue)] == 0);
        REQUIRE(cost.mana_value_at_x_zero() == 1);
    }

    SECTION("a Reserved List card mtg_v1.card would have dropped is present") {
        REQUIRE(find("Tropical Island").faces[0].is_land);
    }
}

TEST_CASE("refuses a file it cannot open", "[load]") {
    REQUIRE_THROWS_MATCHES(
        cs::io::load_card_db("does/not/exist.json"), cs::io::LoadError,
        Catch::Matchers::MessageMatches(ContainsSubstring("run the exporter")));
}

TEST_CASE("refuses malformed JSON as malformed, not as a missing field", "[load]") {
    const TempJson file("{ this is not json ");
    REQUIRE_THROWS_MATCHES(cs::io::load_card_db(file.path()), cs::io::LoadError,
                           Catch::Matchers::MessageMatches(ContainsSubstring("malformed JSON")));
}

TEST_CASE("refuses a structurally wrong document", "[load]") {
    SECTION("top level is an array") {
        const TempJson file("[]");
        REQUIRE_THROWS_MATCHES(
            cs::io::load_card_db(file.path()), cs::io::LoadError,
            Catch::Matchers::MessageMatches(ContainsSubstring("top level must be an object")));
    }
    SECTION("no manifest") {
        const TempJson file(R"({"cards":[]})");
        REQUIRE_THROWS_MATCHES(
            cs::io::load_card_db(file.path()), cs::io::LoadError,
            Catch::Matchers::MessageMatches(ContainsSubstring("'manifest'")));
    }
    SECTION("no cards") {
        const TempJson file(R"({"manifest":{"source_view":"v","card_count":0,"cards_sha256":"a"}})");
        REQUIRE_THROWS_MATCHES(cs::io::load_card_db(file.path()), cs::io::LoadError,
                               Catch::Matchers::MessageMatches(ContainsSubstring("'cards'")));
    }
}

TEST_CASE("refuses a card with a missing or mistyped field", "[load]") {
    SECTION("missing field is named") {
        const TempJson file(minimal(R"({"export_index":0,"listed_name":"Sol Ring"})"));
        REQUIRE_THROWS_MATCHES(
            cs::io::load_card_db(file.path()), cs::io::LoadError,
            Catch::Matchers::MessageMatches(ContainsSubstring("missing required field 'name'")));
    }
    SECTION("wrong type is named, not silently coerced") {
        // nlohmann's operator[] would give a zero here. A card that quietly
        // costs nothing looks exactly like a fast deck.
        const TempJson file(
            minimal(replacing(one_card(), R"("mana_value":1,)", R"("mana_value":"1",)")));
        REQUIRE_THROWS_MATCHES(
            cs::io::load_card_db(file.path()), cs::io::LoadError,
            Catch::Matchers::MessageMatches(ContainsSubstring("must be an integer")));
    }
    SECTION("empty faces array") {
        const TempJson file(minimal(one_card("[]")));
        REQUIRE_THROWS_MATCHES(
            cs::io::load_card_db(file.path()), cs::io::LoadError,
            Catch::Matchers::MessageMatches(ContainsSubstring("non-empty array")));
    }
}

TEST_CASE("RULE C1: manifest count must equal the array length", "[load]") {
    // A truncated or hand-edited file. Loading it would simulate a different
    // deck and report a number that looks fine.
    const TempJson file(minimal(one_card(), /*card_count=*/100));
    REQUIRE_THROWS_MATCHES(
        cs::io::load_card_db(file.path()), cs::io::LoadError,
        Catch::Matchers::MessageMatches(ContainsSubstring("manifest says 100 cards")));
}

TEST_CASE("refuses export_index that is not a dense slot number", "[load]") {
    SECTION("duplicate merges two cards into one bit") {
        const TempJson file(minimal(one_card() + "," + one_card(), 2));
        REQUIRE_THROWS_MATCHES(
            cs::io::load_card_db(file.path()), cs::io::LoadError,
            Catch::Matchers::MessageMatches(ContainsSubstring("duplicate export_index 0")));
    }
    SECTION("a gap leaves a bit nothing owns") {
        const std::string second = replacing(one_card(), R"("export_index":0)", R"("export_index":7)");
        const TempJson file(minimal(one_card() + "," + second, 2));
        REQUIRE_THROWS_MATCHES(cs::io::load_card_db(file.path()), cs::io::LoadError,
                               Catch::Matchers::MessageMatches(ContainsSubstring("must be dense")));
    }
}

TEST_CASE("refuses a cost that disagrees with its own mana value", "[load]") {
    const TempJson file(minimal(replacing(one_card(), R"("generic":1)", R"("generic":9)")));
    REQUIRE_THROWS_MATCHES(
        cs::io::load_card_db(file.path()), cs::io::LoadError,
        Catch::Matchers::MessageMatches(ContainsSubstring("cost implies mana value 9")));
}

TEST_CASE("refuses an unknown colour letter", "[load]") {
    const TempJson file(minimal(replacing(one_card(), R"("pips":[])", R"("pips":["Z"])")));
    REQUIRE_THROWS_MATCHES(cs::io::load_card_db(file.path()), cs::io::LoadError,
                           Catch::Matchers::MessageMatches(ContainsSubstring("unknown colour 'Z'")));
}

TEST_CASE("has_land_face and plays_as_land are different questions", "[load][one-definition]") {
    // Pinned, because the names are one word apart and the answers differ on 24
    // of this deck's 25 lands.
    //
    // mtg_v1.has_land_face asks "is one of this card's FACES a land", which is
    // only ever true for a multi-faced card. Forest is a land and has no faces
    // in the face table, so upstream says false - correctly, for the question it
    // is answering.
    //
    // The land drop wants a different question, and for three phases asked it
    // with a private copy in policy.cpp while this field sat loaded, validated
    // as required, and never read. Nothing failed: the wrong definition was the
    // unused one. Had a consumer reached for the obvious field instead, the deck
    // would have played zero lands.
    const cs::CardDb db = cs::io::load_card_db(kFixture);
    const auto find = [&](const char* listed) -> const cs::Card& {
        for (const cs::Card& card : db.cards) {
            if (card.listed_name == listed) return card;
        }
        FAIL("fixture is missing " << listed);
        return db.cards.front();
    };

    const cs::Card& forest = find("Forest");
    REQUIRE(forest.plays_as_land());
    REQUIRE_FALSE(forest.has_land_face);  // and upstream is right about that

    const cs::Card& mdfc = find("Sink into Stupor");
    REQUIRE(mdfc.plays_as_land());
    REQUIRE(mdfc.has_land_face);  // the only shape where the two agree

    const cs::Card& ring = find("Sol Ring");
    REQUIRE_FALSE(ring.plays_as_land());
    REQUIRE_FALSE(ring.has_land_face);
}

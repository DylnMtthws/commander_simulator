#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "io/card_db_load.hpp"
#include "io/service.hpp"
#include "io/sha256.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

class TempJson {
public:
    explicit TempJson(const std::string& contents) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("cs_service_" + std::to_string(++counter) + ".json");
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

constexpr const char* kCommander = "8d11aa49-d4cd-48b1-aa0f-8548fa733416";
constexpr const char* kForest = "b34bb2dc-c1af-4d77-b0b3-a0fb342a5fc6";

std::string candidate_json(int quantity, const std::string& hash) {
    return std::string(R"({
  "schema_version":"cedh-deck-candidate.v2",
  "candidate_id":"fixture",
  "commander_oracle_ids":[")") + kCommander + R"("],
  "library":[{"oracle_id":")" + kForest + R"(","quantity":)" +
           std::to_string(quantity) + R"(}],
  "strategy_pack_id":"kinnan-midrange-goldfish",
  "strategy_pack_version":"1.0.0",
  "provenance":{
    "producer":{"name":"fixture","version":"1"},
    "card_data":{"source":"fixture","snapshot_at":"2026-01-01T00:00:00Z","hash":"sha256:fixture"},
    "corpus":{"source":"fixture","snapshot_at":"2026-01-01T00:00:00Z","hash":"sha256:fixture"}
  },
  "deck_sha256":")" + hash + "\"\n}";
}

// Spelled out rather than obtained from deck_sha256(), so this test would
// catch the implementation redefining the preimage under itself.
std::string valid_hash(int quantity) {
    const std::string preimage = std::string("C:") + kCommander + "\n" + kForest + ":" +
                                 std::to_string(quantity) + "\n";
    return "sha256:" + cs::io::sha256_hex(preimage);
}

}  // namespace

TEST_CASE("SHA-256 uses the standard digest", "[service][hash]") {
    REQUIRE(cs::io::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("a candidate request enforces 99 cards and recomputes its deck hash", "[service]") {
    SECTION("valid aggregate library") {
        const TempJson file(candidate_json(99, valid_hash(99)));
        const cs::io::CandidateRequest request = cs::io::load_candidate_request(file.path());
        REQUIRE(request.candidate_id == "fixture");
        REQUIRE(request.library[0].quantity == 99);
        REQUIRE(request.deck_sha256 == cs::io::deck_sha256(request));
    }
    SECTION("98 cards is refused before simulation") {
        const TempJson file(candidate_json(98, valid_hash(98)));
        REQUIRE_THROWS_MATCHES(cs::io::load_candidate_request(file.path()), cs::io::ContractError,
                               MessageMatches(ContainsSubstring("expected exactly 99")));
    }
    SECTION("a deck hash that does not describe the submitted list is refused") {
        const TempJson file(candidate_json(99, "sha256:" + std::string(64, '0')));
        REQUIRE_THROWS_MATCHES(
            cs::io::load_candidate_request(file.path()), cs::io::ContractError,
            MessageMatches(ContainsSubstring("does not describe the submitted list")));
    }
    SECTION("a wrong deck hash is a deck mismatch and nothing else") {
        const TempJson file(candidate_json(99, "sha256:" + std::string(64, '0')));
        try {
            (void)cs::io::load_candidate_request(file.path());
            FAIL("expected a refusal");
        } catch (const cs::io::ContractError& error) {
            REQUIRE(error.code() == cs::io::kDeckHashMismatch);
        }
    }
    SECTION("the removed v1 candidate_hash is refused by name, not ignored") {
        // Silently accepting it would let a v1 producer believe its
        // pack-inclusive hash was checked when nothing had checked it.
        std::string document = candidate_json(99, valid_hash(99));
        document.insert(document.size() - 2, ",\n  \"candidate_hash\":\"sha256:deadbeef\"");
        const TempJson file(document);
        try {
            (void)cs::io::load_candidate_request(file.path());
            FAIL("expected a refusal");
        } catch (const cs::io::ContractError& error) {
            REQUIRE(error.code() == cs::io::kContractViolation);
            REQUIRE_THAT(std::string(error.what()), ContainsSubstring("candidate_hash"));
        }
    }
}

TEST_CASE("the same deck under two strategy packs has one deck hash", "[service][hash]") {
    // The regression this contract exists for. These two fixtures are the same
    // 99 cards requesting different packs; under the old pack-inclusive
    // candidate_hash they hashed differently, and a consumer comparing deck
    // identity concluded the user's deck had changed when only the pack had.
    const cs::io::CandidateRequest named = cs::io::load_candidate_request(
        std::filesystem::path(CS_FIXTURE_DIR) / "kinnan-candidate.v2.json");
    const cs::io::CandidateRequest derived = cs::io::load_candidate_request(
        std::filesystem::path(CS_FIXTURE_DIR) / "kinnan-derived-candidate.v2.json");
    REQUIRE(named.strategy_pack_id != derived.strategy_pack_id);
    REQUIRE(cs::io::deck_sha256(named) == cs::io::deck_sha256(derived));
}

TEST_CASE("a pack the simulator cannot run is not reported as a deck problem",
          "[service][pack][errors]") {
    const cs::io::CandidateRequest request = cs::io::load_candidate_request(
        std::filesystem::path(CS_FIXTURE_DIR) / "unsupported-pack-candidate.v2.json");
    const cs::CardDb db = cs::io::load_card_db(
        std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");
    cs::io::DeckFile pack;
    pack.strategy_pack.id = "kinnan-midrange-goldfish";
    pack.strategy_pack.version = "1.0.0";
    try {
        cs::io::validate_candidate_for_pack(request, db, pack);
        FAIL("expected a refusal");
    } catch (const cs::io::ContractError& error) {
        REQUIRE(error.code() == cs::io::kExecutionContextUnsupported);
        REQUIRE(error.code() != cs::io::kDeckHashMismatch);
    }
}

TEST_CASE("an unsupported strategy pack never falls back to Kinnan", "[service][pack]") {
    const cs::io::CandidateRequest request = cs::io::load_candidate_request(
        std::filesystem::path(CS_FIXTURE_DIR) / "unsupported-pack-candidate.v2.json");
    cs::io::DeckFile pack;
    pack.strategy_pack.id = "kinnan-midrange-goldfish";
    pack.strategy_pack.version = "1.0.0";
    const cs::CardDb db = cs::io::load_card_db(
        std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");

    REQUIRE_THROWS_MATCHES(cs::io::validate_candidate_for_pack(request, db, pack),
                           cs::io::ContractError,
                           MessageMatches(ContainsSubstring("generic Kinnan fallback is forbidden")));
}

TEST_CASE("the registry selects an installed pack by exact id and version",
          "[service][pack][registry]") {
    const cs::io::CandidateRequest request = cs::io::load_candidate_request(
        std::filesystem::path(CS_FIXTURE_DIR) / "kinnan-candidate.v2.json");
    const std::filesystem::path data =
        std::filesystem::path(CS_FIXTURE_DIR).parent_path().parent_path() / "data";
    const cs::io::StrategyPackSelection selected =
        cs::io::select_strategy_pack(request, data);
    REQUIRE_FALSE(selected.derived);
    REQUIRE(selected.path.filename() == "kinnan.deck.toml");

    cs::io::CandidateRequest wrong = request;
    wrong.strategy_pack_version = "9.9.9";
    REQUIRE_THROWS_MATCHES(cs::io::select_strategy_pack(wrong, data), cs::io::ContractError,
                           MessageMatches(ContainsSubstring("not installed")));
}

TEST_CASE("generic execution is an explicit reserved pack, never an unknown-pack fallback",
          "[service][pack][derived]") {
    cs::io::CandidateRequest request;
    request.strategy_pack_id = cs::io::kDerivedStrategyPackId;
    request.strategy_pack_version = cs::io::kDerivedStrategyPackVersion;
    const cs::io::StrategyPackSelection selected =
        cs::io::select_strategy_pack(request, std::filesystem::path(CS_FIXTURE_DIR));
    REQUIRE(selected.derived);
    REQUIRE(selected.path.empty());
}

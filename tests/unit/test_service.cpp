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
  "schema_version":"cedh-deck-candidate.v1",
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
  "candidate_hash":")" + hash + "\"\n}";
}

std::string valid_hash() {
    const std::string canonical =
        std::string("{\"commander_oracle_ids\":[\"") + kCommander +
        "\"],\"library\":[{\"oracle_id\":\"" + kForest +
        "\",\"quantity\":99}],\"schema_version\":\"cedh-deck-candidate.v1\","
        "\"strategy_pack_id\":\"kinnan-midrange-goldfish\","
        "\"strategy_pack_version\":\"1.0.0\"}";
    return "sha256:" + cs::io::sha256_hex(canonical);
}

}  // namespace

TEST_CASE("SHA-256 uses the standard digest", "[service][hash]") {
    REQUIRE(cs::io::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("a candidate request enforces 99 cards and its semantic hash", "[service]") {
    SECTION("valid aggregate library") {
        const TempJson file(candidate_json(99, valid_hash()));
        const cs::io::CandidateRequest request = cs::io::load_candidate_request(file.path());
        REQUIRE(request.candidate_id == "fixture");
        REQUIRE(request.library[0].quantity == 99);
    }
    SECTION("98 cards is refused before simulation") {
        const TempJson file(candidate_json(98, valid_hash()));
        REQUIRE_THROWS_MATCHES(cs::io::load_candidate_request(file.path()), cs::io::LoadError,
                               MessageMatches(ContainsSubstring("expected exactly 99")));
    }
    SECTION("a stale hash is refused") {
        const TempJson file(candidate_json(99, "sha256:" + std::string(64, '0')));
        REQUIRE_THROWS_MATCHES(cs::io::load_candidate_request(file.path()), cs::io::LoadError,
                               MessageMatches(ContainsSubstring("does not match")));
    }
}

TEST_CASE("an unsupported strategy pack never falls back to Kinnan", "[service][pack]") {
    const cs::io::CandidateRequest request = cs::io::load_candidate_request(
        std::filesystem::path(CS_FIXTURE_DIR) / "unsupported-pack-candidate.v1.json");
    cs::io::DeckFile pack;
    pack.strategy_pack.id = "kinnan-midrange-goldfish";
    pack.strategy_pack.version = "1.0.0";
    const cs::CardDb db = cs::io::load_card_db(
        std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");

    REQUIRE_THROWS_MATCHES(cs::io::validate_candidate_for_pack(request, db, pack),
                           cs::io::LoadError,
                           MessageMatches(ContainsSubstring("generic Kinnan fallback is forbidden")));
}

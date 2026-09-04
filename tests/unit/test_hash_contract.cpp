// The cross-language hash contract.
//
// contracts/hash-golden-vectors.json is the single source of truth for both
// digests. The Python exporter, the Deck Lab, and this file all read the same
// file and must all reproduce it. That is the point: two implementations that
// each test themselves against themselves will agree with themselves forever
// and still disagree with each other, which is exactly how a deck hash and a
// deck-plus-pack hash came to share one field name.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "io/service.hpp"

namespace {

using json = nlohmann::json;

const std::filesystem::path kVectors =
    std::filesystem::path(CS_CONTRACTS_DIR) / "hash-golden-vectors.json";

const json& document() {
    static const json parsed = [] {
        std::ifstream stream(kVectors);
        REQUIRE(stream.good());
        return json::parse(stream);
    }();
    return parsed;
}

const json& vector_named(const std::string& name) {
    for (const json& entry : document().at("vectors")) {
        if (entry.at("name") == name) {
            return entry;
        }
    }
    FAIL("golden vector '" + name + "' is missing from " + kVectors.string());
    static const json unreachable;
    return unreachable;
}

cs::io::CandidateRequest request_from(const json& entry) {
    cs::io::CandidateRequest request;
    request.schema_version = cs::io::kCandidateSchemaVersion;
    request.candidate_id = entry.at("name");
    for (const json& id : entry.at("commander_oracle_ids")) {
        request.commander_oracle_ids.push_back(id.get<std::string>());
    }
    for (const json& card : entry.at("library")) {
        request.library.push_back(
            {card.at("oracle_id").get<std::string>(), card.at("quantity").get<int>()});
    }
    const json& execution = entry.at("execution");
    request.strategy_pack_id = execution.at("strategy_pack_id");
    request.strategy_pack_version = execution.at("strategy_pack_version");
    request.deck_sha256 = entry.at("deck_sha256");
    return request;
}

cs::io::SimulationInput input_from(const json& entry) {
    const json& execution = entry.at("execution");
    cs::io::SimulationInput input;
    input.deck_sha256 = entry.at("deck_sha256");
    input.strategy_pack_id = execution.at("strategy_pack_id");
    input.strategy_pack_version = execution.at("strategy_pack_version");
    input.strategy_pack_content_sha256 = execution.at("strategy_pack_content_sha256");
    input.strategy_pack_derived = execution.at("strategy_pack_derived");
    input.simulator_version = execution.at("simulator_version");
    input.card_data_manifest_hash = execution.at("card_data_manifest_hash");
    input.cards_sha256 = execution.at("cards_sha256");
    input.scenario_id = execution.at("scenario_id");
    input.scenario_version = execution.at("scenario_version");
    input.seed = execution.at("seed").get<std::uint64_t>();
    input.games = execution.at("games");
    input.objective_turn = execution.at("objective_turn");
    input.sweep = execution.at("sweep");
    for (const json& name : execution.at("ablations")) {
        input.ablations.push_back(name.get<std::string>());
    }
    return input;
}

std::string deck_hash_of(const std::string& name) {
    return cs::io::deck_sha256(request_from(vector_named(name)));
}

std::string input_hash_of(const std::string& name) {
    return cs::io::simulation_input_sha256(input_from(vector_named(name)));
}

}  // namespace

TEST_CASE("every golden vector's published digests are reproduced exactly") {
    for (const json& entry : document().at("vectors")) {
        const std::string name = entry.at("name");
        INFO("vector: " << name);
        CHECK(cs::io::deck_sha256(request_from(entry)) ==
              entry.at("deck_sha256").get<std::string>());
        CHECK(cs::io::simulation_input_sha256(input_from(entry)) ==
              entry.at("simulation_input_sha256").get<std::string>());
    }
}

TEST_CASE("ordering does not affect deck_sha256") {
    CHECK(deck_hash_of("base") == deck_hash_of("reordered_inputs"));
    CHECK(deck_hash_of("two_commanders") == deck_hash_of("two_commanders_reordered"));
}

TEST_CASE("changing the commander changes deck_sha256") {
    CHECK(deck_hash_of("base") != deck_hash_of("commander_changed"));
}

TEST_CASE("changing a library card changes deck_sha256") {
    CHECK(deck_hash_of("base") != deck_hash_of("card_changed"));
}

TEST_CASE("changing a quantity changes deck_sha256") {
    CHECK(deck_hash_of("base") != deck_hash_of("quantity_changed"));
}

TEST_CASE("changing the strategy pack does not change deck_sha256") {
    // ADR-025. A deck hash identifies a LIST, not a list plus the pack it was
    // built from and not a list plus whoever asked for it.
    CHECK(deck_hash_of("base") == deck_hash_of("pack_changed"));
}

TEST_CASE("changing the strategy pack does change simulation_input_sha256") {
    // The other half, and the reason two hashes exist: the same 99 cards under
    // a different pack is the same deck and a different measurement.
    CHECK(input_hash_of("base") != input_hash_of("pack_changed"));
}

TEST_CASE("run parameters move only the input fingerprint") {
    CHECK(deck_hash_of("base") == deck_hash_of("seed_changed"));
    CHECK(input_hash_of("base") != input_hash_of("seed_changed"));
}

TEST_CASE("ablation order does not affect simulation_input_sha256") {
    CHECK(input_hash_of("ablations_sorted") == input_hash_of("ablations_reordered"));
}

TEST_CASE("the golden file's own assertions are executable, not decorative") {
    // Walks the "assertions" block so a requirement added there without a
    // matching TEST_CASE above still fails the build rather than sitting
    // unchecked in a JSON file.
    const json& assertions = document().at("assertions");
    REQUIRE(assertions.size() >= 7);
    for (const json& rule : assertions) {
        INFO("requirement: " << rule.at("requirement").get<std::string>());
        const auto pair = [&](const char* key) {
            return std::pair<std::string, std::string>{rule.at(key).at(0), rule.at(key).at(1)};
        };
        if (rule.contains("equal_deck_sha256")) {
            const auto [left, right] = pair("equal_deck_sha256");
            CHECK(deck_hash_of(left) == deck_hash_of(right));
        }
        if (rule.contains("different_deck_sha256")) {
            const auto [left, right] = pair("different_deck_sha256");
            CHECK(deck_hash_of(left) != deck_hash_of(right));
        }
        if (rule.contains("equal_simulation_input_sha256")) {
            const auto [left, right] = pair("equal_simulation_input_sha256");
            CHECK(input_hash_of(left) == input_hash_of(right));
        }
        if (rule.contains("different_simulation_input_sha256")) {
            const auto [left, right] = pair("different_simulation_input_sha256");
            CHECK(input_hash_of(left) != input_hash_of(right));
        }
    }
}

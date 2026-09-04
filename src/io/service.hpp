#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "core/sim.hpp"
#include "core/stats.hpp"
#include "io/deck_load.hpp"

namespace cs::io {

inline constexpr const char* kCandidateSchemaVersion = "cedh-deck-candidate.v1";
inline constexpr const char* kResultSchemaVersion = "cedh-simulation-result.v1";
inline constexpr const char* kGoldfishScenarioId = "goldfish_assembly";
inline constexpr const char* kGoldfishScenarioVersion = "1.0.0";
inline constexpr const char* kDerivedStrategyPackId = "derived-generic";
inline constexpr const char* kDerivedStrategyPackVersion = "1.0.0";

struct CandidateCard {
    std::string oracle_id;
    int quantity = 0;
};

struct CandidateRequest {
    std::string schema_version;
    std::string candidate_id;
    std::vector<std::string> commander_oracle_ids;
    std::vector<CandidateCard> library;
    std::string strategy_pack_id;
    std::string strategy_pack_version;
    std::string candidate_hash;
};

// Parses the JSON contract and verifies its executable invariants: 99 library
// cards, unique/coalesced oracle IDs, and the semantic-content SHA-256.
[[nodiscard]] CandidateRequest load_candidate_request(const std::filesystem::path& path);

struct StrategyPackSelection {
    std::filesystem::path path;
    bool derived = false;
};

// Selects an installed *.deck.toml by the candidate's exact id and version.
// A file path is accepted for compatibility; a directory is the normal
// registry boundary. The reserved derived id selects generic execution rather
// than falling through from an unknown named pack.
[[nodiscard]] StrategyPackSelection select_strategy_pack(
    const CandidateRequest& request, const std::filesystem::path& registry);

// Refuses a pack/commander/snapshot mismatch. There is intentionally no
// generic fallback: Kinnan logic must never run for an unsupported candidate.
void validate_candidate_for_pack(const CandidateRequest& request, const CardDb& db,
                                 const DeckFile& pack);

// The snapshot check is shared by installed and derived execution.
void validate_candidate_snapshot(const CandidateRequest& request, const CardDb& db);

struct ServiceRunMetadata {
    int games = 0;
    std::uint64_t seed = 0;
    int objective_turn = 3;
    std::string scenario_id = kGoldfishScenarioId;
    std::string scenario_version = kGoldfishScenarioVersion;
    std::string started_at;
    std::string completed_at;
};

struct ServiceAblationResult {
    std::string oracle_id;
    std::string replacement_oracle_id;
    int objective_turn = 0;
    double delta = 0.0;
    double interval_low = 0.0;
    double interval_high = 0.0;
};

[[nodiscard]] std::string utc_timestamp_now();

// Serialises a schema-valid result. JSON remains private to this implementation
// so no nlohmann type leaks through the I/O boundary or into core.
[[nodiscard]] std::string build_simulation_result_json(
    const CandidateRequest& request, const CardDb& db, const DeckFile& pack,
    const EffectDb& effects, const GameConfig& config, const RunSummary& run,
    const ServiceRunMetadata& metadata,
    const std::vector<ServiceAblationResult>& ablations = {});

}  // namespace cs::io

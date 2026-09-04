#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "core/sim.hpp"
#include "core/stats.hpp"
#include "io/card_db_load.hpp"
#include "io/deck_load.hpp"

namespace cs::io {

inline constexpr const char* kCandidateSchemaVersion = "cedh-deck-candidate.v2";
inline constexpr const char* kResultSchemaVersion = "cedh-simulation-result.v3";
inline constexpr const char* kSimulationInputSchemaVersion = "cedh-simulation-input.v1";
inline constexpr const char* kGoldfishScenarioId = "goldfish_assembly";
inline constexpr const char* kGoldfishScenarioVersion = "1.0.0";
inline constexpr const char* kDerivedStrategyPackId = "derived-generic";
inline constexpr const char* kDerivedStrategyPackVersion = "1.0.0";

// The machine-readable reason a candidate or its execution context was
// refused. These are three genuinely different failures and the boundary must
// not blur them: only kDeckHashMismatch means "this is not the deck you think
// it is". A pack that is not installed says nothing whatsoever about the deck.
inline constexpr const char* kContractViolation = "contract_violation";
inline constexpr const char* kDeckHashMismatch = "deck_hash_mismatch";
inline constexpr const char* kExecutionContextUnsupported = "execution_context_unsupported";

// A refusal at the contract boundary, carrying its machine code so the HTTP
// service can map it without pattern-matching English prose.
class ContractError : public LoadError {
public:
    ContractError(std::string code, const std::string& what)
        : LoadError(what), code_(std::move(code)) {}
    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

struct CandidateCard {
    std::string oracle_id;
    int quantity = 0;
};

struct CandidateRequest {
    std::string schema_version;
    std::string candidate_id;
    std::vector<std::string> commander_oracle_ids;
    std::vector<CandidateCard> library;
    // The pack the producer ASKED for. What was actually resolved and run is
    // reported by the DeckFile, and only that goes into the run fingerprint.
    std::string strategy_pack_id;
    std::string strategy_pack_version;
    // The producer's claim about which list this is. Never trusted: it is
    // recomputed at load and compared.
    std::string deck_sha256;
};

// The deck list, and only the deck list.
//
// SHA-256 over "C:<oracle_id>\n" for each commander oracle_id in sorted order,
// then "<oracle_id>:<quantity>\n" for each library entry sorted by oracle_id,
// rendered "sha256:<lowercase hex>". The strategy pack, the requester, the
// simulator version and the corpus are all excluded, so the same 99 cards
// always hash the same way no matter who asked or how it will be run. The
// algorithm is the producer's, adopted verbatim under its ADR-025; pinned in
// contracts/hash-golden-vectors.json.
[[nodiscard]] std::string deck_sha256(const CandidateRequest& request);

// Everything that can change the numbers. Two runs sharing this fingerprint
// must produce identical statistics; two runs sharing only deck_sha256 need
// not, which is exactly why there are two hashes.
struct SimulationInput {
    std::string deck_sha256;
    std::string strategy_pack_id;
    std::string strategy_pack_version;
    std::string strategy_pack_content_sha256;
    bool strategy_pack_derived = false;
    std::string simulator_version;
    std::string card_data_manifest_hash;
    std::string cards_sha256;
    std::string scenario_id;
    std::string scenario_version;
    std::uint64_t seed = 0;
    int games = 0;
    int objective_turn = 0;
    bool sweep = false;
    std::vector<std::string> ablations;
};

[[nodiscard]] std::string simulation_input_sha256(const SimulationInput& input);

// Parses the JSON contract and verifies its executable invariants: 99 library
// cards, unique/coalesced oracle IDs, and an independently recomputed
// deck_sha256. Throws ContractError with kContractViolation for a malformed
// document and kDeckHashMismatch when the submitted hash does not describe
// the submitted list.
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
    // Part of the run fingerprint: a swept run and a single run over the same
    // deck are different measurements and must not share an identity.
    bool sweep = false;
    std::vector<std::string> ablations;
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

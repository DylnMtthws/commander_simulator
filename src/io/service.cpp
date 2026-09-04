#include "io/service.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include "core/version.hpp"
#include "io/card_db_load.hpp"
#include "io/sha256.hpp"

namespace cs::io {
namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) { throw LoadError(message); }

const json& require(const json& node, std::string_view key, std::string_view context) {
    const auto found = node.find(key);
    if (found == node.end()) {
        fail(std::string(context) + ": missing required field '" + std::string(key) + "'");
    }
    return *found;
}

std::string require_string(const json& node, std::string_view key, std::string_view context) {
    const json& value = require(node, key, context);
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        fail(std::string(context) + ": field '" + std::string(key) +
             "' must be a non-empty string");
    }
    return value.get<std::string>();
}

json semantic_document(const CandidateRequest& request) {
    std::vector<std::string> commanders = request.commander_oracle_ids;
    std::sort(commanders.begin(), commanders.end());
    std::vector<CandidateCard> library = request.library;
    std::sort(library.begin(), library.end(), [](const CandidateCard& left,
                                                  const CandidateCard& right) {
        return left.oracle_id < right.oracle_id;
    });
    json cards = json::array();
    for (const CandidateCard& card : library) {
        cards.push_back({{"oracle_id", card.oracle_id}, {"quantity", card.quantity}});
    }
    // nlohmann::json uses std::map by default, so dump() emits object keys in
    // lexical order. Arrays are explicitly sorted above.
    return {{"commander_oracle_ids", commanders},
            {"library", cards},
            {"schema_version", request.schema_version},
            {"strategy_pack_id", request.strategy_pack_id},
            {"strategy_pack_version", request.strategy_pack_version}};
}

json interval_json(const Interval& interval) {
    return {{"low", interval.low}, {"high", interval.high}};
}

json optional_turn(std::optional<int> turn) {
    return turn.has_value() ? json(*turn) : json(nullptr);
}

std::string manifest_hash(const Manifest& manifest) {
    const json canonical{{"card_count", manifest.card_count},
                         {"cards_sha256", manifest.cards_sha256},
                         {"corpus_row_count", manifest.corpus_row_count},
                         {"generated_at", manifest.generated_at},
                         {"max_content_updated_at", manifest.max_content_updated_at},
                         {"source_view", manifest.source_view}};
    return "sha256:" + sha256_hex(canonical.dump());
}

}  // namespace

CandidateRequest load_candidate_request(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        fail("cannot open candidate request " + path.string());
    }
    json document;
    try {
        document = json::parse(stream, nullptr, true);
    } catch (const json::parse_error& error) {
        fail("malformed JSON in " + path.string() + ": " + error.what());
    }
    if (!document.is_object()) {
        fail(path.string() + ": candidate request must be a JSON object");
    }

    CandidateRequest request;
    request.schema_version = require_string(document, "schema_version", "candidate");
    if (request.schema_version != kCandidateSchemaVersion) {
        fail("candidate: unsupported schema_version '" + request.schema_version +
             "'; supported: " + kCandidateSchemaVersion);
    }
    request.candidate_id = require_string(document, "candidate_id", "candidate");
    request.strategy_pack_id = require_string(document, "strategy_pack_id", "candidate");
    request.strategy_pack_version =
        require_string(document, "strategy_pack_version", "candidate");
    request.candidate_hash = require_string(document, "candidate_hash", "candidate");

    const json& commanders = require(document, "commander_oracle_ids", "candidate");
    if (!commanders.is_array() || commanders.empty() || commanders.size() > 2) {
        fail("candidate: commander_oracle_ids must contain one or two IDs");
    }
    std::set<std::string> unique_commanders;
    for (const json& value : commanders) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            fail("candidate: every commander_oracle_ids entry must be a string");
        }
        const std::string id = value.get<std::string>();
        if (!unique_commanders.insert(id).second) {
            fail("candidate: duplicate commander oracle_id '" + id + "'");
        }
        request.commander_oracle_ids.push_back(id);
    }

    const json& library = require(document, "library", "candidate");
    if (!library.is_array() || library.empty()) {
        fail("candidate: library must be a non-empty array");
    }
    std::set<std::string> unique_library;
    int library_count = 0;
    for (const json& value : library) {
        if (!value.is_object()) {
            fail("candidate: every library entry must be an object");
        }
        CandidateCard card;
        card.oracle_id = require_string(value, "oracle_id", "candidate library entry");
        const json& quantity = require(value, "quantity", "candidate library entry");
        if (!quantity.is_number_integer() || quantity.get<int>() < 1) {
            fail("candidate library entry: quantity must be a positive integer");
        }
        card.quantity = quantity.get<int>();
        if (!unique_library.insert(card.oracle_id).second) {
            fail("candidate: oracle_id '" + card.oracle_id +
                 "' appears more than once; coalesce it with quantity");
        }
        library_count += card.quantity;
        request.library.push_back(std::move(card));
    }
    if (library_count != 99) {
        fail("candidate: library quantities sum to " + std::to_string(library_count) +
             ", expected exactly 99");
    }

    const std::string expected = "sha256:" + sha256_hex(semantic_document(request).dump());
    if (request.candidate_hash != expected) {
        fail("candidate: candidate_hash does not match semantic content; expected " + expected);
    }
    // Provenance does not affect simulation semantics, but it is required
    // audit data. Check its contract shape here so the service cannot accept a
    // candidate which only an out-of-process schema validator would reject.
    const json& provenance = require(document, "provenance", "candidate");
    if (!provenance.is_object()) {
        fail("candidate: provenance must be an object");
    }
    const json& producer = require(provenance, "producer", "candidate provenance");
    if (!producer.is_object()) {
        fail("candidate provenance: producer must be an object");
    }
    require_string(producer, "name", "candidate provenance producer");
    require_string(producer, "version", "candidate provenance producer");
    for (const char* field : {"card_data", "corpus"}) {
        const json& source = require(provenance, field, "candidate provenance");
        if (!source.is_object()) {
            fail("candidate provenance: " + std::string(field) + " must be an object");
        }
        require_string(source, "source", "candidate provenance " + std::string(field));
        require_string(source, "snapshot_at", "candidate provenance " + std::string(field));
        require_string(source, "hash", "candidate provenance " + std::string(field));
    }
    return request;
}

StrategyPackSelection select_strategy_pack(const CandidateRequest& request,
                                           const std::filesystem::path& registry) {
    if (request.strategy_pack_id == kDerivedStrategyPackId &&
        request.strategy_pack_version == kDerivedStrategyPackVersion) {
        return {.path = {}, .derived = true};
    }

    std::vector<std::filesystem::path> candidates;
    if (std::filesystem::is_regular_file(registry)) {
        candidates.push_back(registry);
    } else if (std::filesystem::is_directory(registry)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(registry)) {
            const std::string filename = entry.path().filename().string();
            if (entry.is_regular_file() && filename.ends_with(".deck.toml")) {
                candidates.push_back(entry.path());
            }
        }
        std::sort(candidates.begin(), candidates.end());
    } else {
        fail("strategy-pack registry does not exist: " + registry.string());
    }

    std::filesystem::path selected;
    for (const std::filesystem::path& path : candidates) {
        toml::table root;
        try {
            root = toml::parse_file(path.string());
        } catch (const toml::parse_error& error) {
            fail("cannot parse installed strategy pack " + path.string() + ": " +
                 std::string(error.description()));
        }
        const auto* pack = root["strategy_pack"].as_table();
        if (pack == nullptr) {
            fail(path.string() + ": installed *.deck.toml is missing [strategy_pack]");
        }
        const std::string id = (*pack)["id"].value_or<std::string>("");
        const std::string version = (*pack)["version"].value_or<std::string>("");
        if (id == request.strategy_pack_id && version == request.strategy_pack_version) {
            if (!selected.empty()) {
                fail("strategy-pack registry contains duplicate '" + id + "@" + version + "'");
            }
            selected = path;
        }
    }
    if (selected.empty()) {
        fail("candidate requests strategy pack '" + request.strategy_pack_id + "@" +
             request.strategy_pack_version +
             "', but that exact pack is not installed. Unknown named packs are rejected; "
             "use derived-generic@1.0.0 to request explicit derived execution.");
    }
    return {.path = selected, .derived = false};
}

void validate_candidate_snapshot(const CandidateRequest& request, const CardDb& db) {
    std::map<std::string, int> snapshot_commanders;
    std::map<std::string, int> snapshot_library;
    for (const Card& card : db.cards) {
        ++(card.is_commander ? snapshot_commanders : snapshot_library)[card.oracle_id];
    }
    std::map<std::string, int> candidate_commanders;
    for (const std::string& id : request.commander_oracle_ids) {
        ++candidate_commanders[id];
    }
    std::map<std::string, int> candidate_library;
    for (const CandidateCard& card : request.library) {
        candidate_library[card.oracle_id] = card.quantity;
    }
    if (candidate_commanders != snapshot_commanders || candidate_library != snapshot_library) {
        fail("candidate oracle IDs/quantities do not match the enriched --cards snapshot. "
             "Export that candidate with mtgsim-export --candidate before simulating it.");
    }
}

void validate_candidate_for_pack(const CandidateRequest& request, const CardDb& db,
                                 const DeckFile& pack) {
    if (request.strategy_pack_id != pack.strategy_pack.id ||
        request.strategy_pack_version != pack.strategy_pack.version) {
        fail("candidate requests strategy pack '" + request.strategy_pack_id + "@" +
             request.strategy_pack_version + "', but this simulator loaded '" +
             pack.strategy_pack.id + "@" + pack.strategy_pack.version +
             "'. Unsupported packs are rejected; generic Kinnan fallback is forbidden.");
    }

    std::vector<std::string> requested_commanders = request.commander_oracle_ids;
    std::vector<std::string> supported_commanders =
        pack.strategy_pack.supported_commander_oracle_ids;
    std::sort(requested_commanders.begin(), requested_commanders.end());
    std::sort(supported_commanders.begin(), supported_commanders.end());
    if (requested_commanders != supported_commanders) {
        fail("candidate commander oracle IDs are not supported by strategy pack '" +
             pack.strategy_pack.id + "'; refusing to run commander-specific logic");
    }

    validate_candidate_snapshot(request, db);
}

std::string utc_timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string build_simulation_result_json(const CandidateRequest& request, const CardDb& db,
                                         const DeckFile& pack, const EffectDb& effects,
                                         const GameConfig& config, const RunSummary& run,
                                         const ServiceRunMetadata& metadata,
                                         const std::vector<ServiceAblationResult>& ablations) {
    json cdf = json::array();
    for (int turn = 1; turn <= config.turn_cap; ++turn) {
        const int assembled = cumulative(run, turn);
        const Interval interval = wilson(assembled, run.games);
        cdf.push_back({{"turn", turn},
                       {"assembled_games", assembled},
                       {"probability", static_cast<double>(assembled) / run.games},
                       {"wilson_95", interval_json(interval)}});
    }

    json percentiles = json::array();
    for (const double p : {0.10, 0.25, 0.50, 0.75, 0.90}) {
        const Percentile value = percentile(run, p);
        percentiles.push_back(
            {{"percentile", p},
             {"status", value.turn.has_value() ? "observed" : "censored"},
             {"turn", optional_turn(value.turn)},
             {"interval_95",
              {{"low_turn", optional_turn(value.low)}, {"high_turn", optional_turn(value.high)}}}});
    }

    json inert_by_reason = json::object();
    for (std::size_t i = 0; i < effects.inert_categories.size(); ++i) {
        inert_by_reason[effects.inert_categories[i]] = effects.inert_counts[i];
    }

    json pattern_json = json::array();
    for (const WinPattern& pattern : pack.patterns.patterns) {
        const bool inherited = std::find(pack.patterns.inherited_pattern_names.begin(),
                                         pack.patterns.inherited_pattern_names.end(),
                                         pattern.name) !=
                               pack.patterns.inherited_pattern_names.end();
        pattern_json.push_back({{"name", pattern.name},
                                {"source", inherited ? "library" : "pack"},
                                {"terminal_state", pattern.terminal_state}});
    }

    json assumptions = json::array();
    const auto add_assumption = [&](const char* code, const std::string& detail) {
        assumptions.push_back({{"code", code}, {"detail", detail}});
    };
    add_assumption("no_opponents",
                   "No opponents are simulated; table fields are declared card-text context only.");
    add_assumption("no_stack", "The stack, priority, responses, and live interaction are unsupported.");
    add_assumption("no_combat", "Combat, attacks, damage, and opponent-facing kills are unsupported.");
    add_assumption("assembly_proxy", "A declared assembly state ends a game; it is not a demonstrated win.");
    add_assumption("not_general_rules_engine",
                   "Only explicitly authored effects execute; unlisted cards are reported unauthored.");

    json warnings = json::array(
        {"Assembly probability is not win rate, deck strength, or card quality."});
    if (effects.inert > 0) {
        warnings.push_back(std::to_string(effects.inert) +
                           " cards are explicitly inert under this scenario.");
    }
    if (effects.unauthored > 0) {
        for (const std::string& name : effects.unauthored_names) {
            warnings.push_back(name +
                               " is unauthored and therefore dilutes draws without executing text.");
        }
    }
    if (effects.disputed > 0) {
        warnings.push_back(std::to_string(effects.disputed) +
                           " inert classification is explicitly disputed by the strategy-pack author.");
    }
    if (pack.patterns.patterns.empty()) {
        warnings.push_back(
            "No card-set pattern is known for this candidate; every game is censored and no "
            "assembly claim is made.");
    }
    if (ablations.empty()) {
        warnings.push_back("No ablations were requested; ablation_results is empty.");
    }

    json ablation_json = json::array();
    for (const ServiceAblationResult& ablation : ablations) {
        ablation_json.push_back(
            {{"oracle_id", ablation.oracle_id},
             {"replacement_oracle_id", ablation.replacement_oracle_id},
             {"objective_turn", ablation.objective_turn},
             {"goldfish_turn_to_assembly_delta", ablation.delta},
             {"interval_95", {{"low", ablation.interval_low},
                              {"high", ablation.interval_high}}},
             {"method", "paired_common_random_numbers"}});
    }

    std::ostringstream digest;
    digest << std::hex << std::setfill('0') << std::setw(16) << run.digest_xor;
    const std::string run_material = std::string(version()) + ":" +
                                     request.candidate_hash + ":" +
                                     manifest_hash(db.manifest) + ":" +
                                     std::to_string(metadata.games) + ":" +
                                     std::to_string(metadata.seed) + ":" +
                                     std::to_string(metadata.objective_turn) + ":" +
                                     metadata.scenario_id + "@" + metadata.scenario_version;

    json document{
        {"schema_version", kResultSchemaVersion},
        {"run_id", "run-" + sha256_hex(run_material).substr(0, 24)},
        {"timestamps", {{"started_at", metadata.started_at}, {"completed_at", metadata.completed_at}}},
        {"simulator", {{"version", std::string(version())}, {"build_flavour", std::string(build_flavour())}}},
        {"metric", {{"id", "goldfish_turns_to_assembly"},
                    {"measures", "turns until a declared pattern is assembled, unopposed"},
                    {"does_not_measure", "deck strength, win rate, or card quality"}}},
        {"candidate", {{"candidate_id", request.candidate_id}, {"candidate_hash", request.candidate_hash}}},
        {"strategy_pack", {{"id", pack.strategy_pack.id},
                           {"version", pack.strategy_pack.version},
                           {"derived", pack.derived},
                           {"play_policy", pack.strategy_pack.play_policy_implementation},
                           {"assembly_objectives", pack.strategy_pack.assembly_objectives},
                           {"patterns", pattern_json},
                           {"inherited_patterns", pack.patterns.inherited_pattern_names},
                           {"rank_overrides", pack.rank_override_names},
                           {"declared_table_assumptions", pack.strategy_pack.declared_table_assumptions},
                           {"known_blind_spots", pack.strategy_pack.known_blind_spots}}},
        {"card_data", {{"manifest_hash", manifest_hash(db.manifest)},
                       {"cards_sha256", db.manifest.cards_sha256},
                       {"source_view", db.manifest.source_view},
                       {"max_content_updated_at", db.manifest.max_content_updated_at},
                       {"corpus_row_count", db.manifest.corpus_row_count}}},
        {"simulation", {{"games", metadata.games},
                        {"seed", metadata.seed},
                        {"objective_turn", metadata.objective_turn},
                        {"requested_scenario", {{"id", metadata.scenario_id},
                                                {"version", metadata.scenario_version}}}}},
        {"coverage", {{"total_cards", static_cast<int>(db.size())},
                      {"library_cards", 99},
                      {"commander_cards", static_cast<int>(request.commander_oracle_ids.size())},
                      {"modeled_cards", effects.modeled},
                      {"inert_cards", effects.inert},
                      {"unauthored_cards", effects.unauthored},
                      {"inert_by_reason", inert_by_reason}}},
        {"assembly_cdf", cdf},
        {"censored", {{"games", run.censored},
                      {"rate", static_cast<double>(run.censored) / run.games},
                      {"wilson_95", interval_json(wilson(run.censored, run.games))}}},
        {"percentiles", percentiles},
        {"ablation_results", ablation_json},
        {"warnings", warnings},
        {"unsupported_assumptions", assumptions},
        {"determinism", {{"seed_scheme", "splitmix64(base_seed ^ splitmix64(game_index)) + xoshiro256++"},
                         {"result_digest", digest.str()}}}};
    return document.dump(2) + "\n";
}

}  // namespace cs::io

#include "io/resource_probe_io.hpp"
#include "io/sha256.hpp"
#include "core/resource_probe.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <stdexcept>

namespace cs::io {
namespace {
using J = nlohmann::json;
int integer(const J& j, const char* key, int low, int high) {
    const auto& value=j.at(key);
    if (!value.is_number_integer()) throw std::invalid_argument("expected integer");
    const auto n=value.get<std::int64_t>();
    if (n<low || n>high) throw std::invalid_argument("integer out of bounds");
    return static_cast<int>(n);
}
void keys(const J& j, const std::set<std::string>& allowed) {
    if (!j.is_object() || j.size()!=allowed.size()) throw std::invalid_argument("object fields mismatch");
    for (const auto& [key,value]: j.items()) {
        (void)value;
        if (!allowed.contains(key)) throw std::invalid_argument("unknown field");
    }
}
}
std::string run_resource_request(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path)>1024*1024) throw std::invalid_argument("request too large");
    std::ifstream f(path);
    J j=J::parse(f);
    keys(j,{"schema_version","cards","commander_cost","extra_land_plays","landfall_draw","games","turns","seed","deck_identity"});
    if (j.at("schema_version")!="resource-probe-request.v1") throw std::invalid_argument("unsupported schema");
    ProbeConfig config;
    config.games=integer(j,"games",1,20000);
    config.turns=integer(j,"turns",1,12);
    config.extra_land_plays=integer(j,"extra_land_plays",0,2);
    config.landfall_draw=integer(j,"landfall_draw",0,2);
    config.seed=static_cast<std::uint64_t>(integer(j,"seed",0,2147483647));
    const auto& cost=j.at("commander_cost");
    keys(cost,{"generic","pips"});
    config.commander_cost.generic=static_cast<std::uint8_t>(integer(cost,"generic",0,30));
    const auto& pips=cost.at("pips");
    if (!pips.is_array() || pips.size()!=5) throw std::invalid_argument("five colored pips required");
    for (std::size_t i=0; i<5; ++i) {
        J single={{"pip",pips[i]}};
        config.commander_cost.pips[i]=static_cast<std::uint8_t>(integer(single,"pip",0,10));
    }
    const auto& input=j.at("cards");
    if (!input.is_array() || input.size()!=99) throw std::invalid_argument("99 card slots required");
    std::vector<ProbeCard> cards;
    J ids=J::array();
    for (const auto& c: input) {
        keys(c,{"id","land","colors","tapped"});
        if (!c.at("id").is_string() || c.at("id").get<std::string>().empty() || !c.at("land").is_boolean() || !c.at("tapped").is_boolean()) throw std::invalid_argument("invalid card");
        ProbeCard card;
        card.land=c.at("land").get<bool>();
        card.tapped=c.at("tapped").get<bool>();
        card.colors=static_cast<ColourMask>(integer(c,"colors",0,31));
        if (!card.land && (card.tapped || card.colors!=0)) throw std::invalid_argument("nonland source in land-only scenario");
        cards.push_back(card);
        ids.push_back(c.at("id"));
    }
    // The ordered slot list is deliberate: common random numbers pair slot
    // positions across substitutions. The compiled request hash captures that.
    const auto identity="sha256:"+sha256_hex(ids.dump());
    if (j.at("deck_identity")!=identity) throw std::invalid_argument("deck identity mismatch");
    const auto result=resource_probe(cards,config);
    J output={{"schema_version","resource-probe-result.v1"},
        {"scenario","commander-land-engine-only.v1"}, {"deck_identity",identity},
        {"simulation_input_sha256","sha256:"+sha256_hex("resource-probe.v1\n"+j.dump())},
        {"games",result.games}, {"turns",config.turns},
        {"commander_cast_count",result.commander_cast},
        {"lands_mean",result.lands_mean}, {"commander_draws_mean",result.commander_draws_mean},
        {"cast_samples",result.cast_samples},
        {"assumptions",{"opening seven; no mulligan", "draw on turn one", "no noncommander spells, interaction or combat", "unconditional one-mana lands only", "optional declared commander land allowance/draw only"}}};
    return output.dump();
}
}

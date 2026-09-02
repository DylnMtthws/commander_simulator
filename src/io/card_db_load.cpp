#include "io/card_db_load.hpp"

#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace cs::io {
namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) { throw LoadError(message); }

// Every field access goes through these. nlohmann's operator[] on a missing key
// returns null and then converts to a zero-ish default, which is precisely the
// silent-wrong-value failure this project keeps finding: a card that quietly
// costs nothing looks exactly like a fast deck. So: check presence, check type,
// and name the field when either fails.
const json& require(const json& node, std::string_view key, std::string_view context) {
    const auto it = node.find(key);
    if (it == node.end()) {
        fail(std::string(context) + ": missing required field '" + std::string(key) + "'");
    }
    return *it;
}

int require_int(const json& node, std::string_view key, std::string_view context) {
    const json& value = require(node, key, context);
    if (!value.is_number_integer()) {
        fail(std::string(context) + ": field '" + std::string(key) + "' must be an integer, got " +
             std::string(value.type_name()));
    }
    return value.get<int>();
}

std::string require_string(const json& node, std::string_view key, std::string_view context) {
    const json& value = require(node, key, context);
    if (!value.is_string()) {
        fail(std::string(context) + ": field '" + std::string(key) + "' must be a string, got " +
             std::string(value.type_name()));
    }
    return value.get<std::string>();
}

bool require_bool(const json& node, std::string_view key, std::string_view context) {
    const json& value = require(node, key, context);
    if (!value.is_boolean()) {
        fail(std::string(context) + ": field '" + std::string(key) + "' must be a boolean, got " +
             std::string(value.type_name()));
    }
    return value.get<bool>();
}

std::string optional_string(const json& node, std::string_view key) {
    const auto it = node.find(key);
    return (it != node.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

std::size_t colour_index(const std::string& letter, std::string_view context) {
    // Matches the exporter's WUBRG order. A letter outside it is an error, not
    // a skip - the same closed-set discipline as the cost parser upstream.
    static constexpr std::string_view kOrder = "WUBRG";
    const auto pos = kOrder.find(letter);
    if (letter.size() != 1 || pos == std::string_view::npos) {
        fail(std::string(context) + ": unknown colour '" + letter + "', expected one of WUBRG");
    }
    return pos;
}

Cost parse_cost(const json& node, std::string_view context) {
    Cost cost;
    const int generic = require_int(node, "generic", context);
    const int variable = require_int(node, "variable", context);
    if (generic < 0 || variable < 0) {
        fail(std::string(context) + ": cost components must not be negative");
    }
    cost.generic = static_cast<std::uint8_t>(generic);
    cost.variable = static_cast<std::uint8_t>(variable);

    for (const auto& pip : require(node, "pips", context)) {
        ++cost.pips[colour_index(pip.get<std::string>(), context)];
    }
    for (const auto& pip : require(node, "phyrexian", context)) {
        ++cost.phyrexian[colour_index(pip.get<std::string>(), context)];
    }
    return cost;
}

Face parse_face(const json& node, const std::string& card_name) {
    const std::string context = card_name + " face";
    Face face;
    face.name = require_string(node, "name", context);
    face.type_line = require_string(node, "type_line", context);
    face.is_land = require_bool(node, "is_land", context);

    const json& mv = require(node, "mana_value", context);
    if (!mv.is_null()) {
        face.mana_value = mv.get<int>();
    }

    // null is meaningful and is not zero: an uncastable face has no cost.
    const json& cost = require(node, "cost", context);
    if (!cost.is_null()) {
        face.cost = parse_cost(cost, context);
        if (face.mana_value.has_value() &&
            face.cost->mana_value_at_x_zero() != *face.mana_value) {
            fail(context + " '" + face.name + "': cost implies mana value " +
                 std::to_string(face.cost->mana_value_at_x_zero()) + " but the file says " +
                 std::to_string(*face.mana_value));
        }
    }
    return face;
}

Card parse_card(const json& node) {
    const std::string listed = require_string(node, "listed_name", "card");
    Card card;
    card.export_index = require_int(node, "export_index", listed);
    card.listed_name = listed;
    card.name = require_string(node, "name", listed);
    card.layout = require_string(node, "layout", listed);
    card.mana_value = require_int(node, "mana_value", listed);
    card.has_land_face = require_bool(node, "has_land_face", listed);
    card.is_commander = require_bool(node, "is_commander", listed);

    for (const auto& value : require(node, "castable_cmcs", listed)) {
        card.castable_cmcs.push_back(value.get<int>());
    }
    for (const auto& value : require(node, "all_types", listed)) {
        card.all_types.push_back(value.get<std::string>());
    }
    for (const auto& value : require(node, "color_identity", listed)) {
        const auto index = colour_index(value.get<std::string>(), listed);
        card.colour_identity |= static_cast<ColourMask>(1U << index);
    }

    const json& faces = require(node, "faces", listed);
    if (!faces.is_array() || faces.empty()) {
        fail(listed + ": 'faces' must be a non-empty array. The exporter synthesises a "
                      "face for single-faced cards so this is never empty.");
    }
    for (const auto& face : faces) {
        card.faces.push_back(parse_face(face, listed));
    }
    return card;
}

Manifest parse_manifest(const json& node) {
    Manifest manifest;
    manifest.source_view = require_string(node, "source_view", "manifest");
    manifest.card_count = require_int(node, "card_count", "manifest");
    manifest.cards_sha256 = require_string(node, "cards_sha256", "manifest");
    manifest.generated_at = optional_string(node, "generated_at");
    manifest.max_content_updated_at = optional_string(node, "max_content_updated_at");
    manifest.metric = optional_string(node, "metric");
    manifest.measures = optional_string(node, "measures");
    manifest.does_not_measure = optional_string(node, "does_not_measure");

    const auto corpus = node.find("corpus_row_count");
    if (corpus != node.end() && corpus->is_number_integer()) {
        manifest.corpus_row_count = corpus->get<int>();
    }
    const auto fixture = node.find("is_fixture");
    manifest.is_fixture = fixture != node.end() && fixture->is_boolean() && fixture->get<bool>();
    return manifest;
}

}  // namespace

CardDb load_card_db(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        fail("cannot open " + path.string() +
             ". Card data is generated, not committed - run the exporter "
             "(see README, 'Getting this to run').");
    }

    json document;
    try {
        // Second argument nullptr: no callback. Third: throw on parse error
        // rather than returning a discarded value that would then fail later
        // with a message about a missing field instead of about broken JSON.
        document = json::parse(stream, nullptr, true);
    } catch (const json::parse_error& error) {
        fail("malformed JSON in " + path.string() + ": " + error.what());
    }

    if (!document.is_object()) {
        fail(path.string() + ": top level must be an object with 'manifest' and 'cards'");
    }

    CardDb db;
    db.manifest = parse_manifest(require(document, "manifest", path.string()));

    const json& cards = require(document, "cards", path.string());
    if (!cards.is_array()) {
        fail(path.string() + ": 'cards' must be an array");
    }
    for (const auto& card : cards) {
        db.cards.push_back(parse_card(card));
    }

    // RULE C1 on this side of the boundary too. The manifest states a count;
    // the array has a length. A file where they disagree has been truncated or
    // edited, and loading it would silently simulate a different deck.
    if (static_cast<int>(db.cards.size()) != db.manifest.card_count) {
        fail(path.string() + ": manifest says " + std::to_string(db.manifest.card_count) +
             " cards but the array has " + std::to_string(db.cards.size()));
    }

    // export_index is a dense slot number: it is the bit position this card
    // occupies in the zone bitsets, and the final tiebreak that makes policy
    // orderings total. A duplicate silently merges two cards; a gap leaves a
    // bit nothing owns.
    std::set<int> seen;
    for (const auto& card : db.cards) {
        if (!seen.insert(card.export_index).second) {
            fail(path.string() + ": duplicate export_index " +
                 std::to_string(card.export_index) + " (" + card.listed_name + ")");
        }
    }
    const int count = static_cast<int>(db.cards.size());
    if (!seen.empty() && (*seen.begin() != 0 || *seen.rbegin() != count - 1)) {
        fail(path.string() + ": export_index must be dense 0.." + std::to_string(count - 1) +
             ", got " + std::to_string(*seen.begin()) + ".." + std::to_string(*seen.rbegin()));
    }
    return db;
}

}  // namespace cs::io

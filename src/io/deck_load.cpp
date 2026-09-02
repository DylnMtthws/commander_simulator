#include "io/deck_load.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

namespace cs::io {
namespace {

[[noreturn]] void fail(const std::string& message) { throw DeckError(message); }

// The closed vocabulary of pattern terms (SIM_PLAN.md section 5.2).
//
// Anything not on this list is REJECTED BY NAME. That matters more than it
// looks: a term silently ignored would make its pattern strictly easier to
// satisfy, so the pattern would fire early and often and the deck would look
// fast. And a term that is in the plan but not yet implemented must fail here
// too, or the pattern quietly never fires - the same failure class as a
// mechanism not running.
constexpr std::string_view kZoneTerms[] = {"in_play", "in_hand", "in_play_or_hand", "untapped",
                                           "any_of"};
constexpr std::string_view kScalarTerms[] = {"turn_gte", "creature_count_gte",
                                            "library_size_lte", "loop_entry_cost",
                                            "activations"};
constexpr std::string_view kOtherTerms[] = {"flag", "flags", "entry_pips"};

// Declared in section 5.2 but not implemented yet. Named separately so the
// error says "not implemented" rather than "unknown", which is the difference
// between a typo and a missing feature.
constexpr std::string_view kNotYetImplemented[] = {"available_mana", "resolved",
                                                   "devotion_blue_gte_library", "attached",
                                                   "imprinted"};

bool contains(auto&& list, std::string_view key) {
    return std::find(std::begin(list), std::end(list), key) != std::end(list);
}

// Slot index for a card name, by listed name then by stored name.
int slot_for(const CardDb& db, const std::string& name, const std::string& context) {
    for (const Card& card : db.cards) {
        if (card.listed_name == name || card.name == name) {
            return card.export_index;
        }
    }
    fail(context + ": names card '" + name +
         "', which is not in the deck. Every card named in a pattern must be in the "
         "deck list (SIM_PLAN.md section 9.3).");
}

Zone zone_from(const toml::node& node, const CardDb& db, const std::string& context) {
    const auto* array = node.as_array();
    if (array == nullptr) {
        fail(context + ": expected an array of card names");
    }
    Zone zone;
    for (const toml::node& entry : *array) {
        const auto name = entry.value<std::string>();
        if (!name) {
            fail(context + ": card names must be strings");
        }
        zone.set(slot_for(db, *name, context));
    }
    return zone;
}

int flag_index(std::vector<std::string>& names, const std::string& flag,
               const std::string& context) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == flag) {
            return static_cast<int>(i);
        }
    }
    if (names.size() >= kMaxFlags) {
        fail(context + ": more than " + std::to_string(kMaxFlags) + " distinct flags");
    }
    names.push_back(flag);
    return static_cast<int>(names.size() - 1);
}

Requirement parse_requirement(const toml::table& table, const CardDb& db, PatternSet& set,
                              const std::string& context, bool allow_flags) {
    Requirement requirement;
    bool saw_activations = false;
    bool saw_pips = false;
    for (const auto& [key, value] : table) {
        const std::string_view name{key.str()};
        if (contains(kNotYetImplemented, name)) {
            fail(context + ": term '" + std::string(name) +
                 "' is in the section 5.2 vocabulary but NOT IMPLEMENTED. Failing rather "
                 "than ignoring it: an ignored term makes its pattern easier to satisfy, "
                 "so the pattern would fire early and the deck would look fast.");
        }
        if (contains(kZoneTerms, name)) {
            Zone zone = zone_from(value, db, context + " " + std::string(name));
            if (name == "in_play") requirement.in_play = zone;
            else if (name == "in_hand") requirement.in_hand = zone;
            else if (name == "in_play_or_hand") requirement.in_play_or_hand = zone;
            else if (name == "untapped") requirement.untapped = zone;
            else { requirement.any_of = zone; requirement.has_any_of = true; }
        } else if (contains(kScalarTerms, name)) {
            const auto number = value.value<int64_t>();
            if (!number) {
                fail(context + ": '" + std::string(name) + "' must be an integer");
            }
            if (name == "turn_gte") requirement.turn_gte = static_cast<int>(*number);
            else if (name == "creature_count_gte")
                requirement.creature_count_gte = static_cast<int>(*number);
            else if (name == "loop_entry_cost")
                requirement.loop_entry_cost = static_cast<int>(*number);
            else if (name == "activations") {
                requirement.activations = static_cast<int>(*number);
                saw_activations = true;
            }
            else requirement.library_size_lte = static_cast<int>(*number);
        } else if (name == "entry_pips") {
            // The coloured half of an entry cost (R2, section 16.7). Written as
            // colour letters rather than a mana string, because the pattern
            // vocabulary has no cost parser and adding one to express two pips
            // would be more machinery than the thing it expresses.
            const auto* array = value.as_array();
            if (array == nullptr) {
                fail(context + ": entry_pips must be an array of colour letters, e.g. "
                               "[\"G\", \"U\"]");
            }
            for (const toml::node& entry : *array) {
                const auto letter = entry.value<std::string>();
                static constexpr std::string_view kOrder = "WUBRG";
                const auto at = letter ? kOrder.find(*letter) : std::string_view::npos;
                if (!letter || letter->size() != 1 || at == std::string_view::npos) {
                    fail(context + ": unknown colour in entry_pips; expected letters from WUBRG");
                }
                ++requirement.entry_pips[at];
            }
            saw_pips = true;
        } else if (contains(kOtherTerms, name)) {
            if (!allow_flags) {
                fail(context +
                     ": an engine may not require a flag. Engines set flags; wins consume "
                     "them. Exactly one level of indirection (section 5.1).");
            }
            std::vector<std::string> flags;
            if (const auto single = value.value<std::string>()) {
                flags.push_back(*single);
            } else if (const auto* array = value.as_array()) {
                for (const toml::node& entry : *array) {
                    if (const auto flag = entry.value<std::string>()) flags.push_back(*flag);
                }
            }
            for (const std::string& flag : flags) {
                requirement.flags |= FlagMask{1} << flag_index(set.flag_names, flag, context);
            }
        } else {
            fail(context + ": unknown pattern term '" + std::string(name) +
                 "'. The vocabulary is closed (section 5.2) - a term outside it is a typo "
                 "or a feature nobody implemented, and both must fail here.");
        }
    }
    // REQUIRED WHERE IT APPLIES, with no default. `activations` is a judgement
    // about when a line counts as assembled, not a fact read off a card, and
    // section 16.7 measured it at 2.97 points of P(assembled by turn 3) between
    // one and two - more than every card in the deck except three. A silently
    // defaulted 1 would be the largest unstated assumption in the model.
    if ((requirement.loop_entry_cost >= 0 || saw_pips) && !saw_activations) {
        fail(context +
             ": a requirement with `loop_entry_cost` must also declare `activations` - how "
             "many times the ability has to be used before this counts as assembled. It has "
             "no default: it is a judgement about Magic rather than a fact about the card, "
             "it is worth ~3 points of P(assembled by turn 3) between 1 and 2, and it is "
             "printed in the honesty header (SIM_PLAN.md section 16.7).");
    }
    if (requirement.activations < 1) {
        fail(context + ": `activations` must be at least 1");
    }
    // entry_pips without a generic part still needs the check to RUN, and the
    // gate is `loop_entry_cost >= 0`. Declaring pips implies an entry cost of
    // zero generic rather than no entry cost at all.
    if (saw_pips && requirement.loop_entry_cost < 0) {
        requirement.loop_entry_cost = 0;
    }
    return requirement;
}

// A pattern names a STATE, not an outcome (section 5.4). Enforced, because a
// caveat in a document is scrolled past and a caveat in a name is not.
void reject_outcome_naming(const std::string& name) {
    static constexpr std::string_view kBanned[] = {"win", "won", "wins", "kill", "lethal"};
    std::string lowered;
    for (const char c : name) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const std::string_view word : kBanned) {
        std::size_t at = lowered.find(word);
        while (at != std::string::npos) {
            const bool left = at == 0 || !std::isalpha(static_cast<unsigned char>(lowered[at - 1]));
            const std::size_t after = at + word.size();
            const bool right = after >= lowered.size() ||
                               !std::isalpha(static_cast<unsigned char>(lowered[after]));
            if (left && right) {
                fail("pattern '" + name + "' is named for an outcome. Patterns name the STATE "
                     "they detect - 'infinite_C_thrasios', not 'win_thrasios'. This deck has "
                     "no card that reads 'you win the game'; every real kill is opponent-facing "
                     "and outside the model, so the quantity is TURNS TO ASSEMBLY "
                     "(SIM_PLAN.md section 5.4).");
            }
            at = lowered.find(word, at + 1);
        }
    }
}

}  // namespace

DeckFile load_deck(const std::filesystem::path& path, const CardDb& db) {
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        fail("cannot parse " + path.string() + ": " + std::string(error.description()));
    }

    DeckFile deck;
    const auto require_table = [&](const char* key) -> const toml::table& {
        const auto* table = root[key].as_table();
        if (table == nullptr) {
            fail(path.string() + ": missing required [" + key + "] section");
        }
        return *table;
    };

    const toml::table& deck_table = require_table("deck");
    const toml::table& table_ctx = require_table("table");
    const toml::table& ablation = require_table("ablation");

    const auto require_field = [&](const toml::table& table, const char* section,
                                   const char* key) -> const toml::node& {
        const auto* node = table.get(key);
        if (node == nullptr) {
            fail(path.string() + ": [" + section + "] is missing required field '" + key +
                 "'. No field here has a default - an unstated assumption is the failure "
                 "sections 2.8 and 9.4 exist to prevent.");
        }
        return *node;
    };

    deck.commander = require_field(deck_table, "deck", "commander").value_or<std::string>("");
    deck.name = deck_table["name"].value_or<std::string>(path.stem().string());
    deck.table.opponents =
        static_cast<int>(require_field(table_ctx, "table", "opponents").value_or<int64_t>(-1));
    deck.table.on_the_play =
        require_field(table_ctx, "table", "on_the_play").value_or<bool>(true);
    {
        const toml::node& colours = require_field(table_ctx, "table", "opponent_colors");
        static constexpr std::string_view kOrder = "WUBRG";
        if (const auto* array = colours.as_array()) {
            for (const toml::node& entry : *array) {
                const auto letter = entry.value<std::string>();
                const auto at = letter ? kOrder.find(*letter) : std::string_view::npos;
                if (!letter || at == std::string_view::npos) {
                    fail(path.string() + ": [table] opponent_colors must be WUBRG letters");
                }
                deck.table.opponent_colors |= static_cast<ColourMask>(1U << at);
            }
        }
    }
    deck.ablation_replacement =
        require_field(ablation, "ablation", "replacement").value_or<std::string>("");

    // PROVENANCE, required as a block. A deck file without a date is a claim
    // about a list that may not exist any more, and this list is a snapshot of a
    // living one that has since changed.
    //
    // `source_url` may be EMPTY and the others may not. That asymmetry is
    // deliberate: the URL for this snapshot was never recorded, and a
    // reconstructed one would be worse than none because it resolves. An empty
    // string is a stated gap; the honesty header prints it on every run.
    {
        const auto* provenance = root["provenance"].as_table();
        if (provenance == nullptr) {
            fail(path.string() +
                 ": missing [provenance]. A decklist is a snapshot of something that "
                 "changes, so it needs source, snapshot_date and cards_sha256 the same way "
                 "an authored effect needs `authored_from`. Without them a number cannot be "
                 "attributed to a list.");
        }
        deck.provenance.source =
            require_field(*provenance, "provenance", "source").value_or<std::string>("");
        deck.provenance.snapshot_date =
            require_field(*provenance, "provenance", "snapshot_date").value_or<std::string>("");
        deck.provenance.cards_sha256 =
            require_field(*provenance, "provenance", "cards_sha256").value_or<std::string>("");
        deck.provenance.snapshot_note =
            (*provenance)["snapshot_note"].value_or<std::string>("");
        // Present-but-empty is allowed; absent is not, so "we did not record it"
        // has to be written down rather than left off.
        require_field(*provenance, "provenance", "source_url");
        deck.provenance.source_url = (*provenance)["source_url"].value_or<std::string>("");
    }

    PatternSet& set = deck.patterns;
    for (const Card& card : db.cards) {
        if (card.is_creature()) {
            set.creature_slots.set(card.export_index);
        }
    }

    if (const auto* engines = root["engine"].as_array()) {
        for (const toml::node& node : *engines) {
            const auto* entry = node.as_table();
            if (entry == nullptr) fail("each [[engine]] must be a table");
            Engine engine;
            engine.name = (*entry)["name"].value_or<std::string>("");
            if (engine.name.empty()) fail("every [[engine]] needs a name");
            const std::string context = "engine '" + engine.name + "'";

            const auto* sets = (*entry)["sets"].as_array();
            if (sets == nullptr || sets->empty()) {
                fail(context + ": must set at least one flag, or it can never matter");
            }
            for (const toml::node& flag : *sets) {
                engine.sets |= FlagMask{1}
                               << flag_index(set.flag_names, flag.value_or<std::string>(""), context);
            }
            const auto* requires_table = (*entry)["requires"].as_table();
            if (requires_table == nullptr) fail(context + ": missing 'requires'");
            engine.requires_ = parse_requirement(*requires_table, db, set, context, false);
            set.engines.push_back(std::move(engine));
        }
    }

    if (const auto* wins = root["win"].as_array()) {
        for (const toml::node& node : *wins) {
            const auto* entry = node.as_table();
            if (entry == nullptr) fail("each [[win]] must be a table");
            WinPattern pattern;
            pattern.name = (*entry)["name"].value_or<std::string>("");
            if (pattern.name.empty()) fail("every [[win]] needs a name");
            reject_outcome_naming(pattern.name);
            const auto* requires_table = (*entry)["requires"].as_table();
            if (requires_table == nullptr) {
                fail("pattern '" + pattern.name + "': missing 'requires'");
            }
            pattern.requires_ =
                parse_requirement(*requires_table, db, set, "pattern '" + pattern.name + "'", true);
            set.patterns.push_back(std::move(pattern));
        }
    }

    // [policy]. Ranks are per-slot, so the lookup in the hot path is an array
    // index rather than a name comparison.
    deck.weights.rank.assign(db.cards.size(), 0);
    if (const auto* policy = root["policy"].as_table()) {
        deck.weights.land_floor =
            static_cast<int>((*policy)["land_floor"].value_or<int64_t>(3));
        deck.weights.land_ceiling =
            static_cast<int>((*policy)["land_ceiling"].value_or<int64_t>(6));
        deck.life_floor = static_cast<int>((*policy)["life_floor"].value_or<int64_t>(10));
        const auto default_rank =
            static_cast<int>((*policy)["default_rank"].value_or<int64_t>(0));
        std::fill(deck.weights.rank.begin(), deck.weights.rank.end(), default_rank);

        if (const auto* ranks = (*policy)["rank"].as_table()) {
            for (const auto& [key, value] : *ranks) {
                // A rank naming a card not in the deck is a typo that would
                // silently do nothing - the same shape as a pattern naming an
                // absent card, and refused for the same reason.
                const int slot = slot_for(db, std::string(key.str()), "[policy.rank]");
                deck.weights.rank[static_cast<std::size_t>(slot)] =
                    static_cast<int>(value.value_or<int64_t>(0));
            }
        }
    }

    if (set.patterns.empty()) {
        fail(path.string() + ": no [[win]] patterns declared. Without one the simulator has "
                             "nothing to detect and every game is censored.");
    }

    // Every flag a win consumes must be set by some engine. A win requiring a
    // flag nothing produces can never fire, and would be reported as a dead
    // line rather than as the typo it is.
    FlagMask produced = 0;
    for (const Engine& engine : set.engines) produced |= engine.sets;
    for (const WinPattern& pattern : set.patterns) {
        const FlagMask missing = pattern.requires_.flags & ~produced;
        if (missing != 0) {
            for (std::size_t i = 0; i < set.flag_names.size(); ++i) {
                if ((missing & (FlagMask{1} << i)) != 0) {
                    fail("pattern '" + pattern.name + "' requires flag '" + set.flag_names[i] +
                         "', which no engine sets. It could never fire.");
                }
            }
        }
    }
    return deck;
}

}  // namespace cs::io

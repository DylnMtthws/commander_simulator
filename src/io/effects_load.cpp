#include "io/effects_load.hpp"

#include <algorithm>
#include <map>
#include <string>

#include <toml++/toml.hpp>

namespace cs::io {
namespace {

[[noreturn]] void fail(const std::string& message) { throw DeckError(message); }

ColourMask colours_from(const toml::node& node, const std::string& context) {
    const auto* array = node.as_array();
    if (array == nullptr) {
        fail(context + ": 'colors' must be an array (an empty one means colourless)");
    }
    ColourMask mask = 0;
    for (const toml::node& entry : *array) {
        const auto letter = entry.value<std::string>();
        static constexpr std::string_view kOrder = "WUBRG";
        const auto at = letter ? kOrder.find(*letter) : std::string_view::npos;
        if (!letter || letter->size() != 1 || at == std::string_view::npos) {
            fail(context + ": unknown colour in 'colors'; expected letters from WUBRG");
        }
        mask |= static_cast<ColourMask>(1U << at);
    }
    return mask;
}

SourceCondition condition_from(const std::string& text, const std::string& context) {
    if (text == "artifact_count_gte") return SourceCondition::ArtifactCountGte;
    if (text == "legendary_creature_count_gte") return SourceCondition::LegendaryCreatureCountGte;
    if (text == "untapped_creature_gte") return SourceCondition::UntappedCreatureGte;
    if (text == "untapped_permanent_gte") return SourceCondition::UntappedPermanentGte;
    fail(context + ": unknown condition '" + text + "'. The set is closed.");
}

CardFilter filter_from(const std::string& text, const std::string& context) {
    if (text.empty() || text == "any") return CardFilter::Any;
    if (text == "land") return CardFilter::Land;
    if (text == "nonland") return CardFilter::Nonland;
    fail(context + ": unknown card filter '" + text + "'");
}

RitualZone zone_from(const std::string& text, const std::string& context) {
    if (text.empty() || text == "battlefield") return RitualZone::Battlefield;
    if (text == "hand") return RitualZone::Hand;
    fail(context + ": unknown from_zone '" + text + "'");
}

EntersTappedUnless shape_from(const std::string& text, const std::string& context) {
    if (text == "land_count") return EntersTappedUnless::LandCount;
    if (text == "pay_life") return EntersTappedUnless::PayLife;
    if (text == "opponent_count") return EntersTappedUnless::OpponentCount;
    // RULE K1: three shapes exist and the fourth promotes this flag to its own
    // kind. Refusing an unknown one is what makes that threshold observable
    // rather than a thing someone remembers.
    fail(context + ": unknown enters_tapped_unless shape '" + text +
         "'. Three exist (land_count, pay_life, opponent_count) and RULE K1 says a "
         "FOURTH promotes this flag to its own kind, with a re-audit of every "
         "MANA_SOURCE card. Adding one here is that decision.");
}

}  // namespace

EffectDb load_effects(const std::filesystem::path& path, const CardDb& db) {
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        fail("cannot parse " + path.string() + ": " + std::string(error.description()));
    }

    EffectDb effects;
    effects.by_slot.assign(db.cards.size(), CardEffects{});

    std::map<std::string, int> slots;
    for (const Card& card : db.cards) {
        slots[card.listed_name] = card.export_index;
    }

    std::map<std::string, int> categories;
    const auto* cards = root["cards"].as_table();
    if (cards == nullptr) {
        fail(path.string() + ": missing [cards] section");
    }

    for (const auto& [key, value] : *cards) {
        const std::string name{key.str()};
        const auto found = slots.find(name);
        if (found == slots.end()) {
            // effects.toml is card-level and shared, so it will outgrow any one
            // deck. Skipping is right; the exporter names these separately.
            continue;
        }
        const auto* entry = value.as_table();
        if (entry == nullptr) {
            fail(name + ": each card entry must be a table");
        }
        CardEffects& target = effects.by_slot[static_cast<std::size_t>(found->second)];
        const auto status = (*entry)["status"].value_or<std::string>("");

        if (status == "inert") {
            target.status = AuthorStatus::Inert;
            target.reason_category = (*entry)["reason_category"].value_or<std::string>("");
            if (target.reason_category.empty()) {
                fail(name + ": an inert card requires a reason_category. A count says how "
                            "much the model cannot see; the category says what.");
            }
            // The enumeration is CLOSED (section 4.4) and was documented as
            // closed for a whole phase without anything enforcing it - a check
            // that accepts every string checks nothing. One free-text outlier
            // turns the grouped table back into a list.
            if (reason_category_meaning(target.reason_category) == nullptr) {
                fail(name + ": unknown reason_category '" + target.reason_category +
                     "'. The set is closed (SIM_PLAN.md section 4.4): interaction, "
                     "opponent_trigger, opponent_permanent, timing_only, "
                     "no_object_in_model.");
            }
            if ((*entry)["reason"].value_or<std::string>("").empty()) {
                fail(name + ": an inert card requires a free-text 'reason' a human can "
                            "disagree with.");
            }
            target.disputed = (*entry)["disputed"].value_or<std::string>("");
            ++categories[target.reason_category];
            ++effects.inert;
            if (!target.disputed.empty()) {
                ++effects.disputed;
            }
            continue;
        }
        if (status != "modeled") {
            fail(name + ": status must be \"modeled\" or \"inert\", got '" + status + "'");
        }
        target.status = AuthorStatus::Modeled;
        ++effects.modeled;

        // CONVOKE is a cost property, so it is a card-level key rather than a
        // member of the closed effect set (core/effects.hpp).
        target.convoke = (*entry)["convoke"].value_or<bool>(false);

        const auto* list = (*entry)["effects"].as_array();
        if (list == nullptr || list->empty()) {
            fail(name + ": a modeled card needs at least one effect");
        }
        for (const toml::node& node : *list) {
            const auto* effect = node.as_table();
            if (effect == nullptr) {
                fail(name + ": each effect must be a table");
            }
            const auto kind = (*effect)["kind"].value_or<std::string>("");
            const std::string context = name + " (" + kind + ")";

            if (kind == "MANA_SOURCE") {
                ManaSourceEffect mana;
                if (const auto from_table = (*effect)["colors_from_table"].value<std::string>()) {
                    if (*from_table != "opponent_colors") {
                        fail(context + ": colors_from_table only supports 'opponent_colors'");
                    }
                    mana.colours_from_table = true;
                } else if (const auto* colours = (*effect)["colors"].as_array()) {
                    mana.produces = colours_from(*colours, context);
                } else {
                    fail(context + ": needs 'colors' or 'colors_from_table'");
                }
                mana.amount = static_cast<std::uint8_t>((*effect)["amount"].value_or<int64_t>(1));
                mana.is_land = (*effect)["is_land"].value_or<bool>(false);
                mana.is_creature = (*effect)["is_creature"].value_or<bool>(false);
                mana.untaps_normally = (*effect)["untaps_normally"].value_or<bool>(true);
                mana.life_cost = static_cast<int>((*effect)["life_cost"].value_or<int64_t>(0));
                if (const auto shape = (*effect)["enters_tapped_unless"].value<std::string>()) {
                    mana.enters_tapped_unless = shape_from(*shape, context);
                    mana.enters_tapped_param =
                        static_cast<int>((*effect)["enters_tapped_param"].value_or<int64_t>(0));
                }
                target.has_mana_source = true;
                target.mana_source = mana;
            } else if (kind == "DYNAMIC_MANA_SOURCE") {
                ManaSourceEffect mana;
                if (const auto* colours = (*effect)["colors"].as_array()) {
                    mana.produces = colours_from(*colours, context);
                } else {
                    fail(context + ": needs 'colors'");
                }
                mana.amount = static_cast<std::uint8_t>((*effect)["amount"].value_or<int64_t>(1));
                mana.is_land = (*effect)["is_land"].value_or<bool>(false);
                mana.is_creature = (*effect)["is_creature"].value_or<bool>(false);
                mana.life_cost = static_cast<int>((*effect)["life_cost"].value_or<int64_t>(0));
                const auto condition = (*effect)["condition"].value_or<std::string>("");
                if (condition.empty()) {
                    fail(context + ": a DYNAMIC_MANA_SOURCE without a condition is just a "
                                   "MANA_SOURCE - use that kind instead.");
                }
                mana.condition = condition_from(condition, context);
                mana.condition_param =
                    static_cast<int>((*effect)["condition_param"].value_or<int64_t>(1));
                target.has_mana_source = true;
                target.mana_source = mana;
            } else if (kind == "FETCH") {
                FetchEffect fetch;
                const auto* finds = (*effect)["finds"].as_array();
                if (finds == nullptr || finds->empty()) {
                    fail(context + ": a FETCH needs 'finds', a non-empty list of land types");
                }
                for (const toml::node& entry_type : *finds) {
                    fetch.finds.push_back(entry_type.value_or<std::string>(""));
                }
                fetch.life_cost = static_cast<int>((*effect)["life_cost"].value_or<int64_t>(0));
                target.has_fetch = true;
                target.fetch = fetch;
            } else if (kind == "CARD_COST") {
                CardCostEffect cost;
                cost.cards = static_cast<int>((*effect)["cards"].value_or<int64_t>(1));
                cost.filter =
                    filter_from((*effect)["filter"].value_or<std::string>(""), context);
                target.has_card_cost = true;
                target.card_cost = cost;
            } else if (kind == "RITUAL") {
                RitualEffect ritual;
                if (const auto* colours = (*effect)["colors"].as_array()) {
                    ritual.produces = colours_from(*colours, context);
                } else {
                    fail(context + ": needs 'colors'");
                }
                ritual.amount =
                    static_cast<std::uint8_t>((*effect)["amount"].value_or<int64_t>(1));
                ritual.from_zone =
                    zone_from((*effect)["from_zone"].value_or<std::string>(""), context);
                target.has_ritual = true;
                target.ritual = ritual;
            } else if (kind == "TUTOR") {
                TutorEffect tutor;
                const auto filter = (*effect)["filter"].value_or<std::string>("any");
                if (filter == "any") tutor.filter = TutorFilter::Any;
                else if (filter == "creature") tutor.filter = TutorFilter::Creature;
                else if (filter == "non_human_creature") tutor.filter = TutorFilter::NonHumanCreature;
                else if (filter == "artifact") tutor.filter = TutorFilter::Artifact;
                else if (filter == "land") tutor.filter = TutorFilter::Land;
                else fail(context + ": unknown tutor filter '" + filter + "'");

                const auto destination = (*effect)["destination"].value_or<std::string>("");
                if (destination == "battlefield") tutor.destination = TutorDestination::Battlefield;
                else if (destination == "hand") tutor.destination = TutorDestination::Hand;
                else fail(context + ": a TUTOR must state its destination explicitly - "
                                    "'battlefield' or 'hand'. They are different cards.");

                if ((*effect)["max_mana_value"]) {
                    fail(context + ": `max_mana_value` no longer exists. Every constant-cap "
                                   "tutor in this deck searches for an EXACT mana value - "
                                   "Trophy Mage says \"mana value 3\", transmute says \"the "
                                   "same mana value as this card\" - so the key is "
                                   "`mana_value_exactly`. \"X or less\" is max_from_x.");
                }
                tutor.mana_value_exactly =
                    static_cast<int>((*effect)["mana_value_exactly"].value_or<int64_t>(-1));
                tutor.max_from_x = (*effect)["max_from_x"].value_or<bool>(false);
                target.has_tutor = true;
                target.tutor = tutor;
            } else if (kind == "CLONE") {
                CloneEffect clone;
                const auto filter = (*effect)["filter"].value_or<std::string>("nonland_permanent");
                if (filter == "nonland_permanent") clone.filter = CloneFilter::NonlandPermanent;
                else if (filter == "artifact") clone.filter = CloneFilter::Artifact;
                else if (filter == "enchantment") clone.filter = CloneFilter::Enchantment;
                else if (filter == "creature") clone.filter = CloneFilter::Creature;
                else if (filter == "artifact_or_enchantment")
                    clone.filter = CloneFilter::ArtifactOrEnchantment;
                else fail(context + ": unknown clone filter '" + filter + "'");
                clone.max_from_x = (*effect)["max_from_x"].value_or<bool>(false);
                target.has_clone = true;
                target.clone = clone;
            } else if (kind == "DRAW") {
                DrawEffect draw;
                draw.cards = static_cast<int>((*effect)["cards"].value_or<int64_t>(1));
                if (draw.cards <= 0) {
                    fail(context + ": a DRAW of zero or fewer cards is not a DRAW. If the "
                                   "card genuinely draws nothing here, it is inert with a "
                                   "reason.");
                }
                target.has_draw = true;
                target.draw = draw;
            } else if (kind == "MASS_UNTAP") {
                MassUntapEffect untap;
                untap.nonland_only = (*effect)["nonland_only"].value_or<bool>(true);
                target.has_mass_untap = true;
                target.mass_untap = untap;
            } else if (kind == "STATIC_MANA_MODIFIER") {
                ModifierEffect modifier;
                const auto mode = (*effect)["mode"].value_or<std::string>("");
                if (mode == "multiply") {
                    modifier.mode = ModifierMode::Multiply;
                    modifier.bonus =
                        static_cast<std::uint8_t>((*effect)["bonus"].value_or<int64_t>(1));
                    modifier.nonland_only = (*effect)["nonland_only"].value_or<bool>(true);
                } else if (mode == "grant_creature_mana") {
                    modifier.mode = ModifierMode::GrantCreatureMana;
                    if (const auto* colours = (*effect)["colors"].as_array()) {
                        modifier.grants = colours_from(*colours, context);
                    }
                    modifier.grant_amount =
                        static_cast<std::uint8_t>((*effect)["amount"].value_or<int64_t>(1));
                } else {
                    fail(context + ": unknown mode '" + mode + "'");
                }
                target.has_modifier = true;
                target.modifier = modifier;
            } else {
                // The closed set again. An ignored effect makes a card silently
                // free, which reads downstream as a faster deck.
                fail(context + ": unknown effect kind '" + kind +
                     "'. The set is closed (SIM_PLAN.md section 4.2) and adding one is a "
                     "decision that re-audits every already-authored card.");
            }
        }
    }

    for (const CardEffects& entry : effects.by_slot) {
        effects.unauthored += entry.status == AuthorStatus::Unauthored ? 1 : 0;
    }
    for (const auto& [category, count] : categories) {
        effects.inert_categories.push_back(category);
        effects.inert_counts.push_back(count);
    }
    return effects;
}

}  // namespace cs::io

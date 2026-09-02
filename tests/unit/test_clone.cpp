// Clones. Two properties worth confirming rather than assuming.

#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/effects.hpp"
#include "core/policy.hpp"
#include "core/sim.hpp"
#include "io/card_db_load.hpp"

namespace {

const cs::CardDb& db() {
    static const cs::CardDb loaded =
        cs::io::load_card_db(std::filesystem::path(CS_FIXTURE_DIR) / "cards.fixture.json");
    return loaded;
}

int slot_of(const char* listed) {
    for (const cs::Card& card : db().cards) {
        if (card.listed_name == listed) return card.export_index;
    }
    FAIL("fixture is missing " << listed);
    return -1;
}

// Sol Ring as a real source, Kinnan as a multiplier, Clever Impersonator as a
// clone. Built by hand so the test does not depend on the authored file.
cs::EffectDb effects_with_clone() {
    cs::EffectDb effects;
    effects.by_slot.assign(db().cards.size(), cs::CardEffects{});

    cs::CardEffects& ring = effects.by_slot[static_cast<std::size_t>(slot_of("Sol Ring"))];
    ring.status = cs::AuthorStatus::Modeled;
    ring.has_mana_source = true;
    ring.mana_source.produces = 0;
    ring.mana_source.amount = 2;

    cs::CardEffects& kinnan =
        effects.by_slot[static_cast<std::size_t>(slot_of("Kinnan, Bonder Prodigy"))];
    kinnan.status = cs::AuthorStatus::Modeled;
    kinnan.has_modifier = true;
    kinnan.modifier.mode = cs::ModifierMode::Multiply;
    kinnan.modifier.bonus = 1;
    kinnan.modifier.nonland_only = true;

    cs::CardEffects& mirror =
        effects.by_slot[static_cast<std::size_t>(slot_of("Finale of Devastation"))];
    mirror.status = cs::AuthorStatus::Modeled;
    mirror.has_clone = true;
    mirror.clone.filter = cs::CloneFilter::NonlandPermanent;
    return effects;
}

}  // namespace

TEST_CASE("a clone of a mana source is multiplied by Kinnan independently", "[clone][kinnan]") {
    // The interesting case, and confirmed rather than assumed. A copy is a
    // SEPARATE nonland permanent, so Kinnan sees it in its own right: two
    // Sol Rings under Kinnan are 3 + 3, not 2 + 2 + 1.
    const cs::EffectDb effects = effects_with_clone();
    const cs::TableContext table;
    const int ring = slot_of("Sol Ring");
    const int clone = slot_of("Finale of Devastation");

    cs::GameState state;
    state.battlefield.set(ring);
    std::vector<cs::Source> sources;

    SECTION("one Sol Ring, no Kinnan") {
        cs::collect_sources(db(), effects, state, table, sources);
        REQUIRE(cs::total_mana(sources) == 2);
    }
    SECTION("one Sol Ring under Kinnan") {
        state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));
        cs::collect_sources(db(), effects, state, table, sources);
        REQUIRE(cs::total_mana(sources) == 3);
    }
    SECTION("a CLONE of Sol Ring under Kinnan is multiplied too") {
        state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));
        state.battlefield.set(clone);
        state.copy_of[static_cast<std::size_t>(clone)] = static_cast<std::int8_t>(ring);
        cs::collect_sources(db(), effects, state, table, sources);
        REQUIRE(cs::total_mana(sources) == 6);  // 3 + 3, not 2 + 2 + 1
    }
}

TEST_CASE("a clone counts as the card it copied for patterns", "[clone][pattern]") {
    // A Copy Artifact on Basalt Monolith IS a Basalt Monolith. Without this,
    // every engine naming a specific card would ignore the deck's eight clones.
    const int ring = slot_of("Sol Ring");
    const int clone = slot_of("Finale of Devastation");

    cs::PatternSet set;
    cs::WinPattern pattern;
    pattern.name = "needs_sol_ring";
    pattern.requires_.in_play.set(ring);
    set.patterns.push_back(pattern);

    cs::GameState state;
    state.battlefield.set(clone);
    const std::vector<cs::Source> none;

    SECTION("an uncopied clone does not satisfy it") {
        REQUIRE(cs::first_satisfied(set, state, none) < 0);
    }
    SECTION("a clone copying Sol Ring does") {
        state.copy_of[static_cast<std::size_t>(clone)] = static_cast<std::int8_t>(ring);
        REQUIRE(cs::first_satisfied(set, state, none) == 0);
    }
}

TEST_CASE("a clone with nothing to copy is dead, not mediocre", "[clone][policy]") {
    // On an empty board a clone does nothing at all. Scoring it zero would
    // leave it mid-table and the policy would cast it for nothing; it has to
    // read as uncastable.
    const cs::EffectDb effects = effects_with_clone();
    cs::PolicyWeights weights;
    weights.rank.assign(db().cards.size(), 10);
    weights.rank[static_cast<std::size_t>(slot_of("Finale of Devastation"))] = 90;
    const cs::AuthoredPolicy policy(weights);

    const cs::PatternSet patterns;
    const std::vector<cs::Source> rich(8, cs::Source{.produces = 0x1F, .amount = 1});
    cs::GameStats stats;

    SECTION("empty board: the clone is uncastable despite the highest rank") {
        cs::GameState state;
        state.hand.set(slot_of("Finale of Devastation"));
        state.hand.set(slot_of("Sol Ring"));
        const cs::Context context{db(), patterns, state, rich, nullptr, &effects};
        REQUIRE(policy.choose_spell(context, stats) == slot_of("Sol Ring"));
    }
    SECTION("with something to copy, rank decides again") {
        cs::GameState state;
        state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));
        state.hand.set(slot_of("Finale of Devastation"));
        state.hand.set(slot_of("Sol Ring"));
        const cs::Context context{db(), patterns, state, rich, nullptr, &effects};
        REQUIRE(policy.choose_spell(context, stats) == slot_of("Finale of Devastation"));
    }
}

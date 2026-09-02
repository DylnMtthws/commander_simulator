// Every source knows where it came from.
//
// This is the invariant that makes paying correct, and it is here because it
// was violated for a whole phase without any counter noticing. Paying used to
// walk the battlefield and RE-DERIVE what a source was, and re-derived it
// differently from collect_sources: it required has_mana_source, so a creature
// that is a source only because Enduring Vitality grants it one was never
// tapped; it looked a clone up by its own slot rather than the card it copies,
// so a clone was never tapped either; and it never looked in hand at all, so
// Elvish Spirit Guide was never exiled. All three were free, unlimited mana.
//
// The symptom was a trace reading "mana: 2x{G} 1x{UG}" and then paying 2, then
// 4, then 1 out of it. No test failed, because every test asserted on what
// collect_sources produced and none on what spending it consumed.
//
// The structural fix is that one function decides what a source is and the turn
// loop spends what that function returned. What this file pins is the piece
// that makes that possible: a Source is spendable, because it carries its slot.

#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/effects.hpp"
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

cs::EffectDb blank() {
    cs::EffectDb effects;
    effects.by_slot.assign(db().cards.size(), cs::CardEffects{});
    return effects;
}

const cs::Source* source_from(const std::vector<cs::Source>& sources, int slot) {
    for (const cs::Source& source : sources) {
        if (source.slot == slot) return &source;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a Source with no slot cannot be produced by forgetting the field", "[payment]") {
    // PLAN.md 11.0, eleventh rule: a "no value" state expressible as a valid
    // value gets read as one. Slot 0 is a real card, so the empty marker has to
    // be outside the domain - and this asserts that value-initialisation and a
    // designated-initialiser caller that omits the field both land on it.
    const cs::Source blank_source{};
    REQUIRE(blank_source.slot == -1);
    const cs::Source partial{.produces = 0x10, .amount = 2};
    REQUIRE(partial.slot == -1);
}

TEST_CASE("every collected source carries the slot that pays it", "[payment]") {
    cs::EffectDb effects = blank();
    const cs::TableContext table;

    const int ring = slot_of("Sol Ring");
    const int kinnan = slot_of("Kinnan, Bonder Prodigy");
    const int clone = slot_of("Finale of Devastation");
    const int guide = slot_of("Mental Misstep");   // stands in for Elvish Spirit Guide
    const int vitality = slot_of("Invasion of Ikoria");  // stands in for Enduring Vitality

    cs::CardEffects& ring_effect = effects.by_slot[static_cast<std::size_t>(ring)];
    ring_effect.status = cs::AuthorStatus::Modeled;
    ring_effect.has_mana_source = true;
    ring_effect.mana_source.amount = 2;

    cs::CardEffects& guide_effect = effects.by_slot[static_cast<std::size_t>(guide)];
    guide_effect.status = cs::AuthorStatus::Modeled;
    guide_effect.has_ritual = true;
    guide_effect.ritual.produces = 0x10;
    guide_effect.ritual.from_zone = cs::RitualZone::Hand;

    cs::CardEffects& grant = effects.by_slot[static_cast<std::size_t>(vitality)];
    grant.status = cs::AuthorStatus::Modeled;
    grant.has_modifier = true;
    grant.modifier.mode = cs::ModifierMode::GrantCreatureMana;
    grant.modifier.grants = 0x1F;
    grant.modifier.grant_amount = 1;

    cs::GameState state;
    state.battlefield.set(ring);
    state.battlefield.set(vitality);
    state.battlefield.set(kinnan);
    state.battlefield.set(clone);
    state.copy_of[static_cast<std::size_t>(clone)] = static_cast<std::int8_t>(ring);
    state.hand.set(guide);

    std::vector<cs::Source> sources;
    cs::collect_sources(db(), effects, state, table, sources);

    SECTION("an ordinary permanent") {
        REQUIRE(source_from(sources, ring) != nullptr);
    }
    SECTION("a CLONE reports its OWN slot, not the card it copies") {
        // Paying taps the permanent that is there, and a clone is its own
        // permanent. Reporting the copied slot would tap the original twice and
        // never tap the copy - which is the shape of the bug this file exists
        // for, one level down.
        const cs::Source* copy = source_from(sources, clone);
        REQUIRE(copy != nullptr);
        REQUIRE(copy->amount == 2);
    }
    SECTION("a creature that is a source only by GRANT") {
        // has_mana_source is false for this one. The old payment code required
        // it, which is precisely why Enduring Vitality's mana was free.
        const cs::Source* granted = source_from(sources, kinnan);
        REQUIRE(granted != nullptr);
        REQUIRE(granted->is_creature);
    }
    SECTION("a ritual in HAND") {
        // Not on the battlefield at all, so a payment routine that iterates the
        // battlefield can never consume it.
        REQUIRE(source_from(sources, guide) != nullptr);
    }
    SECTION("no source is left unspendable") {
        for (const cs::Source& source : sources) {
            REQUIRE(source.slot >= 0);
        }
    }
}

TEST_CASE("a creature tutor cannot find a Battle", "[tutor]") {
    // Invasion of Ikoria is a Battle whose BACK face is Zilortha, a Legendary
    // Creature, so card.all_types contains "Creature". A search of the library
    // sees only the front face - you cannot Chord of Calling for a Battle - and
    // reading all_types offered it to every creature tutor in the deck at rank
    // 58, above three of the mana dorks.
    cs::GameState state;
    state.library_count = static_cast<std::uint8_t>(db().cards.size());
    state.drawn = 0;
    for (std::size_t i = 0; i < db().cards.size(); ++i) {
        state.library[i] = static_cast<std::uint8_t>(i);
    }

    cs::TutorEffect tutor;
    tutor.filter = cs::TutorFilter::Creature;
    std::vector<int> found;
    cs::tutor_candidates(tutor, db(), state, 99, found);

    for (const int slot : found) {
        REQUIRE(slot != slot_of("Invasion of Ikoria"));
    }
    // and it still finds an actual creature, so the predicate did not just
    // stop working.
    bool has_kinnan = false;
    for (const int slot : found) {
        has_kinnan = has_kinnan || slot == slot_of("Kinnan, Bonder Prodigy");
    }
    REQUIRE(has_kinnan);
}

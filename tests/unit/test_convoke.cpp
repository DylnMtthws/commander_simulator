// Convoke and the two things it must not do.
//
// The whole modelling claim (data/effects.toml, Chord of Calling) is that
// offering only creatures with NO mana ability of their own is exact rather
// than an approximation. That claim is two properties, and both are asserted
// here rather than reasoned about in a comment.

#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/ablation.hpp"
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

cs::EffectDb blank() {
    cs::EffectDb effects;
    effects.by_slot.assign(db().cards.size(), cs::CardEffects{});
    return effects;
}

}  // namespace

TEST_CASE("convoke offers a creature with no mana ability", "[convoke]") {
    // Kinnan is the honest case: a creature that taps for nothing, so convoking
    // it is the only way it ever pays for anything.
    const cs::EffectDb effects = blank();
    cs::GameState state;
    state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));

    std::vector<int> slots;
    cs::convoke_slots(db(), effects, state, slots);
    REQUIRE(slots.size() == 1);
    REQUIRE(slots[0] == slot_of("Kinnan, Bonder Prodigy"));

    SECTION("a tapped creature is not offered") {
        state.tapped.set(slot_of("Kinnan, Bonder Prodigy"));
        cs::convoke_slots(db(), effects, state, slots);
        REQUIRE(slots.empty());
    }
    SECTION("a noncreature permanent is never offered") {
        state.battlefield.set(slot_of("Sol Ring"));
        cs::convoke_slots(db(), effects, state, slots);
        REQUIRE(slots.size() == 1);  // still only Kinnan
    }
}

TEST_CASE("convoke never offers a creature that taps for mana", "[convoke][kinnan]") {
    // THE claim that makes the omission exact rather than an approximation: a
    // mana creature under Kinnan produces two and convoking it produces one, so
    // tapping it for mana weakly dominates. If this test fails, the comment in
    // data/effects.toml is a lie and convoke has become a policy decision.
    cs::EffectDb effects = blank();
    cs::CardEffects& kinnan =
        effects.by_slot[static_cast<std::size_t>(slot_of("Kinnan, Bonder Prodigy"))];
    kinnan.status = cs::AuthorStatus::Modeled;
    kinnan.has_mana_source = true;
    kinnan.mana_source.produces = 0x10;  // {G}
    kinnan.mana_source.amount = 1;

    cs::GameState state;
    state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));

    std::vector<int> slots;
    cs::convoke_slots(db(), effects, state, slots);
    REQUIRE(slots.empty());
}

TEST_CASE("convoke mana is not multiplied", "[convoke][kinnan]") {
    // "Whenever you tap a nonland permanent FOR MANA." Convoke taps no mana
    // ability - it pays a cost - so a multiplier must not see it. Getting this
    // wrong would have made Chord of Calling the best card in the deck.
    cs::EffectDb effects = blank();
    cs::CardEffects& kinnan =
        effects.by_slot[static_cast<std::size_t>(slot_of("Kinnan, Bonder Prodigy"))];
    kinnan.status = cs::AuthorStatus::Modeled;
    kinnan.has_modifier = true;
    kinnan.modifier.mode = cs::ModifierMode::Multiply;
    kinnan.modifier.bonus = 1;

    cs::GameState state;
    state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));

    std::vector<cs::Source> sources;
    cs::convoke_sources(db(), effects, state, sources);
    REQUIRE(sources.size() == 1);
    REQUIRE(cs::total_mana(sources) == 1);  // one, not two
}

TEST_CASE("convoke pays only for the card that has it", "[convoke][policy]") {
    // Convoke sources are per-card and must never reach collect_sources. If
    // they did, every spell in hand could spend a creature only Chord can tap.
    cs::EffectDb effects = blank();
    const int chord = slot_of("Finale of Devastation");  // {X}{G}{G}, stands in
    const int other = slot_of("Invasion of Ikoria");     // {X}{G}{G}, no convoke
    effects.by_slot[static_cast<std::size_t>(chord)].status = cs::AuthorStatus::Modeled;
    effects.by_slot[static_cast<std::size_t>(chord)].convoke = true;
    effects.by_slot[static_cast<std::size_t>(other)].status = cs::AuthorStatus::Modeled;

    cs::PolicyWeights weights;
    weights.rank.assign(db().cards.size(), 10);
    const cs::AuthoredPolicy policy(weights);
    const cs::PatternSet patterns;
    cs::GameStats stats;

    cs::GameState state;
    state.battlefield.set(slot_of("Kinnan, Bonder Prodigy"));  // one convokable body
    state.hand.set(chord);
    state.hand.set(other);

    // One green source. {X}{G}{G} at X=0 needs two, so neither is castable on
    // mana alone; convoke supplies the second for one of them and not the other.
    const std::vector<cs::Source> one_green{cs::Source{.produces = 0x10, .amount = 1}};
    const cs::Context context{db(), patterns, state, one_green, nullptr, &effects};

    REQUIRE(policy.score(context, chord, /*as_land=*/false, stats).castable);
    REQUIRE_FALSE(policy.score(context, other, /*as_land=*/false, stats).castable);
}

TEST_CASE("an instant does not stay on the battlefield", "[permanent]") {
    // The rule belongs to the card TYPE. It used to be a clause inside the
    // TUTOR block, which was right for the nine tutors and left Dramatic
    // Reversal sitting on the battlefield as a blank permanent - counted by
    // Gene Pollinator and offered to every clone as a legal target.
    REQUIRE(cs::is_permanent(db().cards[static_cast<std::size_t>(slot_of("Sol Ring"))]));
    REQUIRE(cs::is_permanent(db().cards[static_cast<std::size_t>(slot_of("Forest"))]));
    REQUIRE(cs::is_permanent(
        db().cards[static_cast<std::size_t>(slot_of("Kinnan, Bonder Prodigy"))]));
    REQUIRE_FALSE(
        cs::is_permanent(db().cards[static_cast<std::size_t>(slot_of("Mental Misstep"))]));
    // Battle is deliberately not a permanent type here (section 4.6): a Siege
    // flips by being attacked and there is no combat, so Invasion of Ikoria's
    // ETB is authored and the permanent is discarded.
    REQUIRE_FALSE(
        cs::is_permanent(db().cards[static_cast<std::size_t>(slot_of("Invasion of Ikoria"))]));
}

TEST_CASE("ablating a card named by a pattern makes the pattern impossible", "[ablation]") {
    // The bug that inverted the largest result in the sweep. Requirements
    // compile to SLOT MASKS at load, so swapping a slot silently rebinds them:
    // with the pattern's card replaced by a Forest, `in_play = [that slot]`
    // asked for a Forest, which the deck plays almost every game. The engine
    // then fired MORE often without its key card, and Enduring Vitality - an
    // engine piece - measured as costing the deck 7.6 points. Fixed, it is
    // worth +7.3.
    //
    // Note the direction that would be wrong in the tempting way: REMOVING the
    // slot from the mask makes the requirement easier, not impossible.
    const int ring = slot_of("Sol Ring");
    const int forest = slot_of("Forest");

    cs::PatternSet set;
    cs::WinPattern pattern;
    pattern.name = "needs_sol_ring";
    pattern.requires_.in_play.set(ring);
    set.patterns.push_back(pattern);

    const cs::EffectDb effects = blank();
    cs::PolicyWeights weights;
    weights.rank.assign(db().cards.size(), 10);

    cs::GameState state;
    state.battlefield.set(ring);
    const std::vector<cs::Source> none;
    REQUIRE(cs::first_satisfied(set, state, none) == 0);

    const cs::AblatedDeck arm = cs::ablate(db(), effects, weights, set, ring, forest);
    SECTION("the slot now holds the replacement") {
        REQUIRE(arm.db.cards[static_cast<std::size_t>(ring)].listed_name == "Forest");
        REQUIRE(arm.db.cards[static_cast<std::size_t>(ring)].export_index == ring);
    }
    SECTION("and the pattern can no longer be satisfied, by anything") {
        REQUIRE(arm.patterns.patterns[0].requires_.impossible);
        REQUIRE(cs::first_satisfied(arm.patterns, state, none) < 0);
        // Not even with the whole board in play, which is what a
        // removed-from-the-mask requirement would happily accept.
        cs::GameState everything;
        for (std::size_t i = 0; i < db().cards.size(); ++i) {
            everything.battlefield.set(static_cast<int>(i));
        }
        REQUIRE(cs::first_satisfied(arm.patterns, everything, none) < 0);
    }
}

TEST_CASE("ablation refuses the two decks it cannot make", "[ablation]") {
    const cs::EffectDb effects = blank();
    cs::PolicyWeights weights;
    weights.rank.assign(db().cards.size(), 10);
    const cs::PatternSet set;
    const int forest = slot_of("Forest");

    // Section 9.4: the two arms would be the same deck, and zero would read as
    // a measurement rather than as a malformed question.
    REQUIRE_THROWS_AS(cs::ablate(db(), effects, weights, set, forest, forest),
                      cs::AblationError);

    // The commander starts in the command zone and the policy only plays lands
    // from HAND, so a replacement there is stuck - a 99-card deck, which is the
    // conflation the replacement design exists to prevent.
    cs::CardDb with_commander = db();
    with_commander.cards[static_cast<std::size_t>(slot_of("Kinnan, Bonder Prodigy"))]
        .is_commander = true;
    REQUIRE_THROWS_AS(cs::ablate(with_commander, effects, weights, set,
                                 slot_of("Kinnan, Bonder Prodigy"), forest),
                      cs::AblationError);
}

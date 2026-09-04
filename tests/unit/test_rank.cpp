#include <catch2/catch_test_macros.hpp>

#include "core/rank.hpp"

TEST_CASE("derived ranks follow roles without card or deck names", "[rank]") {
    cs::Card card;
    card.export_index = 0;
    card.mana_value = 2;
    card.faces.push_back({.name = "fixture", .type_line = "Artifact", .is_land = false,
                          .cost = cs::Cost{}, .mana_value = 2});
    cs::CardEffects effects;
    effects.status = cs::AuthorStatus::Modeled;

    cs::PatternSet patterns;
    REQUIRE(cs::derive_card_rank(card, effects, patterns) == 10);

    effects.has_mana_source = true;
    effects.mana_source.amount = 2;
    REQUIRE(cs::derive_card_rank(card, effects, patterns) == 70);

    cs::Engine engine;
    engine.requires_.in_play.set(0);
    patterns.engines.push_back(engine);
    REQUIRE(cs::derive_card_rank(card, effects, patterns) == 90);

    card.is_commander = true;
    REQUIRE(cs::derive_card_rank(card, effects, patterns) == 100);
}

TEST_CASE("explicit rank overrides are applied after derivation", "[rank]") {
    cs::Card card;
    card.export_index = 0;
    card.faces.push_back({.name = "fixture", .type_line = "Sorcery", .is_land = false,
                          .cost = cs::Cost{}, .mana_value = 0});
    cs::CardDb db;
    db.cards.push_back(card);
    cs::EffectDb effects;
    effects.by_slot.resize(1);
    cs::PolicyWeights weights;
    const cs::RankOverride override{.slot = 0, .rank = 73};
    cs::derive_policy_ranks(db, effects, cs::PatternSet{}, std::span{&override, 1}, weights);
    REQUIRE(weights.rank == std::vector<int>{73});
}

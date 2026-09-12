#include <catch2/catch_test_macros.hpp>
#include "core/resource_probe.hpp"

TEST_CASE("resource probe respects color payment and finite lands") {
    std::vector<cs::ProbeCard> cards(99, {true, 16, false});
    cs::ProbeConfig config;
    config.games=100;
    config.turns=6;
    config.commander_cost.generic=4;
    config.commander_cost.pips[1]=1;
    config.commander_cost.pips[4]=1;
    auto green=cs::resource_probe(cards,config);
    REQUIRE(green.commander_cast==0);
    REQUIRE(green.lands_mean==6);
    for (auto& card: cards) card.colors=18;
    auto dual=cs::resource_probe(cards,config);
    REQUIRE(dual.commander_cast==100);
    config.extra_land_plays=1;
    config.landfall_draw=1;
    auto engine=cs::resource_probe(cards,config);
    REQUIRE(engine.lands_mean==7);
    REQUIRE(engine.commander_draws_mean==1);
}
TEST_CASE("resource probe tapped lands and deterministic samples") {
    std::vector<cs::ProbeCard> cards(99,{true,16,true});
    cs::ProbeConfig config;
    config.games=100;
    config.turns=1;
    config.commander_cost.pips[4]=1;
    REQUIRE(cs::resource_probe(cards,config).commander_cast==0);
    config.turns=2;
    REQUIRE(cs::resource_probe(cards,config).commander_cast==100);
    for (std::size_t i=30;i<cards.size();++i) cards[i]={false,0,false};
    auto a=cs::resource_probe(cards,config);
    auto b=cs::resource_probe(cards,config);
    REQUIRE(a.cast_samples==b.cast_samples);
    cards.pop_back();
    REQUIRE_THROWS(cs::resource_probe(cards,config));
}

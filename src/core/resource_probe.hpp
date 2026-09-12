#pragma once
#include <cstdint>
#include <vector>
#include "core/mana.hpp"

namespace cs {
// Controlled resource scenario: opening seven, draw each turn, no mulligan,
// one land per turn (+ modeled commander allowance), no noncommander spells.
// Not a game/win model. Only unconditional one-mana lands are admitted.
struct ProbeCard {
    bool land = false;
    ColourMask colors = 0;
    bool tapped = false;
};
struct ProbeConfig {
    Cost commander_cost;
    int extra_land_plays = 0;
    int landfall_draw = 0;
    int turns = 6;
    int games = 2000;
    std::uint64_t seed = 12345;
};
struct ProbeResult {
    int games = 0;
    int commander_cast = 0;
    double lands_mean = 0;
    double commander_draws_mean = 0;
    std::vector<unsigned char> cast_samples;
};
[[nodiscard]] ProbeResult resource_probe(const std::vector<ProbeCard>& cards,
                                         const ProbeConfig& config);
}

#include "core/resource_probe.hpp"
#include "core/rng.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace cs {
ProbeResult resource_probe(const std::vector<ProbeCard>& cards, const ProbeConfig& config) {
    if (cards.size() != 99 || config.games < 1 || config.games > 20000 ||
        config.turns < 1 || config.turns > 12 || config.extra_land_plays < 0 ||
        config.extra_land_plays > 2 || config.landfall_draw < 0 || config.landfall_draw > 2) {
        throw std::invalid_argument("invalid resource probe bounds");
    }
    ProbeResult out;
    out.games = config.games;
    out.cast_samples.reserve(static_cast<std::size_t>(config.games));
    for (int game = 0; game < config.games; ++game) {
        Rng rng(seed_for_game(config.seed, static_cast<std::uint64_t>(game)));
        std::array<int, 99> order{};
        for (int i = 0; i < 99; ++i) order[static_cast<std::size_t>(i)] = i;
        for (std::size_t i = 98; i > 0; --i) {
            std::swap(order[i], order[static_cast<std::size_t>(rng.below(i+1))]);
        }
        std::vector<int> hand;
        std::vector<Source> board;
        int next = 0;
        int draws = 0;
        bool commander = false;
        auto draw = [&]() {
            if (next < 99) hand.push_back(order[static_cast<std::size_t>(next++)]);
        };
        for (int i = 0; i < 7; ++i) draw();
        for (int turn = 1; turn <= config.turns; ++turn) {
            draw(); // Multiplayer Commander draws on turn one.
            std::vector<Source> available = board;
            int played = 0;
            bool changed = true;
            while (changed) {
                changed = false;
                // Try commander before the next land, to obtain extra play
                // allowance and landfall draws when existing mana suffices.
                if (!commander) {
                    auto payment = plan_payment(config.commander_cost, available);
                    if (payment.payable) {
                        commander = true;
                        std::vector<Source> remain;
                        for (std::size_t i=0; i<available.size(); ++i)
                            if (!payment.spends(i)) remain.push_back(available[i]);
                        available = std::move(remain);
                        changed = true;
                    }
                }
                const int allowed = 1 + (commander ? config.extra_land_plays : 0);
                if (played >= allowed) continue;
                auto chosen = hand.end();
                int best = -1;
                for (auto it=hand.begin(); it!=hand.end(); ++it) {
                    const auto& card = cards[static_cast<std::size_t>(*it)];
                    if (!card.land) continue;
                    // Prefer currently missing colored pips, then untapped.
                    int score = card.tapped ? 0 : 2;
                    for (std::size_t c=0; c<kColourCount; ++c) {
                        int sources = 0;
                        for (auto s: board) if ((s.produces & (1U<<c)) != 0) ++sources;
                        if ((card.colors & (1U<<c)) != 0 && sources < config.commander_cost.pips[c]) score += 4;
                    }
                    if (score > best) { best=score; chosen=it; }
                }
                if (chosen == hand.end()) continue;
                const auto card = cards[static_cast<std::size_t>(*chosen)];
                hand.erase(chosen);
                Source source; source.produces=card.colors; source.amount=1; source.is_land=true;
                board.push_back(source);
                if (!card.tapped) available.push_back(source);
                ++played;
                if (commander) for (int n=0; n<config.landfall_draw; ++n) { draw(); ++draws; }
                changed = true;
            }
        }
        if (commander) ++out.commander_cast;
        out.cast_samples.push_back(commander ? 1 : 0);
        out.lands_mean += static_cast<double>(board.size());
        out.commander_draws_mean += draws;
    }
    out.lands_mean /= config.games;
    out.commander_draws_mean /= config.games;
    return out;
}
}

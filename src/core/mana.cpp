#include "core/mana.hpp"

#include <algorithm>
#include <array>

namespace cs {
namespace {

// Sizing. A Commander deck is 100 cards, so a battlefield cannot present more
// mana sources than that; 128 leaves room without making the search state big
// enough to fall out of L1. Exceeding either is treated as "cannot pay" rather
// than as undefined behaviour, and both are far above anything reachable.
constexpr std::size_t kMaxSources = 128;
constexpr std::size_t kMaxPips = 32;

// Which source, if any, each pip was assigned to during the search.
struct SourceState {
    int colour = -1;  // -1 == not yet assigned to any colour
    int used = 0;
};

// Depth-first assignment of coloured pips to sources.
//
// Each source may serve exactly ONE colour (see the header), so a source that
// has already been committed to green cannot also supply a blue pip - it can
// only supply further green ones, up to its amount. That single restriction is
// what makes this a search rather than a subtraction.
bool place(std::size_t index,
           std::span<const int> pips,
           std::span<const Source> sources,
           std::array<SourceState, kMaxSources>& state) noexcept {
    if (index == pips.size()) {
        return true;
    }
    const int colour = pips[index];
    const auto bit = static_cast<ColourMask>(1U << static_cast<unsigned>(colour));

    for (std::size_t s = 0; s < sources.size(); ++s) {
        if ((sources[s].produces & bit) == 0) {
            continue;
        }
        SourceState& current = state[s];
        if (current.colour == -1) {
            current.colour = colour;
            current.used = 1;
            if (place(index + 1, pips, sources, state)) {
                return true;
            }
            current.colour = -1;
            current.used = 0;
        } else if (current.colour == colour && current.used < sources[s].amount) {
            ++current.used;
            if (place(index + 1, pips, sources, state)) {
                return true;
            }
            --current.used;
        }
    }
    return false;
}

// Can the coloured requirements be met at all, ignoring generic?
bool colours_assignable(const std::array<int, kColourCount>& need,
                        std::span<const Source> sources) noexcept {
    // Order colours MOST CONSTRAINED FIRST: the colour with the fewest capable
    // sources is placed first, because committing a flexible source to it early
    // is the mistake that forces backtracking. With this ordering the search is
    // effectively linear on real board states; without it, a hand holding one
    // dual and one basic makes it thrash.
    std::array<std::size_t, kColourCount> capable{};
    for (std::size_t c = 0; c < kColourCount; ++c) {
        const auto bit = static_cast<ColourMask>(1U << c);
        for (const Source& source : sources) {
            capable[c] += (source.produces & bit) != 0 ? 1 : 0;
        }
    }

    std::array<std::size_t, kColourCount> order{};
    for (std::size_t i = 0; i < kColourCount; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (capable[a] != capable[b]) {
            return capable[a] < capable[b];
        }
        return a < b;  // total order: ties never depend on sort stability
    });

    std::array<int, kMaxPips> pips{};
    std::size_t count = 0;
    for (const std::size_t colour : order) {
        for (int i = 0; i < need[colour]; ++i) {
            if (count >= kMaxPips) {
                return false;
            }
            pips[count++] = static_cast<int>(colour);
        }
    }
    if (count == 0) {
        return true;
    }

    std::array<SourceState, kMaxSources> state{};
    return place(0, std::span<const int>(pips.data(), count), sources, state);
}

}  // namespace

Source with_multiplier(Source source, const ManaMultiplier& multiplier) noexcept {
    if (multiplier.nonland_only && source.is_land) {
        return source;  // Kinnan does not see lands. See test_mana.cpp.
    }
    source.amount = static_cast<std::uint8_t>(source.amount + multiplier.bonus);
    return source;
}

int total_mana(std::span<const Source> sources) noexcept {
    int total = 0;
    for (const Source& source : sources) {
        total += source.amount;
    }
    return total;
}

bool can_pay(const Cost& cost, std::span<const Source> sources, int x) noexcept {
    if (x < 0 || sources.size() > kMaxSources) {
        return false;
    }

    // Phyrexian pips are treated as ordinary coloured pips.
    //
    // KNOWN SIMPLIFICATION, and its direction: {U/P} can really be paid with 2
    // life, so this UNDERSTATES what is castable. It is moot in this deck - the
    // only Phyrexian cost is Mental Misstep, which is inert (no opponents cast
    // spells) - and paying life needs a life total the model does not track yet.
    std::array<int, kColourCount> need{};
    int pip_total = 0;
    for (std::size_t c = 0; c < kColourCount; ++c) {
        need[c] = cost.pips[c] + cost.phyrexian[c];
        pip_total += need[c];
    }

    // {X} is X generic mana per symbol. Widened to long long before multiplying
    // so a large x cannot overflow into a small positive number and report a
    // cost as payable.
    const long long generic_need =
        static_cast<long long>(cost.generic) + static_cast<long long>(cost.variable) * x;
    const long long required = static_cast<long long>(pip_total) + generic_need;

    // Two independent conditions, and both are necessary.
    //
    // 1. Enough mana in total. Mana produced beyond a colour's requirement is
    //    not wasted - it pays generic - so the total is what matters here.
    if (total_mana(sources) < required) {
        return false;
    }
    // 2. The coloured requirements are actually assignable. This is the half a
    //    sum cannot answer: two mana from one Kinnan'd Birds is two mana, but
    //    it is not one green and one blue.
    return colours_assignable(need, sources);
}

int max_affordable_x(const Cost& cost, std::span<const Source> sources) noexcept {
    if (!can_pay(cost, sources, 0)) {
        return -1;
    }
    if (cost.variable == 0) {
        return 0;
    }
    // Raising x only raises the generic requirement, so payability is monotone
    // decreasing in x and a binary search is exact. total_mana is a hard upper
    // bound: x can never exceed the mana on the battlefield.
    int low = 0;
    int high = total_mana(sources);
    while (low < high) {
        const int mid = low + (high - low + 1) / 2;
        if (can_pay(cost, sources, mid)) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return low;
}

}  // namespace cs

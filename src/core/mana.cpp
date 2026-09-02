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
//
// `out`, when non-null, receives the matching the search found. ONE derivation
// of "which source serves which colour", with two callers: can_pay wants the
// bool and the payer wants the assignment. The previous arrangement computed it
// here, discarded it, and rebuilt something different in the turn loop.
bool colours_assignable(const std::array<int, kColourCount>& need,
                        std::span<const Source> sources,
                        std::array<SourceState, kMaxSources>* out = nullptr) noexcept {
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
    std::array<SourceState, kMaxSources> state{};
    if (count == 0) {
        if (out != nullptr) {
            *out = state;
        }
        return true;
    }

    const bool ok = place(0, std::span<const int>(pips.data(), count), sources, state);
    if (ok && out != nullptr) {
        *out = state;
    }
    return ok;
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

namespace {

// The cost, reduced to what payment needs: coloured requirements and a total.
// Shared so can_pay and plan_payment cannot come to differ about what is owed.
struct Requirement {
    std::array<int, kColourCount> need{};
    long long total = 0;
    bool valid = false;
};

Requirement requirement_of(const Cost& cost, std::span<const Source> sources, int x) noexcept {
    Requirement requirement;
    if (x < 0 || sources.size() > kMaxSources) {
        return requirement;
    }
    // Phyrexian pips are treated as ordinary coloured pips.
    //
    // KNOWN SIMPLIFICATION, and its direction: {U/P} can really be paid with 2
    // life, so this UNDERSTATES what is castable. It is moot in this deck - the
    // only Phyrexian cost is Mental Misstep, which is inert (no opponents cast
    // spells) - and paying life needs a life total the model does not track yet.
    int pip_total = 0;
    for (std::size_t c = 0; c < kColourCount; ++c) {
        requirement.need[c] = cost.pips[c] + cost.phyrexian[c];
        pip_total += requirement.need[c];
    }
    // {X} is X generic mana per symbol. Widened to long long before multiplying
    // so a large x cannot overflow into a small positive number and report a
    // cost as payable.
    const long long generic =
        static_cast<long long>(cost.generic) + static_cast<long long>(cost.variable) * x;
    requirement.total = static_cast<long long>(pip_total) + generic;
    requirement.valid = true;
    return requirement;
}

}  // namespace

Payment plan_payment(const Cost& cost, std::span<const Source> sources, int x) noexcept {
    Payment payment;
    const Requirement requirement = requirement_of(cost, sources, x);
    if (!requirement.valid || total_mana(sources) < requirement.total) {
        return payment;
    }
    std::array<SourceState, kMaxSources> matching{};
    if (!colours_assignable(requirement.need, sources, &matching)) {
        return payment;
    }
    payment.payable = true;

    const auto take = [&](std::size_t index) {
        payment.spend[index / 64] |= std::uint64_t{1} << (index % 64);
        payment.spent += sources[index].amount;
    };

    // Sources the colour matching committed. Required, not chosen: tapping the
    // source yields all of its mana, so the excess over `used` pays generic for
    // free and must be counted here rather than sought again below.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (matching[i].colour != -1) {
            take(i);
        }
    }

    // Then BEST FIT over the remainder, as the header states: the largest
    // source that does not exceed what is still owed, and when none fits, the
    // smallest that covers it. Minimises overpayment, so the mana left untapped
    // is maximised for the next spell this turn.
    while (payment.spent < requirement.total) {
        std::size_t best = sources.size();
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (payment.spends(i)) {
                continue;
            }
            if (best == sources.size()) {
                best = i;
                continue;
            }
            const long long owed = requirement.total - payment.spent;
            const int candidate = sources[i].amount;
            const int incumbent = sources[best].amount;
            const bool candidate_fits = candidate <= owed;
            const bool incumbent_fits = incumbent <= owed;
            if (candidate_fits != incumbent_fits) {
                // A source that fits beats one that overshoots.
                best = candidate_fits ? i : best;
            } else if (candidate_fits) {
                best = candidate > incumbent ? i : best;  // largest that fits
            } else {
                best = candidate < incumbent ? i : best;  // smallest that covers
            }
            // Ties keep the incumbent, which has the lower index. Section 6.5:
            // a tiebreak resolved by iteration accident is a reproducibility bug
            // that looks like variance.
        }
        if (best == sources.size()) {
            break;  // total_mana said this was payable; nothing left to take
        }
        take(best);
    }
    return payment;
}

bool can_pay(const Cost& cost, std::span<const Source> sources, int x) noexcept {
    // The two conditions, both necessary, and both now read off the SAME
    // requirement plan_payment uses:
    //
    // 1. Enough mana in total. Mana produced beyond a colour's requirement is
    //    not wasted - it pays generic - so the total is what matters here.
    // 2. The coloured requirements are actually assignable. This is the half a
    //    sum cannot answer: two mana from one Kinnan'd Birds is two mana, but
    //    it is not one green and one blue.
    //
    // This stays a separate entry point rather than `plan_payment(...).payable`
    // because it is the hot path - called for every candidate card, every turn,
    // of every game - and the generic best-fit loop is work a castability check
    // does not need. It shares the requirement and the colour matching, which
    // are the two things that could come to disagree.
    const Requirement requirement = requirement_of(cost, sources, x);
    if (!requirement.valid || total_mana(sources) < requirement.total) {
        return false;
    }
    return colours_assignable(requirement.need, sources);
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

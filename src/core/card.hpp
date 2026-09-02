#pragma once

// Card data as the simulation sees it. Plain values, standard library only.
//
// Nothing here knows about JSON, files, or the database. That is the point: the
// loader in io/ turns bytes into these types, and everything downstream - the
// mana system, the policy, the eventual Python bindings - depends only on this
// header. Putting a JSON type in this file would drag a parser into the core
// and undo the seal (SIM_PLAN.md section 12.5).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cs {

// Colour indices are fixed and dense so a colour can be an array subscript and
// a set of colours can be a 5-bit mask. WUBRG is the conventional order and the
// one the exporter emits.
enum class Colour : std::uint8_t { White = 0, Blue, Black, Red, Green };
inline constexpr std::size_t kColourCount = 5;

using ColourMask = std::uint8_t;

[[nodiscard]] constexpr ColourMask colour_bit(Colour c) noexcept {
    return static_cast<ColourMask>(1U << static_cast<std::uint8_t>(c));
}

// A cost split into the parts the mana system pays differently.
//
// `generic` is payable by any source. `pips` must be matched by colour, which
// is what makes payment a bipartite matching rather than a sum (section 6.4).
// `variable` counts {X} symbols - see section 2.2 on why an X spell's minimum
// castable cost is a cost at which the card does nothing.
struct Cost {
    std::uint8_t generic = 0;
    std::array<std::uint8_t, kColourCount> pips{};
    std::uint8_t variable = 0;
    std::array<std::uint8_t, kColourCount> phyrexian{};

    [[nodiscard]] int mana_value_at_x_zero() const noexcept {
        int total = generic;
        for (std::size_t i = 0; i < kColourCount; ++i) {
            total += pips[i] + phyrexian[i];
        }
        return total;
    }
};

// One face of a card. Single-faced cards have exactly one, synthesised by the
// exporter, so nothing downstream branches on face count.
struct Face {
    std::string name;
    std::string type_line;
    bool is_land = false;
    // nullopt means "not castable", which is different from a cost of zero.
    // Lotus Petal costs {0} and can be cast; a Forest cannot be cast at all.
    std::optional<Cost> cost;
    std::optional<int> mana_value;

    [[nodiscard]] bool is_castable() const noexcept { return cost.has_value(); }
};

struct Card {
    // Dense slot number, 0..n-1. The stable tiebreak key that makes every
    // ordering in the policy total (section 6.5), and the bit position this
    // card occupies in the zone bitsets (section 11).
    int export_index = 0;
    std::string listed_name;   // as written on a decklist
    std::string name;          // as stored, e.g. "A // B"
    std::string layout;
    int mana_value = 0;
    std::vector<int> castable_cmcs;
    std::vector<std::string> all_types;
    ColourMask colour_identity = 0;

    // UPSTREAM'S COLUMN, AND IT DOES NOT MEAN "IS A LAND".
    //
    // mtg_v1.has_land_face is true only for a MULTI-FACED card one of whose
    // faces is a land - Sink into Stupor // Soporific Springs. Forest is
    // `false`, and so is every other single-faced land: 24 of this deck's 25.
    //
    // It is kept because it is real data and the trap is worth documenting in
    // the place someone reaches for it. Ask plays_as_land() instead.
    bool has_land_face = false;

    bool is_commander = false;
    std::vector<Face> faces;

    // Can this card be put onto the battlefield as a land drop?
    //
    // THE definition, and the only one. It used to be a free function in
    // policy.cpp that walked the faces, sitting beside a loaded field of almost
    // the same name that answered a different question and was never read - so
    // the field was wrong about 24 of 25 lands for three phases and cost
    // nothing, because every consumer had quietly rebuilt it.
    [[nodiscard]] bool plays_as_land() const noexcept {
        for (const Face& face : faces) {
            if (face.is_land) {
                return true;
            }
        }
        return false;
    }

    // Is this a creature?
    //
    // all_types, deliberately, and it is a union over faces: a card on the
    // BATTLEFIELD is characterised by whichever face is up, and for every card
    // in this deck that reaches the battlefield all_types is a superset that
    // agrees. Contrast tutor_candidates, which searches the LIBRARY and must
    // read the front face - see the comment there.
    //
    // One definition because it had four: deck_load's creature_slots,
    // collect_sources' grant, condition_met's counters, and convoke.
    [[nodiscard]] bool is_creature() const noexcept {
        for (const std::string& type : all_types) {
            if (type == "Creature") {
                return true;
            }
        }
        return false;
    }

    // The first castable face, or nullptr. A single-faced card has one face; an
    // MDFC has a spell face and a land face (section 8.2).
    //
    // Also formerly two copies, one in sim.cpp and one in policy.cpp. They
    // agreed, which is the only reason that one was latent rather than a bug.
    [[nodiscard]] const Face* castable_face() const noexcept {
        for (const Face& face : faces) {
            if (face.is_castable()) {
                return &face;
            }
        }
        return nullptr;
    }
};

// Provenance for a card database. Carried so that a changed number is
// attributable to either the code or the card data (section 8.3), and so a
// stale file is detectable rather than silently old.
struct Manifest {
    std::string source_view;
    std::string generated_at;
    std::string max_content_updated_at;
    std::string cards_sha256;
    std::string metric;
    std::string measures;
    std::string does_not_measure;
    int card_count = 0;
    int corpus_row_count = 0;
    bool is_fixture = false;
};

struct CardDb {
    Manifest manifest;
    std::vector<Card> cards;

    [[nodiscard]] std::size_t size() const noexcept { return cards.size(); }
};

}  // namespace cs

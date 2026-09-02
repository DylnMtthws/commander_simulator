#include "io/trace.hpp"

#include <string>

namespace cs::io {

const char* TraceWriter::name_of(int slot) const {
    return db_.cards[static_cast<std::size_t>(slot)].listed_name.c_str();
}

void TraceWriter::turn_begin(int turn, const GameState& state) {
    std::fprintf(out_, "\nT%-2d  hand %d  board %d  library %d  life %d\n", turn,
                 state.hand.count(), state.battlefield.count(), state.library_size(),
                 state.life);
}

void TraceWriter::drew(int slot, int hand_size) {
     std::fprintf(out_, "     draw: %-28s [hand %d]\n", name_of(slot), hand_size);
}

void TraceWriter::mana(std::span<const Source> sources) {
    // Rendered as a colour mask per source rather than a total, because the
    // total is the half that never explains a failure: "4 mana" and "4 mana,
    // all colourless" look identical and only one of them casts Kinnan.
    std::fprintf(out_, "     mana: ");
    if (sources.empty()) {
        std::fprintf(out_, "(none)\n");
        return;
    }
    static constexpr char kLetters[] = "WUBRG";
    for (const Source& source : sources) {
        std::string colours;
        for (std::size_t c = 0; c < kColourCount; ++c) {
            if ((source.produces & static_cast<ColourMask>(1U << c)) != 0) {
                colours.push_back(kLetters[c]);
            }
        }
        if (colours.empty()) {
            colours = "C";
        }
        std::fprintf(out_, "%dx{%s} ", source.amount, colours.c_str());
    }
    std::fprintf(out_, "\n");
}

void TraceWriter::considering(std::span<const Consideration> candidates, const char* what) {
    if (candidates.empty()) {
        return;
    }
    std::fprintf(out_, "     considering (%s):\n", what);
    for (const Consideration& candidate : candidates) {
        std::fprintf(out_, "       %-28s score %6d  %-10s rank %5d", name_of(candidate.slot),
                     candidate.score, candidate.castable ? "castable" : "NOT castable",
                     candidate.rank_term);
        if (candidate.pattern_term != 0) {
            std::fprintf(out_, "  pattern %+d", candidate.pattern_term);
        }
        if (candidate.land_term != 0) {
            std::fprintf(out_, "  land %+d", candidate.land_term);
        }
        if (candidate.castable_term != 0) {
            std::fprintf(out_, "  uncastable %+d", candidate.castable_term);
        }
        std::fprintf(out_, "\n");
    }
}

void TraceWriter::played_land(int slot) {
    std::fprintf(out_, "     PLAY LAND: %s\n", name_of(slot));
}

void TraceWriter::cast_spell(int slot, int paid) {
    std::fprintf(out_, "     CAST: %-28s paying %d\n", name_of(slot), paid);
}

void TraceWriter::engines_active(FlagMask flags) {
    if (flags == 0) {
        return;
    }
    std::fprintf(out_, "     engines:");
    for (std::size_t i = 0; i < patterns_.flag_names.size(); ++i) {
        if ((flags & (FlagMask{1} << i)) != 0) {
            std::fprintf(out_, " %s", patterns_.flag_names[i].c_str());
        }
    }
    std::fprintf(out_, "\n");
}

void TraceWriter::pattern_fired(int pattern, int turn) {
    std::fprintf(out_, "\n  ASSEMBLED on turn %d: %s\n", turn,
                 patterns_.patterns[static_cast<std::size_t>(pattern)].name.c_str());
}

void TraceWriter::game_end(int turn, bool censored) {
    if (censored) {
        std::fprintf(out_, "\n  CENSORED: %d turns, no pattern assembled\n", turn);
    }
}

}  // namespace cs::io

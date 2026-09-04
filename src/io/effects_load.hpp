#pragma once

#include <filesystem>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "io/deck_load.hpp"

namespace cs::io {

// Parses data/effects.toml. Unknown kinds and unknown enters_tapped shapes are
// REFUSED by name: the vocabulary is closed for the same reason the pattern
// vocabulary is, and an ignored effect makes a card silently free.
// `patterns` is the selected/inherited set for this deck. An effects entry may
// be absent only when no inherited pattern names that card; otherwise the
// simulator would claim to detect a line containing text it never authored.
[[nodiscard]] EffectDb load_effects(const std::filesystem::path& path, const CardDb& db,
                                    const PatternSet* patterns = nullptr);

}  // namespace cs::io

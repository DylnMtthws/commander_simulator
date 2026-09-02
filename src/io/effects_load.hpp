#pragma once

#include <filesystem>

#include "core/card.hpp"
#include "core/effects.hpp"
#include "io/deck_load.hpp"

namespace cs::io {

// Parses data/effects.toml. Unknown kinds and unknown enters_tapped shapes are
// REFUSED by name: the vocabulary is closed for the same reason the pattern
// vocabulary is, and an ignored effect makes a card silently free.
[[nodiscard]] EffectDb load_effects(const std::filesystem::path& path, const CardDb& db);

}  // namespace cs::io

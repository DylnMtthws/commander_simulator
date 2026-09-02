// Loads a card database and prints what it found.
//
// The first end-to-end path: exporter -> cards.json -> C++ -> a summary a human
// can check against the database. Argument parsing, the metric banner and the
// statistics arrive in Phase 6 (SIM_PLAN.md section 9.5).

#include <cstdio>
#include <filesystem>
#include <string>

#include "core/card.hpp"
#include "core/version.hpp"
#include "io/card_db_load.hpp"

namespace {

// std::string_view is not guaranteed null-terminated - it is a pointer and a
// length, and may point into the middle of a buffer. "%s" would read until it
// happened to find a zero byte. Print one by passing its length.
void print_view(const char* format, std::string_view value) {
    std::printf(format, static_cast<int>(value.size()), value.data());
}

int summarise(const std::filesystem::path& path) {
    cs::CardDb db;
    try {
        db = cs::io::load_card_db(path);
    } catch (const cs::io::LoadError& error) {
        // stdout is buffered and stderr is not, so without this the error can
        // appear before output already written above it.
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    const cs::Manifest& m = db.manifest;
    std::printf("%s\n", path.c_str());
    std::printf("  %zu cards from %s%s\n", db.size(), m.source_view.c_str(),
                m.is_fixture ? "  [FIXTURE]" : "");
    std::printf("  data as of %s\n", m.max_content_updated_at.c_str());
    std::printf("  cards sha256 %s\n", m.cards_sha256.substr(0, 12).c_str());
    if (m.corpus_row_count > 0) {
        std::printf("  corpus %d rows\n", m.corpus_row_count);
    }

    std::size_t lands = 0;
    std::size_t castable = 0;
    std::size_t variable = 0;
    std::size_t multi_faced = 0;
    for (const cs::Card& card : db.cards) {
        bool has_land = false;
        bool has_castable = false;
        bool has_variable = false;
        for (const cs::Face& face : card.faces) {
            has_land = has_land || face.is_land;
            has_castable = has_castable || face.is_castable();
            has_variable = has_variable || (face.cost && face.cost->variable > 0);
        }
        lands += has_land ? 1 : 0;
        castable += has_castable ? 1 : 0;
        variable += has_variable ? 1 : 0;
        multi_faced += card.faces.size() > 1 ? 1 : 0;
    }

    std::printf("  %zu with a land face, %zu castable, %zu X spells, %zu multi-faced\n",
                lands, castable, variable, multi_faced);

    if (!m.does_not_measure.empty()) {
        std::printf("  metric %s; NOT %s\n", m.metric.c_str(), m.does_not_measure.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto v = cs::version();
    const auto flavour = cs::build_flavour();
    std::printf("commander_simulator ");
    print_view("%.*s", v);
    std::printf(" (");
    print_view("%.*s", flavour);
    std::printf(" build)\n\n");

    const std::filesystem::path path =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("data/cards.json");
    return summarise(path);
}

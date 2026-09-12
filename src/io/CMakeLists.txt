# Loading: bytes on disk -> core types.
#
# THE JSON DEPENDENCY, and why it lives only here.
#
# nlohmann/json, fetched and pinned like Catch2. Reasons, in order:
#
#   1. It is NOT on the hot path. cards.json is ~75 KB and is parsed once per
#      process against a run measured in minutes, so parse speed is irrelevant
#      and readability wins outright. Choosing a fast-but-awkward parser here
#      would be optimising 0.001% of runtime at the cost of every debugging
#      session (SIM_PLAN.md section 8.4).
#   2. It is header-only, so there is no build configuration to get wrong.
#   3. Its API converts into plain structs without a schema compiler, which is
#      exactly the amount of machinery this needs.
#
# The alternatives were considered and rejected for this use: RapidJSON is
# faster and has a much sharper API; simdjson is far faster and is built for
# read-only scanning of large documents, which is not the shape of this problem.
#
# WHERE IT MAY BE USED: this target, and nothing else. cs_core links only the
# standard library, so a core file that includes nlohmann/json fails to compile
# - the include path is not there to find. That is the seal working as a build
# property rather than as a convention (section 12.5).

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3          # pinned, never a branch
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS NAMES nlohmann_json)
FetchContent_MakeAvailable(nlohmann_json)

# toml++ for the deck file. TOML rather than JSON because deck files are
# hand-authored and hand-edited constantly - patterns, ranks, scorer weights -
# and TOML has comments. A design document you cannot annotate is one you will
# stop annotating.
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS NAMES tomlplusplus)
FetchContent_MakeAvailable(tomlplusplus)

add_library(cs_io resource_probe_io.cpp card_db_load.cpp deck_load.cpp effects_load.cpp trace.cpp sha256.cpp service.cpp)
target_include_directories(cs_io PUBLIC ${PROJECT_SOURCE_DIR}/src)

# nlohmann_json is PRIVATE: cs_io's header exposes only core types and
# std::filesystem::path, so nothing that links cs_io inherits the JSON library.
# A consumer reaching for nlohmann/json has to ask for it deliberately.
target_link_libraries(cs_io
    PUBLIC  cs_core
    PRIVATE cs_warnings nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus)

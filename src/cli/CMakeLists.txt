# The command-line front end.
#
# All I/O lives on this side of the boundary. In later phases the JSON/TOML
# loaders and the trace writer join it (as an io/ target); cs_core stays sealed.
add_executable(cs main.cpp)
# cs_core is NOT listed here even though main.cpp includes core/version.hpp.
# cs_io links it PUBLIC - correctly, since card_db_load.hpp exposes core types
# in its own interface - so it arrives transitively. Naming it again puts the
# same static library on the link line twice, which macOS ld warns about.
target_link_libraries(cs PRIVATE cs_io cs_warnings)

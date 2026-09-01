# The command-line front end.
#
# All I/O lives on this side of the boundary. In later phases the JSON/TOML
# loaders and the trace writer join it (as an io/ target); cs_core stays sealed.
add_executable(cs main.cpp)
target_link_libraries(cs PRIVATE cs_core cs_warnings)

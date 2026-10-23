# Builds the demo song "Sodium Lights" (liquid drum & bass) through the Command API, saves it and renders it.
# Reuses the showcase song's helpers (tools/showcase-song). Not run by CI: `spinus_demo_dnb --help`.
add_executable (spinus_demo_dnb EXCLUDE_FROM_ALL DnbSong.cpp)
target_include_directories (spinus_demo_dnb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../showcase-song)
target_link_libraries (spinus_demo_dnb PRIVATE spinus_core spinus_test_support spinus_warnings)

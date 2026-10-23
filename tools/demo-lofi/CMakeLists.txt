# Builds the lo-fi demo song "Rainy Window" through the Command API (reusing tools/showcase-song's helpers),
# saves it and renders it. Not run by CI: `spinus_demo_lofi --help`, and demo/README.md.
add_executable (spinus_demo_lofi EXCLUDE_FROM_ALL LofiSong.cpp)
target_include_directories (spinus_demo_lofi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../showcase-song")
target_link_libraries (spinus_demo_lofi PRIVATE spinus_core spinus_test_support spinus_warnings)

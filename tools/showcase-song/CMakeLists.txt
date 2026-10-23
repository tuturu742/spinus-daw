# Builds the showcase song (a demo and stress test) through the Command API, saves it, renders it and
# measures loading, opening patterns, saving and rendering. Not run by CI: `spinus_showcase_song --help`.
add_executable (spinus_showcase_song EXCLUDE_FROM_ALL ShowcaseSong.cpp)
target_link_libraries (spinus_showcase_song PRIVATE spinus_core spinus_test_support spinus_warnings)

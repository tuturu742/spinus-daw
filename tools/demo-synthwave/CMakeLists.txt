# Builds the "Neon Coast" demo song (synthwave) through the Command API, reusing the showcase song's helpers
# (tools/showcase-song), saves it and renders it. Not run by CI: `spinus_demo_synthwave --help`.
add_executable (spinus_demo_synthwave EXCLUDE_FROM_ALL SynthwaveSong.cpp)
target_include_directories (spinus_demo_synthwave PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../showcase-song")
target_link_libraries (spinus_demo_synthwave PRIVATE spinus_core spinus_test_support spinus_warnings)

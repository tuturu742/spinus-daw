# Helpers for tests: a real-time paced engine runner and audio checks.
add_library (spinus_test_support STATIC
    AudioChecks.h
    FakeSoundCard.cpp
    FakeSoundCard.h
    ProgramsTestPlugin.h
    RealtimeRunner.cpp
    RealtimeRunner.h)

target_include_directories (spinus_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries (spinus_test_support PUBLIC spinus_core PRIVATE spinus_warnings)

cmake_minimum_required (VERSION 3.25)

project (spinus
         VERSION 0.1.0
         DESCRIPTION "Spinus DAW: a Linux-first, FL-style digital audio workstation"
         HOMEPAGE_URL "https://github.com/tuturu742/spinus-daw"
         LANGUAGES C CXX)

# The version people see: PROJECT_VERSION plus a pre-release suffix ("-rc1"), empty for a final release.
set (SPINUS_VERSION_SUFFIX "-rc1" CACHE STRING "Pre-release suffix of the version, e.g. -rc1")
set (SPINUS_VERSION "${PROJECT_VERSION}${SPINUS_VERSION_SUFFIX}")

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

option (SPINUS_BUILD_TESTS "Build the Spinus unit tests" ON)
option (SPINUS_ENABLE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option (SPINUS_WARNINGS_AS_ERRORS "Treat warnings in Spinus code as errors" OFF)

if (SPINUS_ENABLE_ASAN)
    add_compile_options (-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options (-fsanitize=address,undefined)
endif()

#==============================================================================
# Third-party code (pinned git submodules, see THIRD_PARTY.md)

foreach (dep JUCE tracktion_engine Catch2 clap fst)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/${dep}/CMakeLists.txt")
        message (FATAL_ERROR "external/${dep} is missing. Run: git submodule update --init")
    endif()
endforeach()

add_subdirectory (external/JUCE EXCLUDE_FROM_ALL)
# Register only the Tracktion modules; their bundled JUCE submodule is not used.
add_subdirectory (external/tracktion_engine/modules EXCLUDE_FROM_ALL)

#==============================================================================
# Spinus

include (cmake/SpinusJuce.cmake)
include (cmake/SpinusSurge.cmake)
include (cmake/SpinusBundledPlugins.cmake)
include (cmake/SpinusOwnPlugins.cmake)

# Paths built into the programs (the source tree, test files and helpers): a Windows build made on Linux runs
# under Wine, where the Linux file system is drive Z:.
set (SPINUS_BUILT_IN_PATH_PREFIX "")
if (WIN32 AND CMAKE_CROSSCOMPILING)
    set (SPINUS_BUILT_IN_PATH_PREFIX "Z:")
endif()

add_subdirectory (src)
add_subdirectory (app)
include (cmake/SpinusInstall.cmake)

if (SPINUS_BUILD_TESTS)
    add_subdirectory (external/Catch2 EXCLUDE_FROM_ALL)
    enable_testing()
    add_subdirectory (tests)
    add_subdirectory (tools/showcase-song) # a demo song made through the Command API (not part of CI)
    add_subdirectory (tools/demo-synthwave) # the synthwave demo song, "Neon Coast" (not part of CI)
    add_subdirectory (tools/demo-dnb) # the liquid drum & bass demo song, made the same way (not part of CI)
    add_subdirectory (tools/demo-lofi) # the lo-fi demo song "Rainy Window" (not part of CI)

endif()

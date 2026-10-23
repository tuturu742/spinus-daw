# Cross-building Spinus for Windows x64 on Linux: clang-cl and lld-link (LLVM) against Microsoft's C runtime
# and Windows SDK, laid out by `xwin splat` (tools/windows/get-msvc-sdk.sh). JUCE no longer supports MinGW
# (JUCE BREAKING_CHANGES.md; juce_TargetPlatform.h stops with an #error), so the MSVC ABI is the only way.
# The SDK is used to build, never shipped: with the static runtime below, spinus.exe needs nothing from it.
#
#   tools/windows/get-msvc-sdk.sh --accept-license      # once
#   cmake --preset windows-cross && cmake --build --preset windows-cross

set (CMAKE_SYSTEM_NAME Windows)
set (CMAKE_SYSTEM_VERSION 10.0)
set (CMAKE_SYSTEM_PROCESSOR AMD64)

if (NOT SPINUS_XWIN_DIR)
    set (SPINUS_XWIN_DIR "$ENV{SPINUS_XWIN_DIR}")
endif()
if (NOT SPINUS_XWIN_DIR)
    set (SPINUS_XWIN_DIR "$ENV{HOME}/.local/share/spinus-xwin")
endif()
if (NOT EXISTS "${SPINUS_XWIN_DIR}/crt/include")
    message (FATAL_ERROR "No Windows SDK in ${SPINUS_XWIN_DIR}: run tools/windows/get-msvc-sdk.sh first")
endif()
# Passed on to CMake's own test projects (try_compile), which read this file again.
list (APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SPINUS_XWIN_DIR)

find_program (CMAKE_C_COMPILER NAMES clang-cl REQUIRED)
find_program (CMAKE_CXX_COMPILER NAMES clang-cl REQUIRED)
find_program (CMAKE_LINKER NAMES lld-link REQUIRED)
find_program (CMAKE_AR NAMES llvm-lib REQUIRED)
find_program (CMAKE_RC_COMPILER NAMES llvm-rc REQUIRED)
find_program (CMAKE_MT NAMES llvm-mt REQUIRED)

set (xwinFlags "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link")
foreach (dir crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt)
    string (APPEND xwinFlags " /imsvc \"${SPINUS_XWIN_DIR}/${dir}\"")
endforeach()
set (CMAKE_C_FLAGS_INIT "${xwinFlags}")
set (CMAKE_CXX_FLAGS_INIT "${xwinFlags} /EHsc")
set (CMAKE_RC_FLAGS_INIT "/I \"${SPINUS_XWIN_DIR}/sdk/include/um\" /I \"${SPINUS_XWIN_DIR}/sdk/include/shared\"")

set (xwinLinkFlags "")
foreach (dir crt/lib/x86_64 sdk/lib/um/x86_64 sdk/lib/ucrt/x86_64)
    string (APPEND xwinLinkFlags " /libpath:\"${SPINUS_XWIN_DIR}/${dir}\"")
endforeach()
set (CMAKE_EXE_LINKER_FLAGS_INIT "${xwinLinkFlags}")
set (CMAKE_SHARED_LINKER_FLAGS_INIT "${xwinLinkFlags}")
set (CMAKE_MODULE_LINKER_FLAGS_INIT "${xwinLinkFlags}")

# The static runtime: spinus.exe then needs no Visual C++ Redistributable on the user's PC.
set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# xwin fetches the release runtime only; CMake's own test projects would otherwise link the debug one.
set (CMAKE_TRY_COMPILE_CONFIGURATION Release)

# ctest runs the Windows test programs under Wine, when it is installed (a check only: Spinus is tested on
# Windows itself by .github/workflows/windows.yml).
find_program (SPINUS_WINE wine)
if (SPINUS_WINE)
    set (CMAKE_CROSSCOMPILING_EMULATOR "${SPINUS_WINE}")
endif()

# Libraries Spinus builds for Windows itself go in the build folder; nothing is taken from the Linux system.
set (CMAKE_FIND_ROOT_PATH "${SPINUS_XWIN_DIR}")
set (CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set (CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

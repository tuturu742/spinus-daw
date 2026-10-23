# Surge XT, the bundled flagship synth, and Surge XT Effects, its effects
# as a separate plugin.
#
# Surge XT is built as its own CMake project because it pins its own JUCE fork, which must not
# mix with Spinus's JUCE 9. Spinus loads the resulting VST3 in-process like any plugin; the
# plugin binary keeps its JUCE symbols private.
#
# Output (in the build tree):
#   plugins/Surge XT.vst3, plugins/Surge XT.clap
#   plugins/Surge XT Effects.vst3, plugins/Surge XT Effects.clap
#   plugins/SurgeXTData -> factory patches and wavetables ("portable mode": Surge looks for a
#                          SurgeXTData folder beside its binary, so no system install is needed)

option (SPINUS_BUILD_SURGE "Build the bundled Surge XT synth (adds several minutes to a clean build)" ON)

set (SPINUS_PLUGINS_DIR "${CMAKE_BINARY_DIR}/plugins")
set (SPINUS_SURGE_VST3 "${SPINUS_PLUGINS_DIR}/Surge XT.vst3")
set (SPINUS_SURGE_CLAP "${SPINUS_PLUGINS_DIR}/Surge XT.clap")
set (SPINUS_SURGE_FX_VST3 "${SPINUS_PLUGINS_DIR}/Surge XT Effects.vst3")
set (SPINUS_SURGE_FX_CLAP "${SPINUS_PLUGINS_DIR}/Surge XT Effects.clap")

if (NOT SPINUS_BUILD_SURGE)
    return()
endif()

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/surge/CMakeLists.txt")
    message (FATAL_ERROR "external/surge is missing. Run: git submodule update --init --recursive external/surge")
endif()

include (ExternalProject)

set (surgeSource "${CMAKE_CURRENT_SOURCE_DIR}/external/surge")
set (surgeBinary "${CMAKE_BINARY_DIR}/surge-build")
set (surgeProducts "${surgeBinary}/surge_xt_products")

ExternalProject_Add (surge_xt
    SOURCE_DIR "${surgeSource}"
    BINARY_DIR "${surgeBinary}"
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
        -DSURGE_BUILD_TESTRUNNER=OFF
        -DSURGE_BUILD_LV2=OFF
        -DSURGE_SKIP_WERROR=ON
        # Surge XT 1.3.4's bundled libraries predate CMake 4.
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    BUILD_COMMAND ${CMAKE_COMMAND} --build "${surgeBinary}" --target surge-xt_VST3 surge-xt_CLAP surge-fx_VST3 surge-fx_CLAP
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory "${SPINUS_PLUGINS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${surgeProducts}/Surge XT.vst3" "${SPINUS_SURGE_VST3}"
        COMMAND ${CMAKE_COMMAND} -E copy "${surgeProducts}/Surge XT.clap" "${SPINUS_SURGE_CLAP}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${surgeProducts}/Surge XT Effects.vst3" "${SPINUS_SURGE_FX_VST3}"
        COMMAND ${CMAKE_COMMAND} -E copy "${surgeProducts}/Surge XT Effects.clap" "${SPINUS_SURGE_FX_CLAP}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${surgeSource}/resources/data" "${SPINUS_PLUGINS_DIR}/SurgeXTData"
    BUILD_BYPRODUCTS "${SPINUS_SURGE_CLAP}" "${SPINUS_SURGE_FX_CLAP}"
    USES_TERMINAL_BUILD ON)

# Bundled open-source effect plugins.
#
# Spinus ships a curated set of third-party effects instead of writing its own for v1:
#   LSP Plugins (LGPL-3.0-or-later): EQ, compressor, limiter, gate, multiband, analyzer, delay, ...
#   Dragonfly Reverb (GPL-3.0-or-later): hall, room, plate and early-reflections reverbs
# Surge XT Effects comes from the Surge sub-build (cmake/SpinusSurge.cmake).
#
# Both projects use their own Makefiles, so they are built as external projects from pinned
# release tarballs (checked by SHA-256). Spinus loads the CLAP builds in-process like any plugin.
#
# Output (in the build tree):
#   plugins/lsp-plugins.clap, plugins/lsp-plugins.vst3
#   plugins/Dragonfly*.clap, plugins/Dragonfly*.vst3
#
# Runtime libraries needed by LSP Plugins: libsndfile, cairo, freetype, libGL, libX11, libXrandr.

option (SPINUS_BUILD_BUNDLED_PLUGINS "Build the bundled LSP Plugins and Dragonfly Reverb effects" ON)

set (SPINUS_PLUGINS_DIR "${CMAKE_BINARY_DIR}/plugins")
set (SPINUS_LSP_CLAP "${SPINUS_PLUGINS_DIR}/lsp-plugins.clap")
set (SPINUS_LSP_VST3 "${SPINUS_PLUGINS_DIR}/lsp-plugins.vst3")
set (SPINUS_DRAGONFLY_PLUGINS DragonflyHallReverb DragonflyRoomReverb DragonflyPlateReverb DragonflyEarlyReflections)

if (NOT SPINUS_BUILD_BUNDLED_PLUGINS)
    return()
endif()

include (ExternalProject)

find_program (SPINUS_MAKE NAMES gmake make REQUIRED)
cmake_host_system_information (RESULT spinusJobs QUERY NUMBER_OF_LOGICAL_CORES)

# The LSP modules Spinus bundles. Each module contains several variants (mono, stereo, sidechain,
# left/right, mid/side), all of which pass pluginval and the random-parameter test.
# Modules not listed here failed validation (Chorus, Flanger, Phaser and Dynamics Processor gave NaN
# under automation; Clipper crashed) or are not needed.
set (SPINUS_LSP_MODULES
    LSP_PLUGINS_PARA_EQUALIZER
    LSP_PLUGINS_COMPRESSOR
    LSP_PLUGINS_LIMITER
    LSP_PLUGINS_GATE
    LSP_PLUGINS_MB_COMPRESSOR
    LSP_PLUGINS_SPECTRUM_ANALYZER)
list (JOIN SPINUS_LSP_MODULES " " lspModules)

set (lspVersion 1.2.35)
# Packagers can pass the downloaded archives (the PKGBUILD lists them as sources); the hashes still apply.
set (SPINUS_LSP_ARCHIVE
     "https://github.com/lsp-plugins/lsp-plugins/releases/download/${lspVersion}/lsp-plugins-src-${lspVersion}.tar.gz"
     CACHE STRING "LSP Plugins source archive (URL or local file)")
set (lspInstall "${CMAKE_BINARY_DIR}/lsp-plugins-install")

ExternalProject_Add (lsp_plugins
    URL "${SPINUS_LSP_ARCHIVE}"
    URL_HASH SHA256=2c95ec7bb219d561ea3db36051b6c732133bcd76426fb836b1dd850dc4b5bb6c
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${CMAKE_BINARY_DIR}/lsp-plugins"
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${SPINUS_MAKE} config "FEATURES=clap vst3 ui" "PLUGIN_DEPENDENCIES=${lspModules}"
                      "PREFIX=${lspInstall}" "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
    BUILD_COMMAND ${SPINUS_MAKE} -j${spinusJobs}
    INSTALL_COMMAND ${SPINUS_MAKE} install
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SPINUS_PLUGINS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "${lspInstall}/lib/clap/lsp-plugins.clap" "${SPINUS_LSP_CLAP}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${lspInstall}/lib/vst3/lsp-plugins.vst3" "${SPINUS_LSP_VST3}"
    BUILD_BYPRODUCTS "${SPINUS_LSP_CLAP}"
    USES_TERMINAL_BUILD ON)

set (dragonflyVersion 3.2.10)
set (SPINUS_DRAGONFLY_ARCHIVE
     "https://github.com/michaelwillis/dragonfly-reverb/releases/download/${dragonflyVersion}/dragonfly-reverb-${dragonflyVersion}-src.tar.xz"
     CACHE STRING "Dragonfly Reverb source archive (URL or local file)")
set (dragonflyMake ${SPINUS_MAKE} -j${spinusJobs} "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}")
set (dragonflyBuild ${dragonflyMake} -C dpf/dgl FILE_BROWSER_DISABLED=true)
set (dragonflyInstall COMMAND ${CMAKE_COMMAND} -E make_directory "${SPINUS_PLUGINS_DIR}")

# No JACK standalone or LV2 targets: they need extra tools and libraries.
# The VST2 builds are not shipped; tests/checks uses them to test Spinus's VST2 hosting.
foreach (dir hall-reverb room-reverb plate-reverb early-reflections)
    list (APPEND dragonflyBuild COMMAND ${dragonflyMake} -C plugins/dragonfly-${dir} vst3 clap vst2)
endforeach()

set (SPINUS_TEST_PLUGINS_DIR "${CMAKE_BINARY_DIR}/test-plugins")
list (APPEND dragonflyInstall COMMAND ${CMAKE_COMMAND} -E make_directory "${SPINUS_TEST_PLUGINS_DIR}")

foreach (plugin IN LISTS SPINUS_DRAGONFLY_PLUGINS)
    list (APPEND dragonflyInstall
        COMMAND ${CMAKE_COMMAND} -E copy "<SOURCE_DIR>/bin/${plugin}-vst.so" "${SPINUS_TEST_PLUGINS_DIR}/${plugin}-vst.so")
    list (APPEND dragonflyInstall
        COMMAND ${CMAKE_COMMAND} -E copy "<SOURCE_DIR>/bin/${plugin}.clap" "${SPINUS_PLUGINS_DIR}/${plugin}.clap"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "<SOURCE_DIR>/bin/${plugin}.vst3" "${SPINUS_PLUGINS_DIR}/${plugin}.vst3")
endforeach()

ExternalProject_Add (dragonfly_reverb
    URL "${SPINUS_DRAGONFLY_ARCHIVE}"
    URL_HASH SHA256=18af55a9592c9f50c4d5f86c9d5159132735d9ba53d49e9cfe7169b3109f7743
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${CMAKE_BINARY_DIR}/dragonfly-reverb"
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${dragonflyBuild}
    INSTALL_COMMAND ${dragonflyInstall}
    USES_TERMINAL_BUILD ON)

add_custom_target (spinus_bundled_plugins DEPENDS lsp_plugins dragonfly_reverb)

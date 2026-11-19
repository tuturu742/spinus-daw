# Surge XT, the flagship synth, and Surge XT Effects, its effects as a separate plugin, for the tests
# that play them.
#
# These are the Surge project's own pinned releases that users get from Options > Get free plugins
# (src/plugins/PluginDownloads.cpp: the same archives and SHA-256 hashes), so tests run the build users
# run. Nothing is compiled. The archives are kept in SPINUS_DOWNLOADS_DIR, which CI caches.
#
# Output (in the build tree):
#   plugins/Surge XT.vst3, plugins/Surge XT.clap
#   plugins/Surge XT Effects.vst3, plugins/Surge XT Effects.clap
#   plugins/SurgeXTData -> factory patches and wavetables ("portable mode": Surge looks for a
#                          SurgeXTData folder beside its plugin file, so no system install is needed)

option (SPINUS_GET_SURGE "Download Surge XT's release (about 350 MB, once) for the tests that play it" ON)

set (SPINUS_PLUGINS_DIR "${CMAKE_BINARY_DIR}/plugins")
set (SPINUS_SURGE_VST3 "${SPINUS_PLUGINS_DIR}/Surge XT.vst3")
set (SPINUS_SURGE_CLAP "${SPINUS_PLUGINS_DIR}/Surge XT.clap")
set (SPINUS_SURGE_FX_VST3 "${SPINUS_PLUGINS_DIR}/Surge XT Effects.vst3")
set (SPINUS_SURGE_FX_CLAP "${SPINUS_PLUGINS_DIR}/Surge XT Effects.clap")

if (NOT SPINUS_GET_SURGE)
    return()
endif()

include (ExternalProject)

set (SPINUS_DOWNLOADS_DIR "${CMAKE_BINARY_DIR}/downloads" CACHE PATH "Where downloaded release archives are kept")

set (surgeRelease "https://github.com/surge-synthesizer/releases-xt/releases/download/1.3.4")
if (WIN32)
    set (surgePlugins "${surgeRelease}/surge-xt-win64-1.3.4-pluginsonly.zip")
    set (surgePluginsHash 564e162c560af07ad4ed47fe1bfcd827cf97a575de30d06c48249aad2e7c35e6)
else()
    set (surgePlugins "${surgeRelease}/surge-xt-linux-1.3.4-pluginsonly.tar.gz")
    set (surgePluginsHash dd431b75f5fa197c4bffa6ca27ca46970f0a94c834119bb1db7decdeec4c28db)
endif()

# The sounds, the same on every system.
ExternalProject_Add (surge_xt_content
    URL "${surgeRelease}/surge-xt-portable-content-1.3.4.tar.gz"
    URL_HASH SHA256=7ed24822eab69f73101d8558222979b1bceb4882a780292bb7087309b7b101bd
    DOWNLOAD_DIR "${SPINUS_DOWNLOADS_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${CMAKE_BINARY_DIR}/surge-xt-content"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory "${SPINUS_PLUGINS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "<SOURCE_DIR>/Surge Synth Team/SurgeXTData"
                "${SPINUS_PLUGINS_DIR}/SurgeXTData")

ExternalProject_Add (surge_xt
    URL "${surgePlugins}"
    URL_HASH SHA256=${surgePluginsHash}
    DOWNLOAD_DIR "${SPINUS_DOWNLOADS_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${CMAKE_BINARY_DIR}/surge-xt"
    DEPENDS surge_xt_content
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory "${SPINUS_PLUGINS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "<SOURCE_DIR>/Surge XT.vst3" "${SPINUS_SURGE_VST3}"
        COMMAND ${CMAKE_COMMAND} -E copy "<SOURCE_DIR>/Surge XT.clap" "${SPINUS_SURGE_CLAP}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "<SOURCE_DIR>/Surge XT Effects.vst3" "${SPINUS_SURGE_FX_VST3}"
        COMMAND ${CMAKE_COMMAND} -E copy "<SOURCE_DIR>/Surge XT Effects.clap" "${SPINUS_SURGE_FX_CLAP}"
    BUILD_BYPRODUCTS "${SPINUS_SURGE_CLAP}" "${SPINUS_SURGE_FX_CLAP}")

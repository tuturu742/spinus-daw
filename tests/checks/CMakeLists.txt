# Checks that are not unit tests (ctest doesn't run them), built with the tests so they keep compiling.

# An audio device's latency and dropouts (needs a real sound card; tools/owner-checks/audio-latency.sh).
add_executable (spinus_latency_probe AudioLatencyProbe.cpp)
target_link_libraries (spinus_latency_probe PRIVATE spinus_juce spinus_warnings)

# VST2 hosting through the FST header (the VST2 builds of Dragonfly Reverb), and Windows plugins through
# yabridge (only with SPINUS_CHECK_WINDOWS_PLUGINS; tools/owner-checks/windows-plugins-setup.sh).
if (SPINUS_BUILD_BUNDLED_PLUGINS)
    add_executable (spinus_plugin_hosting_checks ../TestMain.cpp Vst2HostingTests.cpp WindowsPluginTests.cpp)
    target_link_libraries (spinus_plugin_hosting_checks PRIVATE spinus_test_support spinus_warnings Catch2::Catch2)
    target_compile_definitions (spinus_plugin_hosting_checks PRIVATE
        SPINUS_PLUGINS_DIR="${SPINUS_PLUGINS_DIR}"
        SPINUS_TEST_PLUGINS_DIR="${SPINUS_TEST_PLUGINS_DIR}")
    add_dependencies (spinus_plugin_hosting_checks dragonfly_reverb)
endif()

# The curated LSP Plugins, Dragonfly Reverb and Surge XT Effects through Spinus's CLAP host and JUCE's VST3
# host: scanning, clean processing, state, random parameters, the channel budget.
if (SPINUS_BUILD_SURGE AND SPINUS_BUILD_BUNDLED_PLUGINS)
    add_executable (spinus_bundled_plugin_checks ../TestMain.cpp BundledPluginTests.cpp)
    target_link_libraries (spinus_bundled_plugin_checks PRIVATE spinus_test_support spinus_warnings Catch2::Catch2)
    target_compile_definitions (spinus_bundled_plugin_checks PRIVATE SPINUS_PLUGINS_DIR="${SPINUS_PLUGINS_DIR}")
    add_dependencies (spinus_bundled_plugin_checks surge_xt spinus_bundled_plugins)
endif()

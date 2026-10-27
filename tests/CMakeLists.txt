add_subdirectory (support)
if (NOT WIN32)
    add_subdirectory (checks) # measuring tools for Linux sound (PipeWire, JACK)
endif()

add_executable (spinus_tests
    TestMain.cpp
    acceptance/CoreAcceptanceTests.cpp
    acceptance/PluginsAndAssistantAcceptanceTests.cpp
    acceptance/CreativeAiAcceptanceTests.cpp
    acceptance/AnalysisAcceptanceTests.cpp
    ai/AiTests.cpp
    commands/CommandTests.cpp
    core/ChildProcessTests.cpp
    core/LogTests.cpp
    core/FileDialogTests.cpp
    dsp/DrumMachineTests.cpp
    dsp/SamplerTests.cpp
    dsp/BeatSlicerTests.cpp
    dsp/DuckerTests.cpp
    dsp/DynamicsTests.cpp
    dsp/ParameterTextTests.cpp
    dsp/EqTests.cpp
    dsp/FilterTests.cpp
    dsp/LoudnessMeterTests.cpp
    dsp/StereoUtilityTests.cpp
    dsp/ThreeOscTests.cpp
    dsp/ThreeOscPresetsTests.cpp
    dsp/TuneTests.cpp
    engine/AudioGuardTests.cpp
    engine/AudioSetupTests.cpp
    engine/PipeWireInputTests.cpp
    engine/EngineAdapterTests.cpp
    engine/LauncherTests.cpp
    engine/SandboxEngineTests.cpp
    engine/ExporterTests.cpp
    engine/TuneRenderTests.cpp
    engine/EngineHostTests.cpp
    golden/GoldenAudioTests.cpp
    model/ModelTests.cpp
    plugins/ClapGuiTests.cpp
    plugins/Lv2GuiTests.cpp
    plugins/ClapPluginFormatTests.cpp
    plugins/ClapSidechainTests.cpp
    plugins/SandboxTests.cpp
    plugins/HostLibrariesTests.cpp
    plugins/CrashGuardTests.cpp
    plugins/ArchiveReaderTests.cpp
    plugins/PluginDownloadsTests.cpp
    plugins/WindowsPluginsTests.cpp
    plugins/WindowsReproTests.cpp
    plugins/PluginDialogsTests.cpp
    plugins/SurgePresetsTests.cpp
    plugins/PluginScannerTests.cpp
    project/ProjectFilesTests.cpp
    session/FreezeTests.cpp
    session/IdeaTests.cpp
    session/DemoSongTests.cpp
    session/ProblemReportTests.cpp
    session/ProjectDocumentTests.cpp
    session/ProjectListTests.cpp
    session/MidiRecorderTests.cpp
    session/SessionTests.cpp
    session/TextLocaleTests.cpp
    session/ProgramFolderTests.cpp
    session/SilenceTests.cpp
    session/AudioRecorderTests.cpp
    session/DeviceParametersTests.cpp
    session/DownloadedPluginsTests.cpp
    session/StemSamplesTests.cpp
    session/AnalysisLibraryTests.cpp
    session/SynthMatchTests.cpp
    session/SynthProfileTests.cpp
    session/InstrumentPresetsTests.cpp
    session/PresetPreviewsTests.cpp
    session/SoundDescriptorsTests.cpp
    session/SoundIndexTests.cpp
    session/PartSoundsTests.cpp
    session/SuggestionsTests.cpp
    session/TunerTests.cpp
    ui/AnalysisEngineTests.cpp
    ui/AnalyzerTests.cpp
    ui/TrackCreatorTests.cpp
    ui/ExperimentalFeaturesTests.cpp
    ui/OwnerSongsTests.cpp
    ui/AutomixerTests.cpp
    ui/LauncherPanelTests.cpp
    ui/AssistantTests.cpp
    ui/AudioClipTests.cpp
    ui/AudioTracksTests.cpp
    ui/SamplesBrowserTests.cpp
    ui/AutomationEditorTests.cpp
    ui/AutomationTests.cpp
    ui/BrowserTests.cpp
    ui/ChannelRackTests.cpp
    ui/HistoryPanelTests.cpp
    ui/MixerTests.cpp
    ui/NoteLaneTests.cpp
    ui/PatternGeneratorTests.cpp
    ui/PianoRollTests.cpp
    ui/PlaylistTests.cpp
    ui/TimelineZoomTests.cpp
    ui/RecordIdeaTests.cpp
    ui/PluginWindowTests.cpp
    ui/SpinusTuneTests.cpp
    ui/ProjectMenuTests.cpp
    ui/TypingKeyboardTests.cpp
    ui/UiShellTests.cpp
    ui/ManualScreenshotTests.cpp
    ui/ManualTests.cpp
    ui/ManyInstrumentsTests.cpp
    ui/WindowsPluginsDialogTests.cpp)

if (WIN32)
    # On Windows: the tests that need Linux (the Wine bridge; libarchive to write test archives; signals,
    # pgrep, environment changes, and shell-script fakes of Python and PyTorch) are left out. The rest is the same suite.
    get_target_property (testSources spinus_tests SOURCES)
    list (REMOVE_ITEM testSources
          plugins/WindowsPluginsTests.cpp plugins/WindowsReproTests.cpp ui/WindowsPluginsDialogTests.cpp plugins/ArchiveReaderTests.cpp
          plugins/PluginDownloadsTests.cpp ai/AiTests.cpp plugins/PluginScannerTests.cpp session/SynthMatchTests.cpp
          ui/AnalysisEngineTests.cpp ui/BrowserTests.cpp acceptance/AnalysisAcceptanceTests.cpp ui/AnalyzerTests.cpp
          ui/OwnerSongsTests.cpp ui/ManyInstrumentsTests.cpp ui/TrackCreatorTests.cpp)
    set_target_properties (spinus_tests PROPERTIES SOURCES "${testSources}")
    target_link_libraries (spinus_tests PRIVATE spinus_core spinus_test_support spinus_warnings Catch2::Catch2)
else()
    find_package (LibArchive REQUIRED) # the download tests write archives
    target_link_libraries (spinus_tests PRIVATE spinus_core spinus_test_support spinus_warnings Catch2::Catch2
                                                LibArchive::LibArchive)
endif()
target_compile_definitions (spinus_tests PRIVATE SPINUS_TEST_FILES_DIR="${SPINUS_BUILT_IN_PATH_PREFIX}${CMAKE_CURRENT_SOURCE_DIR}")

if (SPINUS_BUILD_BUNDLED_PLUGINS)
    # Lets tests use the bundled plugins when they have been built (they skip otherwise).
    target_compile_definitions (spinus_tests PRIVATE SPINUS_PLUGINS_DIR="${SPINUS_BUILT_IN_PATH_PREFIX}${SPINUS_PLUGINS_DIR}")
endif()

include (${CMAKE_CURRENT_SOURCE_DIR}/../external/Catch2/extras/Catch.cmake)
if (SPINUS_ENABLE_ASAN)
    # JUCE loads libX11 at run time and unloads it at exit, before LeakSanitizer reports, so leaks in
    # X11's own caches would show as "unknown module" and could not be suppressed. Linking it keeps it
    # loaded, so tests/lsan.supp can name it.
    find_library (SPINUS_X11_LIBRARY X11 REQUIRED)
    target_link_libraries (spinus_tests PRIVATE ${SPINUS_X11_LIBRARY})
    catch_discover_tests (spinus_tests PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
elseif (WIN32)
    # Listed when ctest runs, not after linking: a cross-build cannot run the tests while building.
    catch_discover_tests (spinus_tests DISCOVERY_MODE PRE_TEST)
else()
    catch_discover_tests (spinus_tests)
endif()

#==============================================================================
# pluginval on the bundled plugins (VST3 builds; pluginval cannot load CLAP).
include (${PROJECT_SOURCE_DIR}/cmake/SpinusPluginval.cmake)

if (SPINUS_BUILD_BUNDLED_PLUGINS)
    foreach (plugin IN LISTS SPINUS_DRAGONFLY_PLUGINS)
        spinus_add_pluginval_test (${plugin} "${SPINUS_PLUGINS_DIR}/${plugin}.vst3")
    endforeach()

    spinus_add_pluginval_test (lsp-plugins "${SPINUS_LSP_VST3}")
endif()

if (SPINUS_BUILD_OWN_PLUGINS)
    # The acceptance test loads Spinus's own plugins as an ordinary VST3 and LV2 plugin.
    target_compile_definitions (spinus_tests PRIVATE
        SPINUS_OWN_VST3="${SPINUS_THREE_OSC_VST3}"
        SPINUS_OWN_LV2="${CMAKE_BINARY_DIR}/spinus_drum_machine_plugin_artefacts/$<CONFIG>/LV2/Spinus Drum Machine.lv2")
    add_dependencies (spinus_tests spinus_three_osc_plugin_VST3 spinus_drum_machine_plugin_LV2)

    spinus_add_pluginval_test (spinus-sampler "${SPINUS_SAMPLER_VST3}")
    spinus_add_pluginval_test (spinus-drum-machine "${SPINUS_DRUM_MACHINE_VST3}")
    spinus_add_pluginval_test (spinus-3x-osc "${SPINUS_THREE_OSC_VST3}")
    spinus_add_pluginval_test (spinus-filter "${SPINUS_FILTER_VST3}")
    spinus_add_pluginval_test (spinus-ducker "${SPINUS_DUCKER_VST3}")
    spinus_add_pluginval_test (spinus-stereo-utility "${SPINUS_STEREO_UTILITY_VST3}")
    spinus_add_pluginval_test (spinus-beat-slicer "${SPINUS_BEAT_SLICER_VST3}")
    spinus_add_pluginval_test (spinus-tune "${SPINUS_TUNE_VST3}")
    spinus_add_pluginval_test (spinus-loudness-meter "${SPINUS_LOUDNESS_METER_VST3}")
    spinus_add_pluginval_test (spinus-eq "${SPINUS_EQ_VST3}")
    spinus_add_pluginval_test (spinus-compressor "${SPINUS_COMPRESSOR_VST3}")
    spinus_add_pluginval_test (spinus-limiter "${SPINUS_LIMITER_VST3}")
    spinus_add_pluginval_test (spinus-gate "${SPINUS_GATE_VST3}")
endif()

if (SPINUS_BUILD_SURGE)
    spinus_add_pluginval_test (surge-xt "${SPINUS_SURGE_VST3}")
    spinus_add_pluginval_test (surge-xt-effects "${SPINUS_SURGE_FX_VST3}")
endif()

#==============================================================================
# Broken CLAP plugins for the plugin scanner tests (crash / hang while loading).
set (SPINUS_BROKEN_PLUGINS_DIR "${CMAKE_CURRENT_BINARY_DIR}/broken-plugins")

foreach (behaviour crash hang)
    add_library (spinus_test_${behaviour}_clap MODULE plugins/BrokenClapPlugin.c)
    target_include_directories (spinus_test_${behaviour}_clap PRIVATE ${PROJECT_SOURCE_DIR}/external/clap/include)
    target_compile_definitions (spinus_test_${behaviour}_clap PRIVATE BROKEN_BEHAVIOUR=$<IF:$<STREQUAL:${behaviour},crash>,1,2>)
    set_target_properties (spinus_test_${behaviour}_clap PROPERTIES
        PREFIX "" OUTPUT_NAME "${behaviour}" SUFFIX ".clap"
        LIBRARY_OUTPUT_DIRECTORY "${SPINUS_BROKEN_PLUGINS_DIR}")
    add_dependencies (spinus_tests spinus_test_${behaviour}_clap)
endforeach()

add_library (spinus_test_crash_test_clap MODULE plugins/CrashTestClap.c)
target_include_directories (spinus_test_crash_test_clap PRIVATE ${PROJECT_SOURCE_DIR}/external/clap/include)
set_target_properties (spinus_test_crash_test_clap PROPERTIES
    PREFIX "" OUTPUT_NAME "crash-test" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${SPINUS_BROKEN_PLUGINS_DIR}")
add_dependencies (spinus_tests spinus_test_crash_test_clap)

add_library (spinus_test_sidechain_clap MODULE plugins/SidechainEchoClap.c)
target_include_directories (spinus_test_sidechain_clap PRIVATE ${PROJECT_SOURCE_DIR}/external/clap/include)
set_target_properties (spinus_test_sidechain_clap PROPERTIES
    PREFIX "" OUTPUT_NAME "sidechain-echo" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${SPINUS_BROKEN_PLUGINS_DIR}")
add_dependencies (spinus_tests spinus_test_sidechain_clap)

# An effect whose CLAP parameter IDs are not their positions (DeviceParametersTests.cpp).
add_library (spinus_test_param_ids_clap MODULE plugins/ParamIdsClap.c)
target_include_directories (spinus_test_param_ids_clap PRIVATE ${PROJECT_SOURCE_DIR}/external/clap/include)
set_target_properties (spinus_test_param_ids_clap PROPERTIES
    PREFIX "" OUTPUT_NAME "param-ids" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${SPINUS_BROKEN_PLUGINS_DIR}")
add_dependencies (spinus_tests spinus_test_param_ids_clap)

# Two tiny instruments with named settings, for synth match (one with nothing to tune).
add_library (spinus_test_synth_clap MODULE plugins/TestSynthClap.c)
target_include_directories (spinus_test_synth_clap PRIVATE ${PROJECT_SOURCE_DIR}/external/clap/include)
if (NOT WIN32)
    target_link_libraries (spinus_test_synth_clap PRIVATE m)
endif()
set_target_properties (spinus_test_synth_clap PROPERTIES
    PREFIX "" OUTPUT_NAME "test-synth" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${SPINUS_BROKEN_PLUGINS_DIR}")
add_dependencies (spinus_tests spinus_test_synth_clap)

# The same synth with two presets that crash and hang it, for the sound index's helper (SoundIndexTests.cpp).
add_library (spinus_test_misbehaving_clap MODULE plugins/TestSynthClap.c)
target_include_directories (spinus_test_misbehaving_clap PRIVATE ${PROJECT_SOURCE_DIR}/external/clap/include)
target_compile_definitions (spinus_test_misbehaving_clap PRIVATE MISBEHAVING=1)
if (NOT WIN32)
    target_link_libraries (spinus_test_misbehaving_clap PRIVATE m)
endif()
set_target_properties (spinus_test_misbehaving_clap PROPERTIES
    PREFIX "" OUTPUT_NAME "misbehaving-synth" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${SPINUS_BROKEN_PLUGINS_DIR}")
add_dependencies (spinus_tests spinus_test_misbehaving_clap)

# A small LV2 plugin with X11 UIs that misbehave as real ones do (Lv2UiTestPlugin.c), for Lv2GuiTests.cpp.
if (UNIX AND NOT APPLE)
    set (lv2TestBundle "${SPINUS_BROKEN_PLUGINS_DIR}/spinus-test-lv2-ui.lv2")
    find_package (X11 REQUIRED)
    add_library (spinus_test_lv2_ui MODULE plugins/Lv2UiTestPlugin.c)
    target_include_directories (spinus_test_lv2_ui PRIVATE
        "${PROJECT_SOURCE_DIR}/external/JUCE/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2")
    target_link_libraries (spinus_test_lv2_ui PRIVATE X11::X11)
    set_target_properties (spinus_test_lv2_ui PROPERTIES
        PREFIX "" OUTPUT_NAME "test-ui" LIBRARY_OUTPUT_DIRECTORY "${lv2TestBundle}")
    set (lv2Manifest "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n@prefix ui: <http://lv2plug.in/ns/extensions/ui#> .\n@prefix doap: <http://usefulinc.com/ns/doap#> .\n")
    foreach (kind unsupported null-resize fails instance-access)
        set (required "")
        if (kind STREQUAL "unsupported")
            set (required "lv2:requiredFeature <urn:spinus:test:no-host-has-this> ;")
        elseif (kind STREQUAL "instance-access")
            set (required "lv2:requiredFeature <http://lv2plug.in/ns/ext/instance-access> ;")
        endif()
        string (APPEND lv2Manifest "
<urn:spinus:test:lv2-ui#${kind}> a lv2:Plugin ;
    lv2:binary <test-ui.so> ;
    doap:name \"Spinus LV2 UI test (${kind})\" ;
    ui:ui <urn:spinus:test:lv2-ui#${kind}-ui> ;
    lv2:port [ a lv2:InputPort, lv2:ControlPort ; lv2:index 0 ; lv2:symbol \"gain\" ; lv2:name \"Gain\" ;
               lv2:default 0.5 ; lv2:minimum 0.0 ; lv2:maximum 1.0 ] ,
             [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 1 ; lv2:symbol \"out\" ; lv2:name \"Out\" ] .
<urn:spinus:test:lv2-ui#${kind}-ui> a ui:X11UI ;
    ${required}
    lv2:requiredFeature ui:idleInterface ;
    lv2:extensionData ui:idleInterface, ui:resize ;
    ui:binary <test-ui.so> .
")
    endforeach()
    file (CONFIGURE OUTPUT "${lv2TestBundle}/manifest.ttl" CONTENT "${lv2Manifest}" @ONLY)
    add_dependencies (spinus_tests spinus_test_lv2_ui)
    target_compile_definitions (spinus_tests PRIVATE
        SPINUS_TEST_LV2_UI_BUNDLE="${SPINUS_BUILT_IN_PATH_PREFIX}${lv2TestBundle}")
endif()

add_dependencies (spinus_tests spinus-plugin-scanner spinus-plugin-host)
if (TARGET spinus-plugin-host-system)
    add_dependencies (spinus_tests spinus-plugin-host-system)
    target_compile_definitions (spinus_tests PRIVATE
        SPINUS_PLUGIN_HOST_SYSTEM="${SPINUS_BUILT_IN_PATH_PREFIX}$<TARGET_FILE:spinus-plugin-host-system>")
endif()
target_compile_definitions (spinus_tests PRIVATE
    SPINUS_PLUGIN_HOST="${SPINUS_BUILT_IN_PATH_PREFIX}$<TARGET_FILE:spinus-plugin-host>"
    SPINUS_BROKEN_PLUGINS_DIR="${SPINUS_BUILT_IN_PATH_PREFIX}${SPINUS_BROKEN_PLUGINS_DIR}"
    SPINUS_PLUGIN_SCANNER="${SPINUS_BUILT_IN_PATH_PREFIX}$<TARGET_FILE:spinus-plugin-scanner>")

#==============================================================================
# The AI service (ai/, Python): tests, lint and strict types (AGENTS: pytest, ruff, mypy --strict).
# Needs uv (https://docs.astral.sh/uv/), which makes ai/.venv from ai/pyproject.toml and ai/uv.lock.
find_program (SPINUS_UV uv)
if (SPINUS_UV)
    foreach (check "pytest:pytest;-q" "ruff:ruff;check;." "mypy:mypy;spinus_ai;tests")
        string (FIND "${check}" ":" colon)
        string (SUBSTRING "${check}" 0 ${colon} name)
        math (EXPR start "${colon} + 1")
        string (SUBSTRING "${check}" ${start} -1 command)
        add_test (NAME "ai.${name}" COMMAND "${SPINUS_UV}" run --project "${PROJECT_SOURCE_DIR}/ai" ${command}
                  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/ai")
        set_tests_properties ("ai.${name}" PROPERTIES LABELS ai TIMEOUT 600)
    endforeach()
else()
    message (STATUS "uv not found: the AI service's Python checks are not run")
endif()

#==============================================================================
# Crashes on purpose, for the plugin crash guard's tests.
add_executable (spinus_crash_guard_helper support/CrashGuardHelper.cpp)
target_link_libraries (spinus_crash_guard_helper PRIVATE spinus_core spinus_warnings)
add_dependencies (spinus_tests spinus_crash_guard_helper spinus_test_crash_test_clap)
target_compile_definitions (spinus_tests PRIVATE SPINUS_CRASH_GUARD_HELPER="${SPINUS_BUILT_IN_PATH_PREFIX}$<TARGET_FILE:spinus_crash_guard_helper>")

# The built program checks itself as packages are checked on other systems (spinus --self-test), started
# through a link as installed packages start it (/usr/bin/spinus -> ../lib/spinus/spinus).
if (WIN32)
    add_test (NAME "Self test of the built program"
              COMMAND "$<TARGET_FILE:spinus>" --self-test "${CMAKE_CURRENT_BINARY_DIR}/self-test")
    set_tests_properties ("Self test of the built program" PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION
                                                                      "Spinus works here")
    return()
endif()
file (MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/linked")
file (CREATE_LINK "${CMAKE_BINARY_DIR}/spinus" "${CMAKE_CURRENT_BINARY_DIR}/linked/spinus" SYMBOLIC)
# Through the PATH, as a launcher starts it: the program's own path is then just "spinus".
add_test (NAME "Self test of the built program"
          COMMAND sh -c "cd / && PATH='${CMAKE_CURRENT_BINARY_DIR}/linked':\$PATH spinus --self-test '${CMAKE_CURRENT_BINARY_DIR}/self-test'")
set_tests_properties ("Self test of the built program" PROPERTIES TIMEOUT 300 PASS_REGULAR_EXPRESSION "Spinus works here")

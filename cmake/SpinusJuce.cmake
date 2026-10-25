# spinus_juce: one static library that compiles the JUCE and Tracktion modules once.
#
# JUCE modules are INTERFACE libraries, so every target that links them would
# otherwise recompile all module sources. The app, the tests and (later) the
# render server all link this library instead.

add_library (spinus_juce STATIC)

target_link_libraries (spinus_juce
    PRIVATE
        tracktion::tracktion_core
        tracktion::tracktion_graph
        tracktion::tracktion_engine
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_osc
    PUBLIC
        juce::juce_recommended_config_flags)
# LTO is left off: it multiplies link times during development. Revisit for packaging.

target_compile_definitions (spinus_juce
    PUBLIC
        JUCE_STANDALONE_APPLICATION=1
        # Needed by MessageManager::runDispatchLoopUntil, which tests use to pump
        # messages. App code must still never run modal loops.
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_USE_CURL=0
        # MP3 decoding (JUCE's own decoder): songs dropped into the Analyzer, samples. The MP3 patents
        # have expired; the decoder is part of JUCE (used under AGPLv3).
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_WEB_BROWSER=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        # Linux: ALSA and JACK (PipeWire through either). Windows: WASAPI (exclusive and shared) and
        # DirectSound, and ASIO through the ASIO headers bundled with JUCE (Steinberg, GPL-3.0; THIRD_PARTY.md).
        $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>
        $<$<PLATFORM_ID:Linux>:JUCE_ALSA=1>
        $<$<PLATFORM_ID:Windows>:JUCE_WASAPI=1>
        $<$<PLATFORM_ID:Windows>:JUCE_DIRECTSOUND=1>
        $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>
        JUCE_PLUGINHOST_VST3=1
        # VST2 hosting through the FST header.
        JUCE_PLUGINHOST_VST=1
        JUCE_PLUGINHOST_LV2=1
        # Audio clips' pitch and stretch use Rubber Band (GPL-2.0-or-later, as for Spinus Tune).
        TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=0
        TRACKTION_ENABLE_TIMESTRETCH_RUBBERBAND=1)

# FST stands in for the VST2 SDK headers JUCE's VST2 host expects; only the JUCE module sources see it.
# JUCE sizes VstSpeakerArrangement as if `speakers` held 8 entries inline (it only allocates extra room
# beyond 8), but FST declares a flexible array, so JUCE overflows the heap when it fills in a stereo
# arrangement. MidiKeyName's flexible `keyName` has the same problem: JUCE puts one on the stack for the
# plugin to write a name of up to 64 characters into (and MSVC refuses to build it). Compile against a
# copy of fst.h with `speakers[8]` and the SDK's MidiKeyName layout; the submodule stays untouched.
set (fstGenerated "${CMAKE_BINARY_DIR}/generated/fst")
file (READ "${PROJECT_SOURCE_DIR}/external/fst/fst/fst.h" fstHeader)
string (REPLACE "t_fstSpeakerProperties speakers[];" "t_fstSpeakerProperties speakers[8]; /* Spinus: JUCE layout */"
        fstPatched "${fstHeader}")
string (REPLACE "char keyName[]; /* length: kVstMaxNameLen */"
        "char keyName[64]; t_fstInt32 reserved; t_fstInt32 flags; /* Spinus: the SDK's layout */"
        fstPatched2 "${fstPatched}")

if (fstPatched STREQUAL fstHeader OR fstPatched2 STREQUAL fstPatched)
    message (FATAL_ERROR "external/fst changed: re-check the fst.h fixes in cmake/SpinusJuce.cmake")
endif()
set (fstPatched "${fstPatched2}")

file (CONFIGURE OUTPUT "${fstGenerated}/fst/fst.h" CONTENT "${fstPatched}" @ONLY)
foreach (header aeffect.h aeffectx.h)
    configure_file ("${PROJECT_SOURCE_DIR}/external/fst/fst/${header}" "${fstGenerated}/fst/${header}" COPYONLY)
endforeach()

# JUCE 9's Windows MIDI input joins its block thread in ~InputDevice even when opening the device failed and
# the thread was never started: std::thread::join then throws std::system_error from a destructor and Spinus
# ends. Windows MIDI inputs are exclusive, so a keyboard already open in another program does this (seen
# under Wine, where a fresh setup's MIDI driver is not ready yet). Windows builds compile a copy of the
# module with that join guarded; the submodule stays untouched. Remove once JUCE fixes it.
if (WIN32)
    set (devicesModule "${PROJECT_SOURCE_DIR}/external/JUCE/modules/juce_audio_devices")
    set (devicesGenerated "${CMAKE_BINARY_DIR}/generated/juce_audio_devices")
    file (READ "${devicesModule}/native/juce_Midi_windows.cpp" midiWindows)
    string (REPLACE "                blockQueueThread.join();"
                    "                if (blockQueueThread.joinable()) blockQueueThread.join(); // Spinus: see SpinusJuce.cmake"
            midiWindowsPatched "${midiWindows}")
    file (READ "${devicesModule}/juce_audio_devices.cpp" devicesUnity)
    # The copy includes every other file from the module itself, and the patched MIDI file from here.
    string (REPLACE "#include \"" "#include \"${devicesModule}/" devicesUnityPatched "${devicesUnity}")
    string (REPLACE "#include \"${devicesModule}/native/juce_Midi_windows.cpp\""
                    "#include \"${devicesGenerated}/juce_Midi_windows.cpp\"" devicesUnityPatched "${devicesUnityPatched}")
    get_target_property (devicesSources juce_audio_devices INTERFACE_SOURCES)
    list (FILTER devicesSources EXCLUDE REGEX "juce_audio_devices\\.cpp")
    if (midiWindowsPatched STREQUAL midiWindows OR devicesUnityPatched STREQUAL devicesUnity)
        message (FATAL_ERROR "external/JUCE changed: re-check the Windows MIDI input fix in cmake/SpinusJuce.cmake")
    endif()
    # Written as is (no @VAR@ replacing), and only touched when the text changes, so nothing rebuilds needlessly.
    foreach (name juce_Midi_windows juce_audio_devices)
        set (text "${midiWindowsPatched}")
        if (name STREQUAL "juce_audio_devices")
            set (text "${devicesUnityPatched}")
        endif()
        file (WRITE "${devicesGenerated}/${name}.cpp.new" "${text}")
        configure_file ("${devicesGenerated}/${name}.cpp.new" "${devicesGenerated}/${name}.cpp" COPYONLY)
    endforeach()
    set_target_properties (juce_audio_devices PROPERTIES INTERFACE_SOURCES
                           "${devicesSources};${devicesGenerated}/juce_audio_devices.cpp")
endif()

# JUCE 9's LV2 host opens a plugin's own window (its UI) trusting what the UI's .ttl says:
# - it reads the extension data the .ttl lists without checking that the UI really returned it. DrumGizmo
#   0.9.20 lists ui:resize but returns NULL for it, so Spinus crashed (SIGSEGV) on "Open instrument window".
# - when the UI's instantiate fails (returns NULL), it kept an editor that calls the UI with a NULL handle.
# - a UI asking to be resized while it opens made it read two flags not yet initialised.
# Every build compiles a copy of juce_LV2PluginFormat.cpp with these fixed; when the UI cannot open, the
# plugin has no editor and Spinus shows its generic one. The submodule stays untouched. Should be reported
# to JUCE; remove once JUCE fixes them itself.
set (processorsModule "${PROJECT_SOURCE_DIR}/external/JUCE/modules/juce_audio_processors")
set (processorsGenerated "${CMAKE_BINARY_DIR}/generated/juce_audio_processors")
file (READ "${processorsModule}/format_types/juce_LV2PluginFormat.cpp" lv2Format)
set (lv2FormatPatched "${lv2Format}")
# Replaces `from` with `to` in lv2FormatPatched; fails loudly when JUCE no longer has `from`.
function (spinus_patch_lv2_host from to)
    string (FIND "${lv2FormatPatched}" "${from}" found)
    if (found EQUAL -1)
        message (FATAL_ERROR "external/JUCE changed: re-check the LV2 plugin window fixes in cmake/SpinusJuce.cmake "
                             "(no longer found: ${from})")
    endif()
    string (REPLACE "${from}" "${to}" patched "${lv2FormatPatched}")
    set (lv2FormatPatched "${patched}" PARENT_SCOPE)
endfunction()
# 1. A UI's extension data: NULL means the UI does not have it, whatever its .ttl says.
spinus_patch_lv2_host ([=[
        return descriptor->getExtensionData<Extension> (world, uid);]=] [=[
        // Spinus: some UIs list extension data in their .ttl and return NULL for it (DrumGizmo 0.9.20).
        if (const auto* d = descriptor->get(); d == nullptr || d->extension_data == nullptr || d->extension_data (uid) == nullptr)
            return {};

        return descriptor->getExtensionData<Extension> (world, uid);]=])
# 2. Whether the UI opened: an instance and a window.
spinus_patch_lv2_host ([=[
    LV2UI_Handle getHandle() const noexcept { return instance.get(); }]=] [=[
    LV2UI_Handle getHandle() const noexcept { return instance.get(); }
    bool isOpen() const noexcept { return instance != nullptr && widget != nullptr; } // Spinus]=])
spinus_patch_lv2_host ([=[
    void setUserScaleFactor (float userScale) { userScaleFactor = userScale; }]=] [=[
    void setUserScaleFactor (float userScale) { userScaleFactor = userScale; }
    bool isOpen() const noexcept { return uiInstance != nullptr && uiInstance->instance.isOpen(); } // Spinus]=])
# 3. A view whose UI did not open is dropped at once (nothing ever calls it), and so is an editor without one...
spinus_patch_lv2_host ([=[
        parentHierarchyChanged();
        const auto initialSize = configuredEditor->getBounds();]=] [=[
        if (! configuredEditor->isOpen()) // Spinus: the UI could not open
        {
            configuredEditor = nullptr;
            return;
        }

        parentHierarchyChanged();
        const auto initialSize = configuredEditor->getBounds();]=])
spinus_patch_lv2_host ([=[
    void destroyView()
    {
        configuredEditor = nullptr;
    }]=] [=[
    void destroyView()
    {
        configuredEditor = nullptr;
    }

    bool hasView() const noexcept { return configuredEditor != nullptr; } // Spinus]=])
spinus_patch_lv2_host ([=[
        editorPointer = newEditor.get();]=] [=[
        if (! newEditor->hasView()) // Spinus: no plugin window, so no editor (the host shows its own)
        {
            uiFailedToOpen = true;
            return nullptr;
        }

        editorPointer = newEditor.get();]=])
# 4. A UI may ask to be resized while it opens (DrumGizmo does): the flags that request reads must be set by
#    then, so they come before the UI instance (JUCE read them uninitialised; UndefinedBehaviorSanitizer).
spinus_patch_lv2_host ([=[
    std::unique_ptr<UiInstanceWithSupports> uiInstance;
    OptionalExtension<LV2UI_Resize> resizeClient;
    OptionalExtension<LV2_Options_Interface> optionsInterface;
    bool resizingSelf = false, resizeFromClient = false;]=] [=[
    bool resizingSelf = false, resizeFromClient = false; // Spinus: before uiInstance
    std::unique_ptr<UiInstanceWithSupports> uiInstance;
    OptionalExtension<LV2UI_Resize> resizeClient;
    OptionalExtension<LV2_Options_Interface> optionsInterface;]=])
# 5. ...and from then on the plugin says it has no editor, as hosts expect after a failed createEditor.
spinus_patch_lv2_host ([=[
        return uiDescriptor.get() != nullptr;]=] [=[
        return uiDescriptor.get() != nullptr && ! uiFailedToOpen; // Spinus]=])
spinus_patch_lv2_host ([=[
    TimedCallback changedParameterFlusher;]=] [=[
    TimedCallback changedParameterFlusher;
    bool uiFailedToOpen = false; // Spinus]=])
file (READ "${processorsModule}/juce_audio_processors.cpp" processorsUnity)
# The copy includes every other file from the module itself, and the patched LV2 file from here.
string (REPLACE "#include \"" "#include \"${processorsModule}/" processorsUnityPatched "${processorsUnity}")
string (REPLACE "#include \"${processorsModule}/format_types/juce_LV2PluginFormat.cpp\""
                "#include \"${processorsGenerated}/juce_LV2PluginFormat.cpp\"" processorsUnityPatched
                "${processorsUnityPatched}")
if (processorsUnityPatched STREQUAL processorsUnity OR NOT processorsUnityPatched MATCHES "generated/juce_audio_processors/juce_LV2")
    message (FATAL_ERROR "external/JUCE changed: re-check the LV2 plugin window fixes in cmake/SpinusJuce.cmake")
endif()
get_target_property (processorsSources juce_audio_processors INTERFACE_SOURCES)
list (FILTER processorsSources EXCLUDE REGEX "/juce_audio_processors\\.cpp$")
foreach (name juce_LV2PluginFormat juce_audio_processors)
    set (text "${lv2FormatPatched}")
    if (name STREQUAL "juce_audio_processors")
        set (text "${processorsUnityPatched}")
    endif()
    # Written as is, and only touched when the text changes, so nothing rebuilds needlessly.
    file (WRITE "${processorsGenerated}/${name}.cpp.new" "${text}")
    configure_file ("${processorsGenerated}/${name}.cpp.new" "${processorsGenerated}/${name}.cpp" COPYONLY)
endforeach()
set_target_properties (juce_audio_processors PROPERTIES INTERFACE_SOURCES
                       "${processorsSources};${processorsGenerated}/juce_audio_processors.cpp")

# JUCE 9 on Linux ignores the window manager's close request (the title bar's close button) while a menu is
# open: the open menu is modal and blocks the window. The user clicks the cross and nothing happens. Linux
# builds compile a copy of juce_XWindowSystem_linux.cpp that closes the open menus first and then the window,
# as asked (a real dialog still blocks it). The submodule stays untouched. Remove once JUCE does this itself.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set (guiModule "${PROJECT_SOURCE_DIR}/external/JUCE/modules/juce_gui_basics")
    set (guiGenerated "${CMAKE_BINARY_DIR}/generated/juce_gui_basics")
    file (READ "${guiModule}/native/juce_XWindowSystem_linux.cpp" xWindowSystem)
    set (closeRequest [=[
        else if (atom == atoms.protocolList [XWindowSystemUtilities::Atoms::DELETE_WINDOW])
        {
            if (! peer->getComponent().isCurrentlyBlockedByAnotherModalComponent())
                peer->handleUserClosingWindow();
        }]=])
    set (closeRequestPatched [=[
        else if (atom == atoms.protocolList [XWindowSystemUtilities::Atoms::DELETE_WINDOW])
        {
            // Spinus: an open menu closes first, then the window as asked (see SpinusJuce.cmake).
            if (PopupMenu::dismissAllActiveMenus())
            {
                MessageManager::callAsync ([window = Component::SafePointer<Component> (&peer->getComponent())]
                {
                    if (window != nullptr && ! window->isCurrentlyBlockedByAnotherModalComponent())
                        window->userTriedToCloseWindow();
                });
            }
            else if (! peer->getComponent().isCurrentlyBlockedByAnotherModalComponent())
                peer->handleUserClosingWindow();
        }]=])
    string (REPLACE "${closeRequest}" "${closeRequestPatched}" xWindowSystemPatched "${xWindowSystem}")
    file (READ "${guiModule}/juce_gui_basics.cpp" guiUnity)
    # The copy includes every other file from the module itself, and the patched X11 file from here.
    string (REPLACE "#include \"" "#include \"${guiModule}/" guiUnityPatched "${guiUnity}")
    string (REPLACE "#include \"${guiModule}/native/juce_XWindowSystem_linux.cpp\""
                    "#include \"${guiGenerated}/juce_XWindowSystem_linux.cpp\"" guiUnityPatched "${guiUnityPatched}")
    if (xWindowSystemPatched STREQUAL xWindowSystem OR NOT guiUnityPatched MATCHES "generated/juce_gui_basics/juce_XWindow")
        message (FATAL_ERROR "external/JUCE changed: re-check the window close fix in cmake/SpinusJuce.cmake")
    endif()
    get_target_property (guiSources juce_gui_basics INTERFACE_SOURCES)
    list (FILTER guiSources EXCLUDE REGEX "/juce_gui_basics\\.cpp$")
    foreach (name juce_XWindowSystem_linux juce_gui_basics)
        set (text "${xWindowSystemPatched}")
        if (name STREQUAL "juce_gui_basics")
            set (text "${guiUnityPatched}")
        endif()
        # Written as is, and only touched when the text changes, so nothing rebuilds needlessly.
        file (WRITE "${guiGenerated}/${name}.cpp.new" "${text}")
        configure_file ("${guiGenerated}/${name}.cpp.new" "${guiGenerated}/${name}.cpp" COPYONLY)
    endforeach()
    set_target_properties (juce_gui_basics PROPERTIES INTERFACE_SOURCES
                           "${guiSources};${guiGenerated}/juce_gui_basics.cpp")
endif()

include ("${CMAKE_CURRENT_LIST_DIR}/SpinusRubberBand.cmake")
target_link_libraries (spinus_juce PRIVATE spinus_rubberband)

target_include_directories (spinus_juce PRIVATE
    "${PROJECT_SOURCE_DIR}/src/plugins/vst2/sdk-shim"
    "${fstGenerated}"
    "${spinus_rubberband_tracktion_include}")

# Expose the module include paths and definitions without propagating the
# module sources themselves.
target_include_directories (spinus_juce
    INTERFACE $<TARGET_PROPERTY:spinus_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions (spinus_juce
    INTERFACE $<TARGET_PROPERTY:spinus_juce,COMPILE_DEFINITIONS>)

# GCC's out-of-line atomics (Linux); MSVC's runtime has them built in.
if (NOT MSVC)
    target_link_libraries (spinus_juce PUBLIC atomic)
else()
    # M_PI and friends, and no min/max macros from <windows.h>.
    target_compile_definitions (spinus_juce PUBLIC _USE_MATH_DEFINES NOMINMAX)
endif()

set_target_properties (spinus_juce PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)

# Warning flags for Spinus-owned targets (not for third-party module code).
# Based on juce_recommended_warning_flags (GCC/Clang), minus -Wmissing-field-initializers:
# designated initialisers that omit defaulted members are intentional.
add_library (spinus_warnings INTERFACE)
if (MSVC)
    # cl and clang-cl (where -Wall means every warning there is): level 4, as JUCE recommends.
    target_compile_options (spinus_warnings INTERFACE /W4
        # As on Linux: designated initialisers that omit defaulted members are intentional.
        $<$<CXX_COMPILER_ID:Clang>:-Wno-missing-field-initializers -Wno-missing-designated-field-initializers>)
    if (SPINUS_WARNINGS_AS_ERRORS)
        target_compile_options (spinus_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options (spinus_warnings INTERFACE
        -Wall -Wextra -Wno-missing-field-initializers -Wpedantic -Wstrict-aliasing -Wuninitialized -Wunused-parameter
        -Wsign-compare -Wsign-conversion -Wunreachable-code -Wcast-align -Wno-implicit-fallthrough
        -Wno-ignored-qualifiers -Wno-multichar -Wswitch-enum -Wredundant-decls -Wshadow -Wfloat-equal
        -Woverloaded-virtual -Wreorder -Wzero-as-null-pointer-constant
        $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized -Wno-strict-overflow>)
    if (SPINUS_WARNINGS_AS_ERRORS)
        target_compile_options (spinus_warnings INTERFACE -Werror)
    endif()
endif()

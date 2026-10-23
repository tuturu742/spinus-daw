# Spinus's own devices as standalone plugins: Spinus hosts the same processors through
# its internal "Spinus" format; these builds are for pluginval and for other DAWs.
#
# They compile the JUCE modules themselves with plugin settings (spinus_juce is built for an app).

option (SPINUS_BUILD_OWN_PLUGINS "Build Spinus's own devices as VST3/LV2 plugins (for pluginval)" ON)

if (NOT SPINUS_BUILD_OWN_PLUGINS)
    return()
endif()

# spinus_add_own_plugin (<target> <product name> <plugin code> <VST3 categories> <LV2 path> [EFFECT] [MIDI_INPUT]
#                        SOURCES ...)
# MIDI_INPUT: an effect that also takes MIDI notes (instruments always do).
function (spinus_add_own_plugin target productName code categories lv2Path)
    cmake_parse_arguments (arg "EFFECT;MIDI_INPUT" "" "SOURCES" ${ARGN})

    if (arg_EFFECT)
        set (isSynth FALSE)
    else()
        set (isSynth TRUE)
    endif()

    if (isSynth OR arg_MIDI_INPUT)
        set (needsMidi TRUE)
    else()
        set (needsMidi FALSE)
    endif()

    juce_add_plugin (${target}
        PRODUCT_NAME "${productName}"
        COMPANY_NAME "Spinus"
        COMPANY_WEBSITE "https://github.com/tuturu742/spinus-daw"
        PLUGIN_MANUFACTURER_CODE Spns
        PLUGIN_CODE ${code}
        FORMATS VST3 LV2
        IS_SYNTH ${isSynth}
        NEEDS_MIDI_INPUT ${needsMidi}
        VST3_CATEGORIES ${categories}
        LV2URI "https://github.com/tuturu742/spinus-daw/plugins/${lv2Path}"
        COPY_PLUGIN_AFTER_BUILD FALSE)

    list (TRANSFORM arg_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/src/dsp/")
    target_sources (${target} PRIVATE ${arg_SOURCES} "${PROJECT_SOURCE_DIR}/src/dsp/DeviceControls.cpp"
                                      "${PROJECT_SOURCE_DIR}/src/core/FileDialog.cpp")
    target_include_directories (${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    # A plugin's file dialogs are JUCE's own browser (core/FileDialog.h): no waiting thread that could
    # outlive the plugin's library in another program.
    target_compile_definitions (${target} PRIVATE SPINUS_FILE_DIALOG_OWN_BROWSER_ONLY=1)

    target_compile_definitions (${target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    # No juce_audio_utils: a plugin does not open audio devices (and so does not need ALSA or JACK).
    target_link_libraries (${target}
        PRIVATE
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags)

    # pluginval and other hosts are not built with AddressSanitizer, so the plugin must not be either.
    if (SPINUS_ENABLE_ASAN)
        foreach (t ${target} ${target}_VST3 ${target}_LV2)
            if (TARGET ${t})
                target_compile_options (${t} PRIVATE -fno-sanitize=all)
                target_link_options (${t} PRIVATE -fno-sanitize=all)
            endif()
        endforeach()
    endif()
endfunction()

spinus_add_own_plugin (spinus_sampler_plugin "Spinus Sampler" Smpl "Instrument;Sampler" sampler
    SOURCES sampler/SampleSet.cpp sampler/SampleStorage.cpp sampler/SamplerEditor.cpp sampler/SamplerEngine.cpp
            sampler/SamplerPluginEntry.cpp sampler/SamplerProcessor.cpp)

spinus_add_own_plugin (spinus_drum_machine_plugin "Spinus Drum Machine" Drum "Instrument;Drum" drum-machine
    SOURCES drums/DrumMachineEditor.cpp drums/DrumMachinePluginEntry.cpp drums/DrumMachineProcessor.cpp
            drums/DrumSynth.cpp sampler/SampleSet.cpp sampler/SampleStorage.cpp sampler/SamplerEngine.cpp)

spinus_add_own_plugin (spinus_three_osc_plugin "Spinus 3x Osc" Tosc "Instrument;Synth" 3x-osc
    SOURCES threeosc/ThreeOscEditor.cpp threeosc/ThreeOscEngine.cpp threeosc/ThreeOscPluginEntry.cpp
            threeosc/ThreeOscPresets.cpp threeosc/ThreeOscProcessor.cpp)

spinus_add_own_plugin (spinus_filter_plugin "Spinus Filter" Fltr "Fx;Filter" filter EFFECT
    SOURCES filter/FilterEditor.cpp filter/FilterEngine.cpp filter/FilterPluginEntry.cpp filter/FilterProcessor.cpp)

spinus_add_own_plugin (spinus_ducker_plugin "Spinus Ducker" Duck "Fx;Dynamics" ducker EFFECT
    SOURCES ducker/DuckerEditor.cpp ducker/DuckerEngine.cpp ducker/DuckerPluginEntry.cpp ducker/DuckerProcessor.cpp)

spinus_add_own_plugin (spinus_stereo_utility_plugin "Spinus Stereo Utility" Stut "Fx;Tools" stereo-utility EFFECT
    SOURCES utility/StereoUtilityEditor.cpp utility/StereoUtilityEngine.cpp utility/StereoUtilityPluginEntry.cpp
            utility/StereoUtilityProcessor.cpp)

spinus_add_own_plugin (spinus_beat_slicer_plugin "Spinus Beat Slicer" Slic "Instrument;Sampler" beat-slicer
    SOURCES slicer/BeatSlicerEditor.cpp slicer/BeatSlicerPluginEntry.cpp slicer/BeatSlicerProcessor.cpp
            slicer/Slicing.cpp sampler/SampleSet.cpp sampler/SampleStorage.cpp sampler/SamplerEngine.cpp)

spinus_add_own_plugin (spinus_tune_plugin "Spinus Tune" Tune "Fx;Pitch Shift" tune EFFECT MIDI_INPUT
    SOURCES tune/PitchDetector.cpp tune/TuneEditor.cpp tune/TuneEngine.cpp tune/TunePluginEntry.cpp
            tune/TuneProcessor.cpp)

spinus_add_own_plugin (spinus_loudness_meter_plugin "Spinus Loudness Meter" Lufs "Fx;Analyzer" loudness-meter EFFECT
    SOURCES loudness/LoudnessEngine.cpp loudness/LoudnessMeterEditor.cpp loudness/LoudnessMeterPluginEntry.cpp
            loudness/LoudnessMeterProcessor.cpp)

set (dynamicsSources dynamics/CompressorEngine.cpp dynamics/DynamicsEditors.cpp dynamics/DynamicsProcessors.cpp
                     dynamics/GateEngine.cpp dynamics/LimiterEngine.cpp)

spinus_add_own_plugin (spinus_eq_plugin "Spinus EQ" Speq "Fx;EQ" eq EFFECT
    SOURCES eq/EqEditor.cpp eq/EqEngine.cpp eq/EqPluginEntry.cpp eq/EqProcessor.cpp)

spinus_add_own_plugin (spinus_compressor_plugin "Spinus Compressor" Comp "Fx;Dynamics" compressor EFFECT
    SOURCES ${dynamicsSources} dynamics/CompressorPluginEntry.cpp)

spinus_add_own_plugin (spinus_limiter_plugin "Spinus Limiter" Limt "Fx;Dynamics" limiter EFFECT
    SOURCES ${dynamicsSources} dynamics/LimiterPluginEntry.cpp)

spinus_add_own_plugin (spinus_gate_plugin "Spinus Gate" Gate "Fx;Dynamics" gate EFFECT
    SOURCES ${dynamicsSources} dynamics/GatePluginEntry.cpp)

set (SPINUS_EQ_VST3 "${CMAKE_BINARY_DIR}/spinus_eq_plugin_artefacts/$<CONFIG>/VST3/Spinus EQ.vst3")
set (SPINUS_COMPRESSOR_VST3
     "${CMAKE_BINARY_DIR}/spinus_compressor_plugin_artefacts/$<CONFIG>/VST3/Spinus Compressor.vst3")
set (SPINUS_LIMITER_VST3 "${CMAKE_BINARY_DIR}/spinus_limiter_plugin_artefacts/$<CONFIG>/VST3/Spinus Limiter.vst3")
set (SPINUS_GATE_VST3 "${CMAKE_BINARY_DIR}/spinus_gate_plugin_artefacts/$<CONFIG>/VST3/Spinus Gate.vst3")
set (SPINUS_LOUDNESS_METER_VST3
     "${CMAKE_BINARY_DIR}/spinus_loudness_meter_plugin_artefacts/$<CONFIG>/VST3/Spinus Loudness Meter.vst3")
set (SPINUS_TUNE_VST3 "${CMAKE_BINARY_DIR}/spinus_tune_plugin_artefacts/$<CONFIG>/VST3/Spinus Tune.vst3")
set (SPINUS_SAMPLER_VST3 "${CMAKE_BINARY_DIR}/spinus_sampler_plugin_artefacts/$<CONFIG>/VST3/Spinus Sampler.vst3")
set (SPINUS_DRUM_MACHINE_VST3
     "${CMAKE_BINARY_DIR}/spinus_drum_machine_plugin_artefacts/$<CONFIG>/VST3/Spinus Drum Machine.vst3")
set (SPINUS_DUCKER_VST3 "${CMAKE_BINARY_DIR}/spinus_ducker_plugin_artefacts/$<CONFIG>/VST3/Spinus Ducker.vst3")
set (SPINUS_STEREO_UTILITY_VST3
     "${CMAKE_BINARY_DIR}/spinus_stereo_utility_plugin_artefacts/$<CONFIG>/VST3/Spinus Stereo Utility.vst3")
set (SPINUS_BEAT_SLICER_VST3
     "${CMAKE_BINARY_DIR}/spinus_beat_slicer_plugin_artefacts/$<CONFIG>/VST3/Spinus Beat Slicer.vst3")
set (SPINUS_FILTER_VST3 "${CMAKE_BINARY_DIR}/spinus_filter_plugin_artefacts/$<CONFIG>/VST3/Spinus Filter.vst3")
set (SPINUS_THREE_OSC_VST3 "${CMAKE_BINARY_DIR}/spinus_three_osc_plugin_artefacts/$<CONFIG>/VST3/Spinus 3x Osc.vst3")

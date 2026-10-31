# `cmake --install` layout, used by packaging/arch/PKGBUILD:
#
#   <prefix>/bin/spinus                       -> ../lib/spinus/spinus
#   <prefix>/lib/spinus/spinus, spinus-plugin-scanner, spinus-plugin-host, spinus-plugin-host-system
#   <prefix>/lib/spinus/ai/                   the AI service (Python) with the manual it searches;
#                                             Help › Manual reads the same copy (ui/Manual), also with AI off
#   <prefix>/lib/vst3, <prefix>/lib/lv2       Spinus's own devices as plugins, for other programs
#   <prefix>/share/applications, mime, icons  desktop entry, .spn file type, icon
#   <prefix>/share/metainfo                   the software centre listing (AppStream)
#   <prefix>/share/spinus/demo/<Name>         the demo songs (CC BY 4.0), read-only: the Browser's Projects
#                                             copy one to the Music folder to open it (src/session/DemoSong.h)
#   <prefix>/share/spinus/samples/Spinus Starter Pack   the starter sample pack (Signature Sounds, CC0 1.0
#                                             and the like) with the Drum Machine's factory kits, read-only
#                                             (src/dsp/drums/StarterPack.h)
#
# The free open-source plugins (Surge XT, LSP Plugins, Dragonfly Reverb) are not installed: Spinus
# downloads them from their projects on request, or uses the system's copies.

include (GNUInstallDirs)

set (spinusLibDir "${CMAKE_INSTALL_LIBDIR}/spinus")

install (TARGETS spinus spinus-plugin-scanner spinus-plugin-host RUNTIME DESTINATION "${spinusLibDir}")
if (TARGET spinus-plugin-host-system)
    install (TARGETS spinus-plugin-host-system RUNTIME DESTINATION "${spinusLibDir}")
endif()
install (CODE "
    file (MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    file (CREATE_LINK \"../${spinusLibDir}/spinus\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/spinus\" SYMBOLIC)
")

if (SPINUS_BUILD_OWN_PLUGINS)
    foreach (plugin "Spinus Sampler:spinus_sampler_plugin" "Spinus Drum Machine:spinus_drum_machine_plugin"
                    "Spinus 3x Osc:spinus_three_osc_plugin" "Spinus Filter:spinus_filter_plugin"
                    "Spinus Ducker:spinus_ducker_plugin" "Spinus Stereo Utility:spinus_stereo_utility_plugin"
                    "Spinus Beat Slicer:spinus_beat_slicer_plugin" "Spinus Tune:spinus_tune_plugin"
                    "Spinus Loudness Meter:spinus_loudness_meter_plugin" "Spinus EQ:spinus_eq_plugin"
                    "Spinus Compressor:spinus_compressor_plugin" "Spinus Limiter:spinus_limiter_plugin"
                    "Spinus Gate:spinus_gate_plugin" "Spinus Delay:spinus_delay_plugin")
        string (REPLACE ":" ";" parts "${plugin}")
        list (GET parts 0 name)
        list (GET parts 1 target)
        set (artefacts "${CMAKE_BINARY_DIR}/${target}_artefacts/${CMAKE_BUILD_TYPE}")
        install (DIRECTORY "${artefacts}/VST3/${name}.vst3" DESTINATION "${CMAKE_INSTALL_LIBDIR}/vst3")
        install (DIRECTORY "${artefacts}/LV2/${name}.lv2" DESTINATION "${CMAKE_INSTALL_LIBDIR}/lv2")
    endforeach()
endif()

install (FILES "${PROJECT_SOURCE_DIR}/packaging/share/spinus.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install (FILES "${PROJECT_SOURCE_DIR}/packaging/share/spinus.xml" DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")
install (FILES "${PROJECT_SOURCE_DIR}/packaging/share/io.github.tuturu742.spinus.metainfo.xml"
         DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install (FILES "${PROJECT_SOURCE_DIR}/packaging/share/spinus.svg"
         DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

# The demo songs: every project folder in demo/ (demo/README.md, which is not installed).
install (DIRECTORY "${PROJECT_SOURCE_DIR}/demo/" DESTINATION "${CMAKE_INSTALL_DATADIR}/spinus/demo"
         PATTERN "README.md" EXCLUDE PATTERN ".autosave" EXCLUDE PATTERN ".session.lock" EXCLUDE)

# The starter sample pack (tools/starter-pack makes it).
install (DIRECTORY "${PROJECT_SOURCE_DIR}/samples/" DESTINATION "${CMAKE_INSTALL_DATADIR}/spinus/samples")

# The AI service: run by the system's Python (python-httpx, python-keyring), only when AI is turned on.
install (DIRECTORY "${PROJECT_SOURCE_DIR}/ai/spinus_ai" DESTINATION "${spinusLibDir}/ai" PATTERN "__pycache__" EXCLUDE)
install (FILES "${PROJECT_SOURCE_DIR}/ai/run_spinus_ai.py" DESTINATION "${spinusLibDir}/ai")
install (DIRECTORY "${PROJECT_SOURCE_DIR}/docs/user" DESTINATION "${spinusLibDir}/ai/docs")
install (FILES "${PROJECT_SOURCE_DIR}/docs/dev/commands.schema.json" DESTINATION "${spinusLibDir}/ai")

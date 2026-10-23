# pluginval (GPL-3.0, a test tool only; not shipped) validates plugin binaries against the
# VST3 API: loading, processing at several sample rates and block sizes, state, automation and
# buses. AGENTS.md requires it for stock plugins; Spinus also runs it on the bundled plugins.
#
# spinus_add_pluginval_test (<name> <path-to-plugin.vst3> [STRICTNESS <n>]) adds a CTest test
# labelled "pluginval".

set (SPINUS_PLUGINVAL_VERSION 1.0.4)
set (pluginvalDir "${CMAKE_BINARY_DIR}/pluginval-${SPINUS_PLUGINVAL_VERSION}")
set (SPINUS_PLUGINVAL "${pluginvalDir}/pluginval")

if (NOT EXISTS "${SPINUS_PLUGINVAL}")
    set (pluginvalZip "${pluginvalDir}/pluginval_Linux.zip")
    file (DOWNLOAD
        "https://github.com/Tracktion/pluginval/releases/download/v${SPINUS_PLUGINVAL_VERSION}/pluginval_Linux.zip"
        "${pluginvalZip}"
        EXPECTED_HASH SHA256=c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352
        STATUS pluginvalStatus)
    list (GET pluginvalStatus 0 pluginvalError)

    if (pluginvalError)
        message (WARNING "Could not download pluginval (${pluginvalStatus}); pluginval tests are skipped")
        set (SPINUS_PLUGINVAL "")
    else()
        file (ARCHIVE_EXTRACT INPUT "${pluginvalZip}" DESTINATION "${pluginvalDir}")
        file (CHMOD "${SPINUS_PLUGINVAL}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                                                       WORLD_READ WORLD_EXECUTE)
    endif()
endif()

function (spinus_add_pluginval_test name pluginPath)
    if (NOT SPINUS_PLUGINVAL)
        return()
    endif()

    cmake_parse_arguments (arg "" "STRICTNESS" "" ${ARGN})

    if (NOT arg_STRICTNESS)
        set (arg_STRICTNESS 5)
    endif()

    # A fixed seed makes failures reproducible; GUI tests need a display and are covered elsewhere.
    add_test (NAME "pluginval.${name}"
              COMMAND "${SPINUS_PLUGINVAL}" --strictness-level ${arg_STRICTNESS} --random-seed 0x5bd1
                      --skip-gui-tests --timeout-ms 120000 --validate "${pluginPath}")
    set_tests_properties ("pluginval.${name}" PROPERTIES LABELS pluginval TIMEOUT 1800)
endfunction()

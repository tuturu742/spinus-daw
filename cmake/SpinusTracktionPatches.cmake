# Fixes Spinus needs in Tracktion Engine that aren't in the pinned commit, applied to the submodule's files when
# CMake configures (every build: CI, Windows, Flatpak, AppImage). Each fix replaces one exact piece of text: it is
# skipped when it is already there, and configuring stops if neither the old nor the new text is found, so a
# Tracktion update can't drop a fix silently (see THIRD_PARTY.md). The submodule shows as modified afterwards.
# Builds from a read-only source tree (the AppImage's) run it first with `cmake -P cmake/SpinusTracktionPatches.cmake`.

function (spinus_patch_file file old new)
    set (path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../external/tracktion_engine/modules/tracktion_engine/${file}")
    file (READ "${path}" text)
    string (FIND "${text}" "${new}" done)
    if (NOT done EQUAL -1)
        return()
    endif()
    string (FIND "${text}" "${old}" at)
    if (at EQUAL -1)
        message (FATAL_ERROR "Tracktion patch: ${file} doesn't have the text to replace (has Tracktion changed?)")
    endif()
    string (REPLACE "${old}" "${new}" text "${text}")
    file (WRITE "${path}" "${text}")
    message (STATUS "Tracktion patch applied: ${file}")
endfunction()

# A render job (a reversed clip's copy, for one) cancelled while the thread pool still held it, waiting to run or
# between two of its runs, was deleted under the pool, which then read it: a rare crash after reversing a clip.
# BackgroundJobManager::removeJob now says whether the job is out of the pool, and a finished job waits until it is.
spinus_patch_file ("utilities/tracktion_BackgroundJobs.h"
    "    void removeJob (ThreadPoolJobWithProgress* job, bool interruptIfRunning, int timeOutMilliseconds)
    {
        pool.removeJob (job, interruptIfRunning, timeOutMilliseconds);
    }"
    "    /** Returns true once the job is out of the pool: removed if it was waiting, or finished running. */
    bool removeJob (ThreadPoolJobWithProgress* job, bool interruptIfRunning, int timeOutMilliseconds)
    {
        return pool.removeJob (job, interruptIfRunning, timeOutMilliseconds);
    }")
spinus_patch_file ("model/export/tracktion_RenderManager.cpp"
    "        case UpdateMessage::cancelled:
            if (isRunning())"
    "        case UpdateMessage::cancelled:
            // The pool may still hold the job: waiting to run (cancelled before it started, or between two of its
            // runs) or running. Take it out of the queue, or wait for it while it runs, or the pool would read it
            // after it's deleted.
            if (! engine.getBackgroundJobs().removeJob (this, false, 0) || isRunning())")

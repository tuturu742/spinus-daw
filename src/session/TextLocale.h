#pragma once

namespace spinus::session
{

/** Makes sure the programs Spinus starts (the file chooser, ffmpeg, the AI service) handle file names as UTF-8.
    When the system's language setting isn't installed where Spinus runs (often inside the Flatpak, whose
    runtime has few), they fall back to plain ASCII and turn names like "Рассвет.opus" into "???????.opus".
    Then LC_ALL is set to C.UTF-8, which every system has. Spinus's own locale stays "C". Linux only; call
    once at startup, before any other thread. Returns whether it changed anything. */
bool ensureUtf8ForChildProcesses();

} // namespace spinus::session

#pragma once

#include <juce_core/juce_core.h>

namespace tracktion
{
inline namespace engine
{
class Engine;
}
} // namespace tracktion

namespace spinus
{

/** `spinus --self-test <folder>`: checks, without an audio device, that this build of
    Spinus can make a song, save it, open it again and export it with sound, that its helper programs
    start, that the AI service's Python has what it needs and that the demo songs are there. For checking
    packages on other systems.

    Writes a line per check through `say` ("ok: ...", "FAILED: ...", or "skipped: ..." for the AI service
    when no Python for it is installed: AI is optional); true when no check failed.
    Message thread only; it waits for the export. */
bool runSelfTest (tracktion::Engine&, const juce::File& folder, const std::function<void (const juce::String&)>& say);

} // namespace spinus

#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <map>

namespace spinus
{
class Session;
class ProjectDocument;
} // namespace spinus

/** Spinus from the command line, for scripts and AI agents (docs/user/command-line.md): no window, no audio
    device. The app's `--run`, `--check`, `--render`, `--describe` and `--commands` call these.

    A script is a JSON list of steps, each either a command, exactly as the CommandRegistry and
    commands.schema.json take it, or one of the few actions that change a device's sounds rather than the project:

        {"command": "channel.add", "params": {...}, "as": "bass"}
        {"action": "loadKit", "params": {"device": "$drums.device", "kit": "Magpie"}}
        {"action": "loadSamples", "params": {"device": "$keys.device", "files": ["Rhodes C4.wav"]}}

    Each command is one undo step, as a user's edit is. `as` names the step's result: a string parameter
    "$bass.channel" is that result's field, "$bass" a result that is not an object; "$3.channel" is the third
    step's. Message thread only. */
namespace spinus::cli
{

using Say = std::function<void (const juce::String&)>;

/** Replaces references ("$label.field", "$label", "$<step>.field") in strings anywhere in `value` with the
    earlier results they name. A reference that names nothing sets `error` (the first one) and stays as it is. */
juce::var resolveReferences (const juce::var& value, const std::map<juce::String, juce::var>& results,
                             juce::String& error);

/** Steps ([{command, parameters or params}]) with their automation points' `text` ("-12 dB", "30% L", "1.2 kHz")
    read as values for each clip's target, as --run reads them; the steps as they were when none has text. A text
    that can't be read sets `error` (the first one). A target made by an earlier step of the same list ("$3.device")
    can't be read yet: its points stay as they are, and the command says what is wrong. */
juce::var readStepTexts (Session&, const juce::var& steps, juce::String& error);

struct RunOptions
{
    juce::File scriptFolder; // relative file paths in parameters are looked for here first
    bool keepGoing = false;  // run the remaining steps after a failed one
};

/** Runs the steps on the session's project; `say` gets one JSON line per step ({step, command or action, ok,
    result or error}). 0 when every step worked, 2 when one failed, 1 when `steps` is not a list of steps. */
int runSteps (Session&, const juce::var& steps, const RunOptions&, const Say& say);

/** What the open project holds, for --check: tempo, key, length, channels, inserts (effects, sends,
    sidechains), patterns, lanes, clips, markers, and what is missing here (devices, audio files) under
    `problems`. `projectFolder` is where its audio/ folder is (none for an unsaved project). */
juce::var summarize (Session&, const juce::File& projectFolder);

/** A device for --describe: its parameters (ID, name, the value it shows at 0, 0.25, 0.5, 0.75 and 1, and for
    a choice or switch every value by name, `choices`), its
    presets (3x Osc), its pads and kits (Drum Machine). Made on the session's project for a moment and removed
    again (undone). An object with `error` when the type is unknown or its plugin is not installed. */
juce::var describe (Session&, const juce::String& stockType);

/** The stock types --describe all covers: Spinus's own devices and the bundled ones installed here. */
juce::StringArray describableTypes (Session&);

/** The bundled (free) devices that are not installed here, with how to get them: --describe all's last line. */
juce::var notInstalled (Session&);

/** The command-line usage, one option per line, for --help. */
juce::String usage();

/** The first argument that looks like an option ("--x", "-h") but is not one of Spinus's; empty when there is
    none. Values after an option (a file, a number) are not options. */
juce::String unknownOption (const juce::StringArray& args);

/** A rendered file's length, integrated loudness (LUFS) and true peak (dBTP): WAV or FLAC. */
juce::var measureFile (const juce::File&);

/** Renders the open project (the song, as File > Export does) to `out` (.wav, .flac, .opus or .mp3 by its
    extension), and one file per insert into `stems` when that is set; says a JSON line with what it measured.
    0 on success. */
int render (Session&, const juce::File& audioFolder, const juce::File& out, const juce::File& stems, double tailSeconds,
            const Say& say);

} // namespace spinus::cli

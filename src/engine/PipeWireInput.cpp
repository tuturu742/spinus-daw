#include "PipeWireInput.h"
#include "core/ChildProcess.h"

#if JUCE_WINDOWS
#include <process.h> // _getpid; PipeWire is Linux-only, so nothing is ever routed there
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace spinus
{

namespace
{
constexpr const char* settingName = "pipeWireInput";

juce::String runProgram (const juce::StringArray& command, bool& ok)
{
    core::ChildProcess process;
    ok = process.start (command, core::ChildProcess::wantStdOut);
    if (! ok)
        return {};
    auto output = process.readAllProcessOutput(); // pw-dump prints more than a pipe holds: read as it runs
    ok = process.waitForProcessToFinish (3000) && process.getExitCode() == 0;
    return output;
}

const juce::var& propsOf (const juce::var& object)
{
    static const juce::var none;
    const auto& info = object["info"];
    return info.isObject() ? info["props"] : none;
}
} // namespace

PipeWireInput::PipeWireInput (juce::AudioDeviceManager& m, juce::PropertiesFile* s, Runner runner)
    : manager (m), settings (s), run (runner ? std::move (runner) : Runner (runProgram))
{
    if (settings != nullptr)
        chosen = settings->getValue (settingName);
    manager.addChangeListener (this);
    if (chosen.isNotEmpty())
        startTimer (300);
}

PipeWireInput::~PipeWireInput() { manager.removeChangeListener (this); }

bool PipeWireInput::isPipeWireDevice (const juce::String& name)
{
    return name.containsIgnoreCase ("PipeWire") || name.containsIgnoreCase ("PulseAudio") || name == "default" ||
           name == "pipewire" || name == "pulse";
}

std::vector<PipeWireInput::Source> PipeWireInput::parseSources (const juce::var& dump)
{
    std::vector<Source> sources;
    if (auto* objects = dump.getArray())
        for (const auto& object : *objects)
        {
            const auto& props = propsOf (object);
            if (props["media.class"].toString() != "Audio/Source")
                continue;
            const auto name = props["node.name"].toString();
            auto description = props["node.description"].toString();
            if (description.isEmpty())
                description = props["node.nick"].toString();
            if (name.isNotEmpty())
                sources.push_back ({ name, description.isNotEmpty() ? description : name });
        }
    return sources;
}

int PipeWireInput::findCaptureStream (const juce::var& dump, int processId)
{
    if (auto* objects = dump.getArray())
        for (const auto& object : *objects)
        {
            const auto& props = propsOf (object);
            if (props["media.class"].toString() == "Stream/Input/Audio" &&
                props["application.process.id"].toString().getIntValue() == processId)
                return (int)object["id"];
        }
    return -1;
}

juce::var PipeWireInput::dump() const
{
    bool ok = false;
    const auto text = run ({ "pw-dump" }, ok);
    return ok ? juce::JSON::parse (text) : juce::var();
}

std::vector<PipeWireInput::Source> PipeWireInput::listSources() const { return parseSources (dump()); }

void PipeWireInput::choose (const juce::String& sourceName)
{
    chosen = sourceName;
    if (settings != nullptr)
    {
        settings->setValue (settingName, chosen);
        settings->saveIfNeeded();
    }
    attempts = 0;
    if (! routeNow())
        startTimer (300);
}

bool PipeWireInput::recordsThroughPipeWire() const
{
    const auto setup = manager.getAudioDeviceSetup();
    return manager.getCurrentAudioDevice() != nullptr && ! setup.inputChannels.isZero() &&
           isPipeWireDevice (setup.inputDeviceName);
}

bool PipeWireInput::routeNow() { return ! recordsThroughPipeWire() || connect ((int)getpid()); }

bool PipeWireInput::connect (int processId)
{
    const auto stream = findCaptureStream (dump(), processId);
    if (stream < 0)
        return false; // not there yet: the device is still opening
    bool ok = false;
    if (chosen.isEmpty())
        run ({ "pw-metadata", "-d", juce::String (stream), "target.object" }, ok);
    else
        run ({ "pw-metadata", juce::String (stream), "target.object", chosen }, ok);
    return ok;
}

void PipeWireInput::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The device (re)opened or changed: its recording stream is new.
    attempts = 0;
    if (chosen.isNotEmpty())
        startTimer (300);
}

void PipeWireInput::timerCallback()
{
    if (routeNow() || ++attempts >= 20)
        stopTimer();
}

} // namespace spinus

#include "SlicesToPattern.h"

#include "commands/CommandRegistry.h"
#include "dsp/slicer/BeatSlicerProcessor.h"
#include "model/ModelJson.h"

namespace spinus
{

namespace te = tracktion;

namespace
{
dsp::BeatSlicerProcessor* slicerOf (Session& session, model::ChannelId channel)
{
    const auto* c = session.getProject().findChannel (channel);
    if (c == nullptr || ! c->instrument)
        return nullptr;
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (c->instrument->id));
    return plugin != nullptr ? dynamic_cast<dsp::BeatSlicerProcessor*> (plugin->getAudioPluginInstance()) : nullptr;
}

std::unique_ptr<commands::Command> make (const char* name, const juce::var& parameters, juce::String& error)
{
    return commands::CommandRegistry::get().create (name, parameters, error);
}
} // namespace

bool hasSlices (Session& session, model::ChannelId channel)
{
    const auto* slicer = slicerOf (session, channel);
    return slicer != nullptr && slicer->hasLoop();
}

juce::Result slicesToPattern (Session& session, model::ChannelId channel)
{
    const auto* slicer = slicerOf (session, channel);
    if (slicer == nullptr)
        return juce::Result::fail ("This channel's instrument is not a Beat Slicer.");
    if (! slicer->hasLoop())
        return juce::Result::fail ("Load a loop into the Beat Slicer first.");

    const auto& project = session.getProject();
    const auto& sig = project.settings.timeSignature;
    const auto bar = model::ticksPerQuarterNote * 4 * sig.numerator / juce::jmax (1, sig.denominator);
    const auto loopTicks = (model::Ticks)std::llround (slicer->getLoopBeats() * model::ticksPerQuarterNote);

    juce::Array<juce::var> notes;
    for (auto& slice : slicer->getSliceNotes())
    {
        model::Note n;
        n.start = (model::Ticks)std::llround (slice.startBeats * model::ticksPerQuarterNote);
        n.length =
            juce::jmax<model::Ticks> (1, (model::Ticks)std::llround (slice.lengthBeats * model::ticksPerQuarterNote));
        n.pitch = slice.note;
        n.velocity = 0.8f;
        notes.add (model::noteToJson (n));
    }

    auto setNotes = [&] (model::PatternId pattern)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("pattern", (int)pattern.value);
        o->setProperty ("channel", (int)channel.value);
        o->setProperty ("notes", notes);
        o->setProperty ("description", "Slices to pattern");
        return juce::var (o);
    };

    juce::String error;
    const auto* current = project.findPattern (session.getCurrentPattern());
    if (current != nullptr && current->length >= loopTicks)
    {
        auto command = make ("pattern.setNotes", setNotes (current->id), error);
        if (command == nullptr)
            return juce::Result::fail (error);
        return session.getHistory().perform (std::move (command));
    }

    // A new pattern, whole bars long. IDs are given out in order from nextId, so the new pattern's ID
    // is known before the transaction runs; the transaction fails as a whole if that ever changes.
    const auto newPattern = model::PatternId { project.nextId };
    auto* add = new juce::DynamicObject();
    add->setProperty ("name", slicer->getLoop().name.isNotEmpty() ? slicer->getLoop().name : juce::String ("Slices"));
    add->setProperty ("length", (int)(((loopTicks + bar - 1) / bar) * bar));

    auto transaction = std::make_unique<commands::Transaction> ("Slices to pattern");
    for (const auto& [name, parameters] :
         { std::pair { "pattern.add", juce::var (add) }, std::pair { "pattern.setNotes", setNotes (newPattern) } })
    {
        auto command = make (name, parameters, error);
        if (command == nullptr)
            return juce::Result::fail (error);
        transaction->add (std::move (command));
    }

    if (auto r = session.getHistory().perform (std::move (transaction)); r.failed())
        return r;
    session.setCurrentPattern (newPattern);
    return juce::Result::ok();
}

void connectSlicesToPattern (Session& session, te::Plugin& plugin)
{
    auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin);
    auto* slicer =
        external != nullptr ? dynamic_cast<dsp::BeatSlicerProcessor*> (external->getAudioPluginInstance()) : nullptr;
    const auto device = session.getAdapter().findDevice (plugin);
    if (slicer == nullptr || ! device)
        return;

    slicer->onSlicesToPattern = [&session, device = *device]
    {
        for (auto& c : session.getProject().channels)
            if (c.instrument && c.instrument->id == device)
            {
                [[maybe_unused]] const auto result = slicesToPattern (session, c.id);
                jassert (result.wasOk());
                return;
            }
    };
}

} // namespace spinus

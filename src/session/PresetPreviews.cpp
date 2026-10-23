#include "PresetPreviews.h"

namespace spinus
{

namespace te = tracktion;

const std::vector<PresetPreviews::PhraseNote>& PresetPreviews::getPhrase()
{
    static const std::vector<PhraseNote> phrase { { 36, 0.0, 0.9 }, { 48, 1.0, 0.9 }, { 60, 2.0, 0.9 } };
    return phrase;
}

PresetPreviews::PresetPreviews (Session& session, const juce::String& stockType) : instrument (session, stockType) {}

PresetPreviews::PresetPreviews (Session& session, const model::Device& device) : instrument (session, device) {}

PresetPreviews::~PresetPreviews() { stopTimer(); }

juce::Result PresetPreviews::renderOne (const juce::File& file, const juce::File& output,
                                        const std::vector<PhraseNote>& notes)
{
    InstrumentPreset preset;
    preset.kind = InstrumentPreset::Kind::file;
    preset.file = file;
    return renderOne (preset, output, notes);
}

juce::Result PresetPreviews::renderOne (const InstrumentPreset& preset, const juce::File& output,
                                        const std::vector<PhraseNote>& notes)
{
    if (auto loaded = instrument.loadPreset (preset); loaded.failed())
        return loaded;
    return renderAsIs (output, notes);
}

juce::Result PresetPreviews::renderAsIs (const juce::File& output, const std::vector<PhraseNote>& notes)
{
    if (getProblem().isNotEmpty())
        return juce::Result::fail (getProblem());
    std::vector<HiddenInstrument::Note> phrase;
    phrase.reserve (notes.size());
    for (auto& n : notes)
        phrase.push_back ({ n.note, n.start, n.length, 100.0f / 127.0f });
    const auto mono = instrument.render (phrase);
    const auto samples = mono.getNumSamples();

    output.getParentDirectory().createDirectory();
    output.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (output);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream,
        juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (mono, 0, samples))
        return juce::Result::fail ("Spinus could not write " + output.getFullPathName() + ".");
    return juce::Result::ok();
}

void PresetPreviews::render (std::vector<Item> items, std::function<void (int, int)> progress,
                             std::function<void (juce::Result)> finished)
{
    queue = std::move (items);
    next = 0;
    onProgress = std::move (progress);
    onFinished = std::move (finished);
    if (getProblem().isNotEmpty())
    {
        if (onFinished)
            onFinished (juce::Result::fail (getProblem()));
        return;
    }
    startTimer (1);
}

void PresetPreviews::cancel()
{
    if (! isTimerRunning())
        return;
    stopTimer();
    queue.clear();
    if (auto finished = std::move (onFinished))
        finished (juce::Result::ok());
}

void PresetPreviews::timerCallback()
{
    // A few at a time, so the window stays responsive.
    const auto started = juce::Time::getMillisecondCounterHiRes();
    while (next < queue.size() && juce::Time::getMillisecondCounterHiRes() - started < 40.0)
    {
        // A preset that does not load is skipped: it just has no preview.
        renderOne (queue[next].preset, queue[next].output);
        ++next;
    }
    if (onProgress)
        onProgress ((int)next, (int)queue.size());
    if (next >= queue.size())
    {
        stopTimer();
        queue.clear();
        if (auto finished = std::move (onFinished))
            finished (juce::Result::ok());
    }
}

juce::Result loadInstrumentPreset (Session& session, model::ChannelId channel, const InstrumentPreset& preset)
{
    const auto* c = session.getProject().findChannel (channel);
    if (c == nullptr || ! c->instrument)
        return juce::Result::fail ("The channel has no instrument.");
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (
        session.getAdapter().getDevicePlugin (c->instrument.value_or (model::Device {}).id));
    auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
    if (instance == nullptr)
        return juce::Result::fail ("The channel's instrument cannot load presets.");
    return InstrumentPresets::load (*instance, preset);
}

} // namespace spinus

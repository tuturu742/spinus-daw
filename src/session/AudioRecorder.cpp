#include "AudioRecorder.h"

#include "RecordingTime.h"
#include "Session.h"

namespace spinus
{

namespace te = tracktion;

AudioRecorder::AudioRecorder (Session& s) : session (s)
{
    session.getEdit().engine.getDeviceManager().deviceManager.addChangeListener (this);
    recording::refreshInputLayout (session.getEdit().engine);
}

AudioRecorder::~AudioRecorder()
{
    session.getEdit().engine.getDeviceManager().deviceManager.removeChangeListener (this);
    onFinished = nullptr;
    onStopped = nullptr;
    cancel();
    detach();
}

void AudioRecorder::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The sound card changed (other inputs in use, or reopened): the recording input is rebuilt
    // (keepMetering() then follows the new one).
    if (! recording)
        recording::refreshInputLayout (session.getEdit().engine);
}

bool AudioRecorder::isAttachedInputAlive() const
{
    if (input == nullptr)
        return false;
    if (auto* context = session.getEdit().getCurrentPlaybackContext())
        for (auto* instance : context->getAllInputs())
            if (instance == input)
                return true;
    return false;
}

void AudioRecorder::keepMetering()
{
    if (! metering || recording)
        return;
    // An armed track's input switched off (another input chosen in Options › Audio), or a track armed:
    // switch it on again (tried once per change of what is armed or switched on).
    const auto wanted = armedInputs();
    const auto active = recording::activeInputChannels (session.getEdit().engine);
    const bool missing = std::any_of (wanted.begin(), wanted.end(), [&active] (int i)
                                      { return std::find (active.begin(), active.end(), i) == active.end(); });
    if (missing && (wanted != lastWanted || active != lastActive))
    {
        lastWanted = wanted;
        lastActive = active;
        setMetering (true);
        return;
    }
    if (! isAttachedInputAlive())
    {
        input = nullptr; // gone with the old sound card, consumers and all
        attach();
    }
}

std::set<int> AudioRecorder::armedInputs() const
{
    auto& engine = session.getEdit().engine;
    const auto names = recording::inputChannelNames (engine);
    std::set<int> inputs;
    for (const auto& channel : session.getProject().channels)
        if (channel.audioTrack && channel.recordArmed)
        {
            const auto first = channel.recordInput > 0 ? channel.recordInput - 1 : recording::getDefaultInput (engine);
            inputs.insert (first);
            if (channel.recordStereo && first + 1 < names.size())
                inputs.insert (first + 1);
        }
    return inputs;
}

juce::Result AudioRecorder::attach()
{
    if (input != nullptr)
        return juce::Result::ok();
    input = recording::findAudioInput (session.getEdit());
    if (input == nullptr)
        return juce::Result::fail ("Spinus has no audio input to record from: choose one in Options › Audio.");
    inputOrder = recording::activeInputChannels (session.getEdit().engine);
    for (auto& level : levels)
        level = 0.0f;
    input->addConsumer (this);
    return juce::Result::ok();
}

void AudioRecorder::detach()
{
    // Removing the consumer waits for the audio thread to finish its block: afterwards the sources can change.
    // An input rebuilt with the sound card took its consumers with it.
    if (isAttachedInputAlive())
        input->removeConsumer (this);
    input = nullptr;
}

juce::Result AudioRecorder::setMetering (bool on)
{
    metering = on;
    if (recording)
        return juce::Result::ok();
    if (! on)
    {
        detach();
        return juce::Result::ok();
    }
    if (auto used = recording::useInputs (session.getEdit().engine, armedInputs()); used.failed())
        return used;
    detach();
    return attach();
}

juce::Result AudioRecorder::start()
{
    if (isRecording())
        return juce::Result::fail ("Already recording.");

    auto& edit = session.getEdit();
    auto& engine = edit.engine;

    // What to record: each armed audio track from its own input, or the default input.
    std::vector<std::unique_ptr<Source>> wanted;
    const auto names = recording::inputChannelNames (engine);
    for (const auto& channel : session.getProject().channels)
        if (channel.audioTrack && channel.recordArmed)
        {
            auto source = std::make_unique<Source>();
            source->track = channel.id;
            source->name = channel.name.isNotEmpty() ? channel.name : juce::String ("Track");
            source->first = channel.recordInput > 0 ? channel.recordInput - 1 : recording::getDefaultInput (engine);
            source->channels = channel.recordStereo && source->first + 1 < names.size() ? 2 : 1;
            wanted.push_back (std::move (source));
        }
    if (wanted.empty())
    {
        auto source = std::make_unique<Source>();
        source->name = "Recording";
        source->first = recording::getDefaultInput (engine);
        wanted.push_back (std::move (source));
    }
    std::set<int> needed;
    for (auto& source : wanted)
        for (int c = 0; c < source->channels; ++c)
            needed.insert (source->first + c);

    if (recording::findAudioInput (edit) == nullptr)
        return juce::Result::fail ("Spinus has no audio input to record from: choose one in Options › Audio.");
    if (auto used = recording::useInputs (engine, needed); used.failed())
        return used;
    detach();
    if (auto attached = attach(); attached.failed())
        return attached;
    detach(); // attached again once the writers are ready

    // Where each input is in the recording input.
    for (auto& source : wanted)
        for (int c = 0; c < source->channels; ++c)
        {
            const auto at = std::find (inputOrder.begin(), inputOrder.end(), source->first + c);
            if (at == inputOrder.end())
                return juce::Result::fail ("Input " + juce::String (source->first + c + 1) + " is not in use.");
            source->index[c] = (int)std::distance (inputOrder.begin(), at);
        }

    sampleRate = engine.getDeviceManager().getSampleRate();
    const auto folder =
        session.getProjectFolder() != juce::File()
            ? session.getProjectFolder().getChildFile ("audio")
            : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-recordings");
    if (! folder.createDirectory())
        return juce::Result::fail ("Cannot create " + folder.getFullPathName());

    writerThread.startThread();
    juce::WavAudioFormat wav;
    for (auto& source : wanted)
    {
        const auto base = juce::File::createLegalFileName (source->name);
        int number = 1;
        while (folder.getChildFile (base + " " + juce::String (number) + ".wav").exists())
            ++number;
        source->file = folder.getChildFile (base + " " + juce::String (number) + ".wav");
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (source->file);
        auto created = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                        .withSampleRate (sampleRate)
                                                        .withNumChannels (source->channels)
                                                        .withBitsPerSample (24));
        if (created == nullptr)
        {
            for (auto& s : wanted)
            {
                s->writer = nullptr;
                s->file.deleteFile();
            }
            writerThread.stopThread (2000);
            return juce::Result::fail ("Cannot write " + source->file.getFullPathName());
        }
        source->writer =
            std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (created.release(), writerThread, 262144);
    }

    sources = std::move (wanted);
    inputName = sources.size() == 1 && ! sources[0]->track
                    ? (names[sources[0]->first].isNotEmpty() ? names[sources[0]->first]
                                                             : "Input " + juce::String (sources[0]->first + 1))
                    : juce::String ((int)sources.size()) + (sources.size() == 1 ? " track" : " tracks");
    firstStreamTime = -1.0;
    samplesWritten = 0;
    fileStartSeconds.reset();
    sawPlaying = false;
    recording = true;
    writing = true;
    if (auto attached = attach(); attached.failed())
    {
        finish (false, attached.getErrorMessage());
        return attached;
    }

    // The song from where the position is, as in FL's song mode recording.
    session.setMode (Session::Mode::song);
    session.play();
    startTimerHz (30);
    return juce::Result::ok();
}

void AudioRecorder::stop()
{
    if (isRecording())
        finish (true);
}

void AudioRecorder::cancel()
{
    if (isRecording())
        finish (false, "Recording cancelled.");
}

float AudioRecorder::getInputLevel() const noexcept
{
    float peak = 0.0f;
    for (auto& source : sources)
        peak = juce::jmax (peak, source->level.load());
    return peak;
}

float AudioRecorder::getInputLevel (int inputNumber) const noexcept
{
    return juce::isPositiveAndBelow (inputNumber, maxInputs) ? levels[(size_t)inputNumber].load() : 0.0f;
}

//==============================================================================
void AudioRecorder::acceptInputBuffer (choc::buffer::ChannelArrayView<float> view)
{
    // Audio thread. Meters: each input's peak.
    const auto frames = (int)view.getNumFrames();
    const auto count = juce::jmin ((int)view.getNumChannels(), (int)inputOrder.size());
    for (int c = 0; c < count; ++c)
    {
        const auto number = inputOrder[(size_t)c];
        if (! juce::isPositiveAndBelow (number, maxInputs))
            continue;
        const auto* data = view.getChannel ((choc::buffer::ChannelCount)c).data.data;
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i)
            peak = juce::jmax (peak, std::abs (data[i]));
        levels[(size_t)number] = peak;
    }

    // Takes: each source's channels straight to its writer's FIFO.
    if (! writing.load() || view.getNumChannels() == 0)
        return;
    if (firstStreamTime.load() < 0.0)
        firstStreamTime = session.getEdit().engine.getDeviceManager().getCurrentStreamTime();
    bool written = true;
    for (auto& source : sources)
    {
        const float* data[2] = { nullptr, nullptr };
        for (int c = 0; c < source->channels; ++c)
            if (source->index[c] < (int)view.getNumChannels())
                data[c] = view.getChannel ((choc::buffer::ChannelCount)source->index[c]).data.data;
        if (data[0] == nullptr || (source->channels > 1 && data[1] == nullptr))
            continue;
        // A full FIFO loses this block (the file is then shorter than the take).
        written = source->writer->write (data, frames) && written;
        float peak = 0.0f;
        for (int c = 0; c < source->channels; ++c)
            for (int i = 0; i < frames; ++i)
                peak = juce::jmax (peak, std::abs (data[c][i]));
        source->level = peak;
    }
    if (written)
        samplesWritten += frames;
}

void AudioRecorder::timerCallback()
{
    if (session.isPlaying())
        sawPlaying = true;
    if (! fileStartSeconds && firstStreamTime.load() >= 0.0 && session.isPlaying())
        fileStartSeconds = recording::editSecondsAt (session.getEdit(), firstStreamTime.load(), sampleRate);
    // Stopping playback (Space, or the transport's stop) ends the take.
    if (sawPlaying && ! session.isPlaying())
        finish (true);
}

void AudioRecorder::finish (bool keep, const juce::String& why)
{
    stopTimer();
    writing = false;
    detach();
    std::vector<Take> takes;
    const bool usable = keep && fileStartSeconds && samplesWritten.load() > 0;
    for (auto& source : sources)
    {
        source->writer.reset(); // writes what is left and closes the file
        if (usable)
            takes.push_back ({ source->file, *fileStartSeconds, source->channels, source->track });
        else
            source->file.deleteFile();
    }
    sources.clear();
    writerThread.stopThread (2000);
    recording = false;
    if (session.isPlaying())
        session.stop();
    if (metering)
        attach();

    if (! usable)
    {
        if (onStopped)
            onStopped (why.isNotEmpty() ? why : juce::String ("Nothing was recorded: no sound came from the input."));
        return;
    }
    if (onFinished)
        onFinished (takes);
}

} // namespace spinus

#include "IdeaRecorder.h"

#include "RecordingTime.h"
#include "Session.h"

namespace spinus
{

namespace te = tracktion;

namespace
{
constexpr int maxBlock = 8192;
constexpr double tailSeconds = 0.3; // after the take, for the last note's end
} // namespace

IdeaRecorder::IdeaRecorder (Session& s) : session (s) { mono.setSize (1, maxBlock); }

IdeaRecorder::~IdeaRecorder()
{
    onFinished = nullptr;
    onStopped = nullptr;
    if (inputUse)
        inputUse->leaveInputsOn();
    cancel();
}

juce::Result IdeaRecorder::startWithPattern()
{
    if (state != State::idle)
        return juce::Result::fail ("Already recording.");
    if (! session.getCurrentPattern().isValid())
        return juce::Result::fail ("There is no pattern to record over: add one first.");

    free = false;
    if (auto result = begin(); result.failed())
        return result;

    // The pattern from its start with the metronome; the first time through is the count-in.
    metronomeWas = session.isMetronomeOn();
    session.setMetronome (true);
    session.stop();
    session.setMode (Session::Mode::pattern);
    session.play();
    state = State::countIn;
    return juce::Result::ok();
}

juce::Result IdeaRecorder::startFree (double seconds)
{
    if (state != State::idle)
        return juce::Result::fail ("Already recording.");

    free = true;
    freeSeconds = seconds;
    if (auto result = begin(); result.failed())
        return result;

    state = State::recording;
    return juce::Result::ok();
}

juce::Result IdeaRecorder::begin()
{
    auto& edit = session.getEdit();
    if (recording::findAudioInput (edit) == nullptr)
        return juce::Result::fail ("Spinus has no audio input to record from: choose one in Options › Audio.");

    // The input chosen in Options › Audio, switched on (a sound card with many inputs has those in use in the
    // recording input).
    const auto chosen = recording::getDefaultInput (edit.engine);
    if (! inputUse)
        inputUse.emplace (edit.engine);
    if (auto used = recording::useInputs (edit.engine, { chosen }); used.failed())
    {
        inputUse.reset();
        return used;
    }
    input = recording::findAudioInput (edit); // rebuilt if the sound card restarted
    if (input == nullptr)
    {
        inputUse.reset();
        return juce::Result::fail ("Spinus has no audio input to record from: choose one in Options › Audio.");
    }
    const auto order = recording::activeInputChannels (edit.engine);
    const auto at = std::find (order.begin(), order.end(), chosen);
    inputChannel = at != order.end() ? (int)std::distance (order.begin(), at) : 0;
    const auto names = recording::inputChannelNames (edit.engine);
    inputName = names[chosen].isNotEmpty() ? names[chosen] : input->getInputDevice().getName();
    sampleRate = edit.engine.getDeviceManager().getSampleRate();

    const auto folder = session.getProjectFolder() != juce::File()
                            ? session.getProjectFolder().getChildFile ("cache").getChildFile ("ideas")
                            : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-ideas");
    if (! folder.createDirectory())
    {
        inputUse.reset();
        return juce::Result::fail ("Cannot create " + folder.getFullPathName());
    }
    file =
        folder.getChildFile (juce::Time::getCurrentTime().formatted ("take-%Y%m%d-%H%M%S.wav")).getNonexistentSibling();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto created = wav.createWriterFor (
        stream,
        juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    if (created == nullptr)
    {
        inputUse.reset();
        return juce::Result::fail ("Cannot write " + file.getFullPathName());
    }

    writerThread.startThread();
    writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (created.release(), writerThread, 65536);
    firstStreamTime = -1.0;
    samplesWritten = 0;
    fileStartSeconds.reset();
    progress = 0.0f;
    beatsToGo = 0;
    input->addConsumer (this);
    startTimerHz (30);
    return juce::Result::ok();
}

//==============================================================================
void IdeaRecorder::acceptInputBuffer (choc::buffer::ChannelArrayView<float> view)
{
    // Audio thread: the chosen input to the writer's FIFO.
    if (writer == nullptr || inputChannel >= (int)view.getNumChannels())
        return;

    if (firstStreamTime.load() < 0.0)
        firstStreamTime = session.getEdit().engine.getDeviceManager().getCurrentStreamTime();

    auto remaining = (int)view.getNumFrames();
    int offset = 0;
    float peak = 0.0f;
    while (remaining > 0)
    {
        const auto count = juce::jmin (remaining, maxBlock);
        auto* out = mono.getWritePointer (0);
        for (int i = 0; i < count; ++i)
        {
            out[i] = view.getSample ((choc::buffer::ChannelCount)inputChannel, (choc::buffer::FrameCount)(offset + i));
            peak = juce::jmax (peak, std::abs (out[i]));
        }
        const float* data[] = { out };
        writer->write (data, count);
        samplesWritten += count;
        offset += count;
        remaining -= count;
    }
    level = peak;
}

//==============================================================================
double IdeaRecorder::editSecondsNow (double streamTime) const
{
    return recording::editSecondsAt (session.getEdit(), streamTime, sampleRate);
}

void IdeaRecorder::timerCallback()
{
    if (free)
    {
        const auto elapsed = (double)samplesWritten.load() / sampleRate;
        progress = (float)juce::jlimit (0.0, 1.0, elapsed / freeSeconds);
        if (elapsed >= freeSeconds)
            finish();
        return;
    }

    auto* context = session.getEdit().getCurrentPlaybackContext();
    if (context == nullptr || ! session.isPlaying())
    {
        if (state == State::recording || firstStreamTime.load() >= 0.0)
        {
            cancel(); // playback was stopped
            return;
        }
        return;
    }

    if (! fileStartSeconds && firstStreamTime.load() >= 0.0)
        fileStartSeconds = editSecondsNow (firstStreamTime.load());

    const auto patternSeconds = session.getAdapter().getPatternLoopRange().getEnd().inSeconds();
    const auto now =
        context->globalStreamTimeToEditTimeUnlooped (session.getEdit().engine.getDeviceManager().getCurrentStreamTime())
            .inSeconds();
    const auto beatSeconds = 60.0 / juce::jmax (1.0, session.getProject().settings.tempoBpm);

    if (now < patternSeconds)
    {
        state = State::countIn;
        beatsToGo = (int)std::ceil ((patternSeconds - now) / beatSeconds - 1.0e-6);
        return;
    }

    state = State::recording;
    beatsToGo = 0;
    progress = (float)juce::jlimit (0.0, 1.0, (now - patternSeconds) / juce::jmax (0.001, patternSeconds));
    if (now >= 2.0 * patternSeconds + tailSeconds && fileStartSeconds)
        finish();
}

void IdeaRecorder::stopRecording()
{
    stopTimer();
    if (input != nullptr)
    {
        input->removeConsumer (this);
        input = nullptr;
    }
    writer.reset(); // writes what is left and closes the file
    writerThread.stopThread (2000);
    inputUse.reset();

    if (! free)
    {
        session.stop();
        session.setMetronome (metronomeWas);
    }
    state = State::idle;
}

void IdeaRecorder::finish()
{
    const auto patternSeconds = free ? 0.0 : session.getAdapter().getPatternLoopRange().getEnd().inSeconds();
    const auto start = free ? 0.0 : patternSeconds - fileStartSeconds.value_or (0.0);
    stopRecording();

    if (onFinished)
        onFinished ({ file, start, patternSeconds, session.getProject().settings.tempoBpm });
}

void IdeaRecorder::cancel()
{
    if (state == State::idle && writer == nullptr)
        return;
    stopRecording();
    file.deleteFile();
    if (onStopped)
        onStopped ("Recording stopped.");
}

} // namespace spinus

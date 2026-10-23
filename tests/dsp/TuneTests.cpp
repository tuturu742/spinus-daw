#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/tune/PitchDetector.h"
#include "dsp/tune/TuneEngine.h"

using namespace spinus::dsp;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 128;

/** A sine (or a band-limited sawtooth, more like a voice) at a frequency that may glide over time. */
std::vector<float> tone (double seconds, const std::function<double (double)>& hzAt, bool saw = false,
                         float level = 0.5f)
{
    std::vector<float> x ((size_t)(seconds * rate));
    double phase = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const auto hz = hzAt ((double)i / rate);
        phase += hz / rate;
        double v = 0.0;
        if (saw)
            for (int h = 1; h * hz < rate * 0.45 && h <= 30; ++h)
                v += std::sin (2.0 * M_PI * h * phase) / h * 0.6;
        else
            v = std::sin (2.0 * M_PI * phase);
        x[i] = level * (float)v;
    }
    return x;
}

std::vector<float> steady (double seconds, double hz, bool saw = false)
{
    return tone (seconds, [hz] (double) { return hz; }, saw);
}

/** Runs mono audio through the engine in blocks, with MIDI notes held throughout. */
std::vector<float> run (TuneEngine& engine, const std::vector<float>& in, const TuneSettings& s,
                        const std::vector<int>& notes = {})
{
    std::vector<float> out;
    juce::AudioBuffer<float> buffer (1, block);
    juce::MidiBuffer midi;
    for (auto n : notes)
        midi.addEvent (juce::MidiMessage::noteOn (1, n, 0.8f), 0);
    for (size_t start = 0; start + block <= in.size(); start += block)
    {
        for (int i = 0; i < block; ++i)
            buffer.setSample (0, i, in[start + (size_t)i]);
        engine.process (buffer, midi, s);
        midi.clear();
        for (int i = 0; i < block; ++i)
            out.push_back (buffer.getSample (0, i));
    }
    return out;
}

/** The pitch over the last part of the audio, in Hz, measured with the detector. */
double pitchAt (const std::vector<float>& x, double fromSeconds, float minHz = 60.0f)
{
    PitchDetector detector;
    detector.prepare (rate, minHz, 1200.0f);
    std::vector<double> periods;
    for (auto start = (size_t)(fromSeconds * rate); start + (size_t)detector.getFrameLength() <= x.size(); start += 480)
        if (auto r = detector.analyse (x.data() + start); r.isVoiced())
            periods.push_back (r.period);
    if (periods.empty())
        return 0.0;
    std::sort (periods.begin(), periods.end());
    return rate / periods[periods.size() / 2];
}

double cents (double hz, double reference) { return 1200.0 * std::log2 (hz / reference); }

std::unique_ptr<TuneEngine> prepared()
{
    auto engine = std::make_unique<TuneEngine>();
    engine->prepare (rate, block, 1);
    return engine;
}
} // namespace

TEST_CASE ("The pitch detector finds the pitch of sines and voice-like tones, and none in noise", "[dsp][tune]")
{
    PitchDetector detector;
    detector.prepare (rate, 50.0f, 1200.0f);

    for (auto hz : { 55.0, 82.4, 110.0, 196.0, 261.6, 440.0, 880.0 })
        for (bool saw : { false, true })
        {
            INFO (hz << " Hz, " << (saw ? "sawtooth" : "sine"));
            const auto x = steady (0.2, hz, saw);
            const auto result = detector.analyse (x.data() + 1000);
            REQUIRE (result.isVoiced());
            CHECK (std::abs (cents (rate / result.period, hz)) < 3.0);
            CHECK (result.clarity > 0.85f);
        }

    juce::Random random (1);
    std::vector<float> noise ((size_t)detector.getFrameLength());
    for (auto& v : noise)
        v = random.nextFloat() - 0.5f;
    CHECK (! detector.analyse (noise.data()).isVoiced());

    std::vector<float> silence ((size_t)detector.getFrameLength(), 0.0f);
    CHECK (! detector.analyse (silence.data()).isVoiced());
}

TEST_CASE ("Spinus Tune's scales and its choice of note", "[dsp][tune]")
{
    using Scale = TuneSettings::Scale;
    CHECK (TuneEngine::isInScale (60, 0, Scale::major));   // C
    CHECK (! TuneEngine::isInScale (61, 0, Scale::major)); // C#
    CHECK (TuneEngine::isInScale (69, 9, Scale::minor));   // A in A minor
    CHECK (TuneEngine::isInScale (72, 9, Scale::minor));   // C in A minor
    CHECK (! TuneEngine::isInScale (73, 9, Scale::minor));

    const std::array<bool, 128> none {};
    CHECK (TuneEngine::chooseTarget (69.3f, -1, 0, Scale::chromatic, none, false) == 69);
    CHECK (TuneEngine::chooseTarget (69.8f, -1, 0, Scale::major, none, false) == 69); // A# is not in C major
    CHECK (TuneEngine::chooseTarget (70.3f, -1, 0, Scale::major, none, false) == 71);
    // Near the middle the current note is kept (no flicker).
    CHECK (TuneEngine::chooseTarget (69.6f, 69, 0, Scale::chromatic, none, false) == 69);
    CHECK (TuneEngine::chooseTarget (69.7f, 69, 0, Scale::chromatic, none, false) == 70);

    // MIDI notes held: only those.
    std::array<bool, 128> held {};
    held[62] = held[66] = true;
    CHECK (TuneEngine::chooseTarget (63.8f, -1, 0, Scale::chromatic, held, true) == 62);
    CHECK (TuneEngine::chooseTarget (65.0f, -1, 0, Scale::chromatic, held, true) == 66);
}

TEST_CASE ("Spinus Tune pulls a voice to the nearest note, within 10 ms of delay", "[dsp][tune]")
{
    CHECK (TuneEngine::getLatency (rate, TuneSettings::Range::normal) <= (int)(0.010 * rate));
    CHECK (TuneEngine::getLatency (44100.0, TuneSettings::Range::normal) <= (int)(0.010 * 44100.0));

    for (bool saw : { false, true })
    {
        INFO ((saw ? "sawtooth" : "sine"));
        auto engine = prepared();
        TuneSettings s;
        s.retuneMs = 0.0f;
        // 450 Hz is 39 cents above A4 (440 Hz).
        const auto out = run (*engine, steady (1.5, 450.0, saw), s);
        CHECK (std::abs (cents (pitchAt (out, 0.5), 440.0)) < 5.0);
        CHECK (engine->getTargetNote() == 69);
        CHECK (engine->getCorrection() == Catch::Approx (-0.39).margin (0.03));

        // The level stays about the same.
        const auto inLevel = 0.5 / std::sqrt (2.0);
        double sum = 0.0;
        for (size_t i = (size_t)(0.5 * rate); i < out.size(); ++i)
            sum += (double)out[i] * out[i];
        const auto outLevel = std::sqrt (sum / (double)(out.size() - (size_t)(0.5 * rate)));
        if (! saw)
            CHECK (outLevel == Catch::Approx (inLevel).epsilon (0.15));
    }
}

TEST_CASE ("Spinus Tune follows the scale, the MIDI notes held, and lower voices", "[dsp][tune]")
{
    TuneSettings s;
    s.retuneMs = 0.0f;

    {
        // 460 Hz is between A4 and A#4; A# is not in C major, so it goes to A.
        auto engine = prepared();
        s.scale = TuneSettings::Scale::major;
        CHECK (std::abs (cents (pitchAt (run (*engine, steady (1.2, 460.0, true), s), 0.5), 440.0)) < 5.0);
    }
    {
        // D4 held on the MIDI input: a voice at 280 Hz goes to 293.7 Hz.
        auto engine = prepared();
        s.scale = TuneSettings::Scale::chromatic;
        const auto out = run (*engine, steady (1.2, 280.0, true), s, { 62 });
        CHECK (std::abs (cents (pitchAt (out, 0.5), 293.66)) < 5.0);
    }
    {
        // A low voice at 100 Hz goes to G2 (98 Hz) in the low range.
        auto engine = prepared();
        s.range = TuneSettings::Range::low;
        const auto out = run (*engine, steady (1.5, 100.0, true), s);
        CHECK (std::abs (cents (pitchAt (out, 0.6, 50.0f), 98.0)) < 8.0);
    }
}

TEST_CASE ("Retune speed: slow keeps the voice's own pitch for a while", "[dsp][tune]")
{
    auto engine = prepared();
    TuneSettings s;
    s.retuneMs = 400.0f;
    const auto out = run (*engine, steady (2.5, 450.0, true), s);
    // Early on still close to 450 Hz; after two seconds at 440 Hz.
    CHECK (std::abs (cents (pitchAt (std::vector<float> (out.begin(), out.begin() + (int)(0.15 * rate)), 0.0), 450.0)) <
           15.0);
    CHECK (std::abs (cents (pitchAt (out, 2.0), 440.0)) < 5.0);
}

TEST_CASE ("Without a pitch the sound passes through, delayed; mix 0 is the voice as it was", "[dsp][tune]")
{
    auto engine = prepared();
    TuneSettings s;
    const auto latency = TuneEngine::getLatency (rate, s.range);

    juce::Random random (7);
    std::vector<float> noise ((size_t)rate);
    for (auto& v : noise)
        v = (random.nextFloat() - 0.5f) * 0.5f;
    const auto out = run (*engine, noise, s);
    float worst = 0.0f;
    for (size_t i = (size_t)latency + 2000; i < out.size(); ++i)
        worst = std::max (worst, std::abs (out[i] - noise[i - (size_t)latency]));
    CHECK (worst < 1.0e-5f);

    auto dryEngine = prepared();
    s.mix = 0.0f;
    const auto voice = steady (1.0, 450.0, true);
    const auto dry = run (*dryEngine, voice, s);
    worst = 0.0f;
    for (size_t i = (size_t)latency; i < dry.size(); ++i)
        worst = std::max (worst, std::abs (dry[i] - voice[i - (size_t)latency]));
    CHECK (worst < 1.0e-6f);
}

TEST_CASE ("Robot retune flattens vibrato; formants can follow the pitch; stereo", "[dsp][tune]")
{
    TuneSettings s;
    s.retuneMs = 0.0f;

    // 445 Hz with a 5.5 Hz vibrato of +-30 cents: corrected, it stays on A4.
    const auto vibrato = tone (
        2.0, [] (double t) { return 445.0 * std::pow (2.0, 0.3 / 12.0 * std::sin (2.0 * M_PI * 5.5 * t)); }, true);
    auto engine = prepared();
    const auto out = run (*engine, vibrato, s);
    PitchDetector detector;
    detector.prepare (rate, 60.0f, 1200.0f);
    double worst = 0.0;
    for (auto start = (size_t)(0.5 * rate); start + (size_t)detector.getFrameLength() < out.size(); start += 240)
        if (auto r = detector.analyse (out.data() + start); r.isVoiced())
            worst = std::max (worst, std::abs (cents (rate / r.period, 440.0)));
    CHECK (worst < 12.0);

    // Formants following the pitch: the same note.
    auto chipmunk = prepared();
    s.keepFormants = false;
    CHECK (std::abs (cents (pitchAt (run (*chipmunk, steady (1.2, 450.0, true), s), 0.5), 440.0)) < 5.0);

    // Both channels of a stereo voice are corrected alike.
    TuneEngine stereo;
    stereo.prepare (rate, block, 2);
    s.keepFormants = true;
    const auto voice = steady (1.2, 450.0, true);
    juce::AudioBuffer<float> buffer (2, block);
    std::vector<float> left, right;
    for (size_t start = 0; start + block <= voice.size(); start += block)
    {
        for (int i = 0; i < block; ++i)
        {
            buffer.setSample (0, i, voice[start + (size_t)i]);
            buffer.setSample (1, i, 0.5f * voice[start + (size_t)i]);
        }
        stereo.process (buffer, {}, s);
        for (int i = 0; i < block; ++i)
        {
            left.push_back (buffer.getSample (0, i));
            right.push_back (buffer.getSample (1, i));
        }
    }
    CHECK (std::abs (cents (pitchAt (left, 0.5), 440.0)) < 5.0);
    for (size_t i = (size_t)rate / 2; i < left.size(); ++i)
        REQUIRE (right[i] == Catch::Approx (0.5f * left[i]).margin (1.0e-5));
}

TEST_CASE ("Spinus Tune at other sample rates and block sizes gives the same result", "[dsp][tune]")
{
    for (auto sampleRate : { 44100.0, 96000.0, 192000.0 })
    {
        INFO (sampleRate);
        TuneEngine engine;
        engine.prepare (sampleRate, 512, 1);
        TuneSettings s;
        s.retuneMs = 0.0f;
        std::vector<float> in ((size_t)(1.2 * sampleRate)), out;
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.5f * (float)std::sin (2.0 * M_PI * 450.0 * (double)i / sampleRate);

        // Blocks of changing sizes, from 1 to 512 samples.
        juce::AudioBuffer<float> buffer (1, 512);
        juce::Random random (3);
        for (size_t start = 0; start < in.size();)
        {
            const auto size = (int)std::min<size_t> (in.size() - start, (size_t)(1 + random.nextInt (512)));
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 1, size);
            for (int i = 0; i < size; ++i)
                view.setSample (0, i, in[start + (size_t)i]);
            engine.process (view, {}, s);
            for (int i = 0; i < size; ++i)
                out.push_back (view.getSample (0, i));
            start += (size_t)size;
        }

        PitchDetector detector;
        detector.prepare (sampleRate, 60.0f, 1200.0f);
        const auto r = detector.analyse (out.data() + (size_t)(0.8 * sampleRate));
        REQUIRE (r.isVoiced());
        CHECK (std::abs (cents (sampleRate / r.period, 440.0)) < 5.0);
        CHECK (TuneEngine::getLatency (sampleRate, TuneSettings::Range::normal) <= (int)(0.010 * sampleRate));
    }

    // The same input gives the same output.
    auto a = prepared(), b = prepared();
    TuneSettings s;
    const auto voice = steady (0.5, 300.0, true);
    CHECK (run (*a, voice, s) == run (*b, voice, s));
}

//==============================================================================
#include "dsp/tune/TuneEditor.h"
#include "dsp/tune/TuneProcessor.h"

namespace
{
void set (TuneProcessor& p, const juce::String& id, float value)
{
    auto* parameter = p.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}
} // namespace

TEST_CASE ("The Spinus Tune device: its delay, its settings in its state, and its window", "[dsp][tune][ui]")
{
    TuneProcessor tune;
    tune.setRateAndBufferSizeDetails (rate, block);
    tune.prepareToPlay (rate, block);
    CHECK (tune.getLatencySamples() == TuneEngine::getLatency (rate, TuneSettings::Range::normal));
    CHECK (tune.acceptsMidi());

    set (tune, "key", 9.0f);   // A
    set (tune, "scale", 2.0f); // minor
    set (tune, "retune", 0.0f);
    set (tune, "range", 1.0f); // low voice: a longer delay, reported
    CHECK (tune.getLatencySamples() == TuneEngine::getLatency (rate, TuneSettings::Range::low));
    CHECK (tune.getSettings().scale == TuneSettings::Scale::minor);

    juce::MemoryBlock state;
    tune.getStateInformation (state);
    TuneProcessor copy;
    copy.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (copy.getSettings().key == 9);
    CHECK (copy.getSettings().scale == TuneSettings::Scale::minor);
    CHECK (copy.getSettings().range == TuneSettings::Range::low);
    CHECK (copy.getLatencySamples() == TuneEngine::getLatency (48000.0, TuneSettings::Range::low));

    // A sung C#5 (554 Hz, not in A minor): the window shows it pulled to C or D.
    set (tune, "range", 0.0f);
    tune.prepareToPlay (rate, block);
    std::unique_ptr<juce::AudioProcessorEditor> editor (tune.createEditorAndMakeActive());
    auto* window = dynamic_cast<TuneEditor*> (editor.get());
    REQUIRE (window != nullptr);
    CHECK (window->getStatusText() == "No pitch");

    const auto voice = steady (0.6, 554.0, true);
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + block <= voice.size(); start += block)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
                buffer.setSample (c, i, voice[start + (size_t)i]);
        tune.processBlock (buffer, midi);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    CHECK (window->getStatusText().startsWith ("C#6 "));
    CHECK ((window->getStatusText().endsWith ("-> D6") || window->getStatusText().endsWith ("-> C6")));
    tune.editorBeingDeleted (editor.get());
}

TEST_CASE ("Spinus Tune window screenshot for the manual", "[.screenshot][tune]")
{
    TuneProcessor tune;
    set (tune, "key", 9.0f);
    set (tune, "scale", 2.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (tune.createEditorAndMakeActive());
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/spinus-tune.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    tune.editorBeingDeleted (editor.get());
}

TEST_CASE ("Spinus Tune CPU cost: stereo voice, 48 kHz, 128 samples", "[.bench][tune]")
{
    TuneEngine engine;
    engine.prepare (rate, block, 2);
    TuneSettings s;
    const auto voice = steady (5.0, 220.0, true);
    juce::AudioBuffer<float> buffer (2, block);
    const auto start = juce::Time::getHighResolutionTicks();
    for (size_t at = 0; at + block <= voice.size(); at += block)
    {
        for (int c = 0; c < 2; ++c)
            buffer.copyFrom (c, 0, voice.data() + at, block);
        engine.process (buffer, {}, s);
    }
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "Spinus Tune: " << seconds / 5.0 * 100.0 << " % of one core\n";
}

//==============================================================================
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/Freeze.h"
#include "session/Session.h"

TEST_CASE ("Spinus Tune on a vocal insert follows the notes of a guide channel", "[dsp][tune][audio]")
{
    using namespace spinus;
    using namespace spinus::model;
    namespace te = tracktion;

    // A 280 Hz voice-like take, two seconds.
    const juce::TemporaryFile take (".wav");
    {
        const auto voice = steady (2.0, 280.0, true);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (take.getFile());
        auto writer = wav.createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        const float* channels[] = { voice.data() };
        writer->writeFromFloatArrays (channels, 1, (int)voice.size());
    }

    EngineHost host ({ .openAudioDevice = false });
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    session.setMode (Session::Mode::song);

    Project p;
    p.settings.tempoBpm = 120.0;
    Insert vocal;
    vocal.id = p.allocateId<InsertId>();
    vocal.name = "Vocal";
    vocal.effects.push_back ({ Device { p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.tune" } });
    p.mixer.inserts = { vocal };

    Channel voice, guide;
    voice.id = p.allocateId<ChannelId>();
    voice.name = "Voice";
    voice.targetInsert = vocal.id;
    guide.id = p.allocateId<ChannelId>();
    guide.name = "Guide";
    p.channels = { voice, guide };

    Pattern melody;
    melody.id = p.allocateId<PatternId>();
    melody.length = 4 * ticksPerQuarterNote;
    melody.notes[guide.id] = { Note { 0, 4 * ticksPerQuarterNote, 62, 0.8f } }; // D4
    p.patterns = { melody };

    Lane lane { p.allocateId<LaneId>(), "Lane" };
    p.playlist.lanes = { lane };
    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), lane.id, 0, melody.length, false, PatternClip { melody.id, 0 } });
    AudioClip audio;
    audio.file = take.getFile().getFullPathName();
    audio.channel = voice.id;
    audio.stretchMode = "off";
    p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane.id, 0, melody.length, false, audio });

    auto render = [&]
    {
        session.getEdit().dispatchPendingUpdatesSynchronously();
        const auto file = juce::File::createTempFile (".wav");
        juce::WavAudioFormat wav;
        te::Renderer::Parameters params (session.getEdit());
        params.destFile = file;
        params.tracksToDo = te::toBitSet (te::getAllTracks (session.getEdit()));
        params.audioFormat = &wav;
        params.bitDepth = 32;
        params.sampleRateForAudio = rate;
        params.blockSizeForAudio = 512;
        params.time = { te::TimePosition(), te::TimePosition::fromSeconds (1.8) };
        params.usePlugins = true;
        params.useMasterPlugins = true;
        params.checkNodesForAudio = false;
        const auto rendered = te::Renderer::renderToFile ("tune test", params);
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (rendered), true));
        REQUIRE (reader != nullptr);
        std::vector<float> left ((size_t)reader->lengthInSamples);
        float* destination[] = { left.data() };
        reader->read (destination, 1, 0, (int)left.size());
        reader.reset();
        rendered.deleteFile();
        return left;
    };

    // Without the guide: the nearest note, C#4 (277.2 Hz).
    session.getHistory().reset (p);
    CHECK (std::abs (cents (pitchAt (render(), 0.6), 277.18)) < 8.0);

    // With it: D4, which the guide plays.
    p.mixer.inserts[0].notesFrom = guide.id;
    session.getHistory().reset (p);
    CHECK (session.getAdapter().describe().contains ("notes for insert " + juce::String (vocal.id.value) +
                                                     ": 0.000-4.000 muted=0 notes=[62@0.000]"));
    CHECK (std::abs (cents (pitchAt (render(), 0.6), 293.66)) < 8.0);

    // Freezing the vocal insert keeps the guide's notes in it, but not the guide's sound.
    const auto plan = makeFreezePlan (session.getProject(), vocal.id, std::nullopt);
    const auto* copied = plan.project.findChannel (guide.id);
    REQUIRE (copied != nullptr);
    CHECK (! copied->instrument);
    CHECK (plan.project.findInsert (vocal.id)->notesFrom == guide.id);
}

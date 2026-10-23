#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "dsp/NoteExpression.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "ui/ChannelRack.h"
#include "ui/PianoRoll.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::ui;
namespace te = tracktion;
namespace ne = spinus::dsp::noteExpression;
using Lane = PianoRoll::LaneMode;

namespace
{
juce::File bundledPlugins()
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR);
#else
    return {};
#endif
}

struct Roll
{
    Roll() { roll.setSize (760, 420); }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (te::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
        return devices;
    }

    /** The Tracktion clip that plays the selected channel's notes in the current pattern. */
    te::MidiClip* clip()
    {
        const auto* channel = session.getProject().findChannel (session.getSelectedChannel());
        for (auto* track : te::getAudioTracks (session.getEdit()))
            if (channel != nullptr && track->getName() == channel->name)
                for (auto* c : track->getClips())
                    if (auto* midi = dynamic_cast<te::MidiClip*> (c))
                        return midi;
        return nullptr;
    }

    juce::MidiMessageSequence playbackMidi()
    {
        auto* c = clip();
        REQUIRE (c != nullptr);
        return c->getSequence().exportToPlaybackMidiSequence (*c, te::MidiList::TimeBase::beatsRaw, c->getMPEMode());
    }

    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    ChannelRack rack { session };
    PianoRoll roll { session };
};

/** The frequency of a periodic signal, from its first and last rising zero crossings. */
double frequencyOf (const std::vector<float>& x, int64_t begin, int64_t end, double sampleRate)
{
    double first = -1.0, last = -1.0;
    int cycles = -1;
    for (auto n = begin + 1; n < end; ++n)
        if (x[(size_t)n - 1] < 0.0f && x[(size_t)n] >= 0.0f)
        {
            const auto at = (double)(n - 1) + x[(size_t)n - 1] / (x[(size_t)n - 1] - x[(size_t)n]);
            first = first < 0.0 ? at : first;
            last = at;
            ++cycles;
        }
    return cycles > 0 ? cycles * sampleRate / (last - first) : 0.0;
}
} // namespace

TEST_CASE ("The note lane edits velocity, release, pan, fine pitch and volume, one undo step each", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    REQUIRE (r.roll.addNote (60, 0, 240, 0.8f));
    CHECK (r.roll.getLaneMode() == Lane::velocity);

    REQUIRE (r.roll.setNoteValue (0, Lane::release, 0.25f));
    REQUIRE (r.roll.setNoteValue (0, Lane::pan, -0.5f));
    REQUIRE (r.roll.setNoteValue (0, Lane::finePitch, 250.0f)); // clamped to a semitone
    REQUIRE (r.roll.setNoteValue (0, Lane::volume, 0.5f));

    auto note = r.roll.getNotes()[0];
    CHECK (juce::approximatelyEqual (note.releaseVelocity, 0.25f));
    CHECK (juce::approximatelyEqual (note.pan, -0.5f));
    CHECK (juce::approximatelyEqual (note.finePitchCents, 100.0f));
    CHECK (juce::approximatelyEqual (note.getVolume(), 0.5f));
    CHECK (juce::approximatelyEqual (note.expressions.at ("volume"), 0.5f)); // the CLAP note expression's name

    // Moving the note keeps every value (the volume too).
    REQUIRE (r.roll.moveNote (0, 240, 2));
    note = r.roll.getNotes()[0];
    CHECK (juce::approximatelyEqual (note.getVolume(), 0.5f));
    CHECK (juce::approximatelyEqual (note.pan, -0.5f));

    CHECK (r.session.getHistory().getUndoDescriptions().joinIntoString (" | ").endsWith (
        "Change release | Change pan | Change fine pitch | Change volume | Move note"));

    // Back to the default volume: nothing extra is stored.
    REQUIRE (r.roll.setNoteValue (0, Lane::volume, 1.0f));
    CHECK (r.roll.getNotes()[0].expressions.empty());

    r.session.getHistory().undo();
    r.session.getHistory().undo();
    r.session.getHistory().undo();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (juce::approximatelyEqual (r.roll.getNotes()[0].getVolume(), 1.0f));
    CHECK (juce::approximatelyEqual (r.roll.getNotes()[0].finePitchCents, 100.0f));
}

TEST_CASE ("Note values out of range are refused by the command", "[commands]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    REQUIRE (r.roll.addNote (60, 0, 240, 0.8f));

    for (auto field : { R"("pan": 1.5)", R"("finePitchCents": -101)", R"("releaseVelocity": 2)",
                        R"("expressions": {"volume": 1.5})" })
    {
        INFO (field);
        const auto json = juce::String (R"({"pattern": )") + juce::String ((int)r.session.getCurrentPattern().value) +
                          R"(, "channel": )" + juce::String ((int)r.session.getSelectedChannel().value) +
                          R"(, "notes": [{"start": 0, "length": 240, "pitch": 60, )" + field + "}]}";
        juce::String error;
        auto command = commands::CommandRegistry::get().create ("pattern.setNotes", juce::JSON::parse (json), error);
        const bool refused = command == nullptr || ! r.session.getHistory().perform (std::move (command)).wasOk();
        CHECK (refused);
    }
}

TEST_CASE ("Per-note values go to the instrument on a channel per note; release velocity goes everywhere",
           "[engine][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "Lead"));
    REQUIRE (r.roll.addNote (60, 0, 480, 0.8f));
    REQUIRE (r.roll.addNote (64, 0, 480, 0.8f));
    REQUIRE (r.roll.setNoteValue (0, Lane::pan, -1.0f));
    REQUIRE (r.roll.setNoteValue (0, Lane::finePitch, 50.0f));
    REQUIRE (r.roll.setNoteValue (0, Lane::volume, 0.5f));
    REQUIRE (r.roll.setNoteValue (1, Lane::release, 0.25f));

    REQUIRE (r.clip() != nullptr);
    CHECK (r.clip()->getMPEMode());
    const auto midi = r.playbackMidi();

    std::map<int, std::vector<juce::MidiMessage>> byChannel;
    for (auto* e : midi)
        byChannel[e->message.getChannel()].push_back (e->message);

    // Two notes at once: two member channels, each with its values just before its note-on.
    REQUIRE (byChannel.size() == 2);
    for (auto& [channel, messages] : byChannel)
    {
        INFO ("channel " << channel);
        CHECK (ne::isMemberChannel (channel));
        REQUIRE (messages.size() == 5);
        CHECK (messages[0].isPitchWheel());
        CHECK (messages[1].isControllerOfType (ne::panController));
        CHECK (messages[2].isControllerOfType (ne::volumeController));
        CHECK (messages[3].isNoteOn());
        CHECK (messages[4].isNoteOff());

        if (messages[3].getNoteNumber() == 60)
        {
            CHECK (std::abs (ne::pitchWheelToCents (messages[0].getPitchWheelValue()) - 50.0f) < 1.0f);
            CHECK (messages[1].getControllerValue() == 1);  // hard left
            CHECK (messages[2].getControllerValue() == 64); // half volume
        }
        else
        {
            CHECK (messages[0].getPitchWheelValue() == 8192);
            CHECK (messages[1].getControllerValue() == 64);
            CHECK (messages[2].getControllerValue() == 127);
            CHECK (messages[4].getVelocity() == 32); // release velocity 0.25
        }
    }

    // An instrument that does not read per-note values (here: none) gets plain notes on channel 1,
    // with the release velocity still there.
    REQUIRE (r.rack.addChannel ({}, "Plain"));
    REQUIRE (r.roll.addNote (60, 0, 480, 0.8f));
    REQUIRE (r.roll.setNoteValue (0, Lane::pan, -1.0f));
    REQUIRE (r.roll.setNoteValue (0, Lane::release, 0.25f));
    REQUIRE (r.clip() != nullptr);
    CHECK (! r.clip()->getMPEMode());
    const auto plain = r.playbackMidi();
    REQUIRE (plain.getNumEvents() == 2);
    CHECK (plain.getEventPointer (0)->message.getChannel() == 1);
    CHECK (plain.getEventPointer (1)->message.isNoteOff());
    CHECK (plain.getEventPointer (1)->message.getVelocity() == 32);
}

TEST_CASE ("3x Osc plays per-note fine pitch, volume and pan", "[engine][pianoroll][audio]")
{
    Roll r;
    test::RealtimeRunner runner (r.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = false });
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "Lead"));

    // Four quarter notes at 120 BPM (half a second each), an eighth long so each fades before the next:
    // plain, a semitone up, half volume, hard left.
    for (int i = 0; i < 4; ++i)
        REQUIRE (r.roll.addNote (69, (model::Ticks)i * 960, 480, 0.8f));
    REQUIRE (r.roll.setNoteValue (1, Lane::finePitch, 100.0f));
    REQUIRE (r.roll.setNoteValue (2, Lane::volume, 0.5f));
    REQUIRE (r.roll.setNoteValue (3, Lane::pan, -1.0f));

    r.session.play();
    runner.start();
    runner.runUntil (2.1);
    runner.stop();
    r.session.stop();

    const auto sr = runner.getParams().sampleRate;
    const auto& left = runner.getChannel (0);
    const auto& right = runner.getChannel (1);
    // Each note measured from where it starts sounding (the transport may start a block or two late).
    auto window = [&] (int note)
    {
        auto onset = (int64_t)(note * 0.5 * sr);
        const auto limit = (int64_t)((note * 0.5 + 0.2) * sr);
        while (onset < limit && std::abs (left[(size_t)onset]) < 1.0e-3f && std::abs (right[(size_t)onset]) < 1.0e-3f)
            ++onset;
        return std::pair { onset + (int64_t)(0.03 * sr), onset + (int64_t)(0.23 * sr) };
    };

    const auto [b0, e0] = window (0);
    const auto [b1, e1] = window (1);
    const auto [b2, e2] = window (2);
    const auto [b3, e3] = window (3);
    const auto plain = test::checks::rms (left, b0, e0);
    REQUIRE (plain > 0.01f);

    const auto ratio = frequencyOf (left, b1, e1, sr) / frequencyOf (left, b0, e0, sr);
    CHECK (std::abs (ratio - std::pow (2.0, 1.0 / 12.0)) < 0.003);

    const auto half = test::checks::rms (left, b2, e2) / plain;
    CHECK (half > 0.47f);
    CHECK (half < 0.53f);

    CHECK (test::checks::rms (left, b3, e3) > 0.5f * plain);
    CHECK (test::checks::rms (right, b3, e3) < 0.01f * plain);
}

TEST_CASE ("Surge XT plays per-note pan as a CLAP note expression", "[engine][pianoroll][audio]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        SKIP ("Surge XT not built");

    Roll r;
    test::RealtimeRunner runner (r.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = false });
    REQUIRE (r.rack.addChannel ("bundled.surge-xt", "Lead"));
    REQUIRE (r.roll.addNote (60, 0, 960, 0.8f));
    REQUIRE (r.roll.addNote (60, 1920, 960, 0.8f));
    REQUIRE (r.roll.setNoteValue (1, Lane::pan, -1.0f));
    r.session.getEdit().dispatchPendingUpdatesSynchronously(); // Surge's graph, before playing

    r.session.play();
    runner.start();
    runner.runUntil (1.6);
    runner.stop();
    r.session.stop();

    const auto sr = runner.getParams().sampleRate;
    auto balance = [&] (double from, double to)
    {
        const auto b = (int64_t)(from * sr), e = (int64_t)(to * sr);
        return test::checks::rms (runner.getChannel (1), b, e) / test::checks::rms (runner.getChannel (0), b, e);
    };

    CHECK (balance (0.1, 0.4) > 0.8f); // centred
    CHECK (balance (1.1, 1.4) < 0.3f); // the second note, hard left (Surge keeps a fifth on the right)
}

TEST_CASE ("Surge XT follows an MPE controller's per-note pitch bend", "[plugins][mpe][audio]")
{
    const auto file = bundledPlugins().getChildFile ("Surge XT.clap");
    if (! file.existsAsFile())
        SKIP ("Surge XT not built");

    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, file.getFullPathName());
    REQUIRE (! found.isEmpty());
    juce::String error;
    auto surge = format.createInstanceFromDescription (*found[0], 48000.0, 256, error);
    REQUIRE (surge != nullptr);
    surge->prepareToPlay (48000.0, 256);

    // The cycles per second of what Surge plays for these messages, after 0.2 s.
    auto play = [&] (const std::vector<juce::MidiMessage>& messages)
    {
        juce::AudioBuffer<float> buffer (surge->getTotalNumOutputChannels(), 256);
        std::vector<float> left;
        for (int block = 0; block < 100; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                for (auto& m : messages)
                    midi.addEvent (m, 0);
            buffer.clear();
            surge->processBlock (buffer, midi);
            left.insert (left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 256);
        }
        int crossings = 0;
        for (size_t i = 9601; i < left.size(); ++i)
            crossings += left[i - 1] < 0.0f && left[i] >= 0.0f ? 1 : 0;
        juce::MidiBuffer off;
        for (auto& m : messages)
            if (m.isNoteOn())
                off.addEvent (juce::MidiMessage::noteOff (m.getChannel(), m.getNoteNumber()), 0);
        for (int block = 0; block < 40; ++block)
        {
            buffer.clear();
            surge->processBlock (buffer, off);
            off.clear();
        }
        return crossings;
    };

    const auto octaveUp = play ({ juce::MidiMessage::noteOn (1, 72, (juce::uint8)100) });
    const auto bentUp = play ({ juce::MidiMessage::pitchWheel (5, 8192 + 2048), // +12 of MPE's 48 semitones
                                juce::MidiMessage::noteOn (5, 60, (juce::uint8)100) });
    const auto plain = play ({ juce::MidiMessage::noteOn (1, 60, (juce::uint8)100) });
    INFO ("C5 " << octaveUp << ", C4 bent an octave up " << bentUp << ", C4 " << plain);
    CHECK (std::abs (bentUp - octaveUp) <= octaveUp / 50);
    CHECK (bentUp > plain * 3 / 2);
    surge->releaseResources();
}

TEST_CASE ("Piano Roll lane screenshot for the manual", "[.screenshot][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "Lead"));
    const std::pair<int, int> melody[] = {
        { 60, 0 }, { 63, 1 }, { 67, 2 }, { 70, 3 }, { 72, 4 }, { 70, 6 }, { 67, 7 }
    };
    const float pans[] = { -0.8f, -0.4f, 0.0f, 0.4f, 0.8f, 0.3f, -0.3f };
    for (auto [pitch, eighth] : melody)
        REQUIRE (r.roll.addNote (pitch, (model::Ticks)eighth * 480, 480, 0.8f));
    for (int i = 0; i < 7; ++i)
        REQUIRE (r.roll.setNoteValue (i, Lane::pan, pans[i]));
    r.roll.setLaneMode (Lane::pan);
    r.roll.scrollToPitch (66);

    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    r.roll.setLookAndFeel (&lookAndFeel);
    r.roll.setSize (760, 360);
    const auto image = r.roll.createComponentSnapshot (r.roll.getLocalBounds(), true, 1.0f);
    r.roll.setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/piano-roll-lane.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

TEST_CASE ("Piano Roll scale screenshot for the manual", "[.screenshot][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "Lead"));
    for (auto [pitch, eighth] : { std::pair { 60, 0 }, { 63, 1 }, { 67, 2 }, { 70, 3 }, { 72, 4 }, { 70, 6 } })
        REQUIRE (r.roll.addNote (pitch, (model::Ticks)eighth * 480, 480, 0.8f));
    for (auto pitch : { 60, 63, 67 }) // a C minor chord, named above it
        REQUIRE (r.roll.addNote (pitch, (model::Ticks)7 * 480, 480, 0.8f));
    r.roll.highlightKey (model::Key { 0, "minor" });
    r.roll.scrollToPitch (66);

    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    r.roll.setLookAndFeel (&lookAndFeel);
    r.roll.setSize (760, 360);
    const auto image = r.roll.createComponentSnapshot (r.roll.getLocalBounds(), true, 1.0f);
    r.roll.setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/piano-roll-scale.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

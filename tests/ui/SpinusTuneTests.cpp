#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "ui/SpinusTune.h"
#include "ui/Theme.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace spinus;
using namespace spinus::model;

namespace
{
/** A vocal clip with two sung notes (A3 a little sharp with vibrato, then a flat C4), analysed. */
struct Vocal
{
    Vocal() : session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()))
    {
        std::vector<float> x (96000, 0.0f);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (take.getFile());
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        const float* channels[] = { x.data() };
        writer->writeFromFloatArrays (channels, 1, (int)x.size());
        writer.reset();

        Project p;
        p.settings.key = Key { 9, "minor" };
        Lane lane { p.allocateId<LaneId>(), "Vocal" };
        p.playlist.lanes = { lane };
        AudioClip audio;
        audio.file = take.getFile().getFullPathName();
        clip = p.allocateId<ClipId>();
        p.playlist.clips.push_back ({ clip, lane.id, 0, 7680, false, audio });
        session.getHistory().reset (p);

        juce::Array<juce::var> times, hz, voiced;
        for (int i = 0; i < 400; ++i)
        {
            const auto t = i * 0.005;
            const auto midi = t < 1.0 ? 57.3 + 0.3 * std::sin (2.0 * M_PI * 5.5 * t) : 59.6;
            times.add (t);
            hz.add (440.0 * std::pow (2.0, (midi - 69.0) / 12.0));
            voiced.add (0.9);
        }
        auto* track = new juce::DynamicObject();
        track->setProperty ("times", times);
        track->setProperty ("hz", hz);
        track->setProperty ("voiced", voiced);
        auto* analysis = new juce::DynamicObject();
        analysis->setProperty ("track", juce::var (track));
        analysis->setProperty ("duration", 2.0);
        analysis->setProperty ("notes", juce::JSON::parse (R"([{"start": 0.0, "end": 1.0, "pitch": 57.3},
                                                               {"start": 1.0, "end": 1.9, "pitch": 59.6}])"));
        session.getTuner().storeAnalysis (take.getFile(), juce::var (analysis));
    }

    juce::TemporaryFile take { ".wav" };
    EngineHost host { { .openAudioDevice = false } };
    Session session;
    ClipId clip;
};
} // namespace

TEST_CASE ("Spinus Tune's window: move, split, join, drift, vibrato, snap and reset, one undo step each", "[ui][tune]")
{
    Vocal v;
    ui::SpinusTuneEditor editor (v.session, v.clip, nullptr);
    editor.setSize (900, 480);
    REQUIRE (editor.isAnalysed());
    REQUIRE (editor.getNotes().size() == 2); // from the analysis, before any edit
    CHECK (editor.getNotes()[0].pitch == Catch::Approx (57.3f));

    // Up a semitone, onto a whole note: 57.3 + 1 snaps to 58.
    REQUIRE (editor.moveNotes ({ 0 }, 1.0f));
    CHECK (editor.getNotes()[0].shift == Catch::Approx (0.7f));
    const auto& tuned = std::get<AudioClip> (v.session.getProject().findClip (v.clip)->content).tune;
    CHECK (tuned.size() == 2); // the first edit writes all the notes
    // Alt: freely.
    REQUIRE (editor.moveNotes ({ 1 }, -0.25f, false));
    CHECK (editor.getNotes()[1].shift == Catch::Approx (-0.25f));

    REQUIRE (editor.splitNote (0, 0.5));
    REQUIRE (editor.getNotes().size() == 3);
    CHECK (editor.getNotes()[0].end == Catch::Approx (0.5));
    CHECK (editor.getNotes()[1].start == Catch::Approx (0.5));
    CHECK (! editor.splitNote (0, 0.495)); // too close to an edge

    REQUIRE (editor.joinNotes (0, 1));
    REQUIRE (editor.getNotes().size() == 2);
    CHECK (editor.getNotes()[0].pitch + editor.getNotes()[0].shift == Catch::Approx (58.0f));

    REQUIRE (editor.setVibrato ({ 0 }, 0.0f));
    REQUIRE (editor.setDrift ({ 0 }, 0.0f));
    CHECK (juce::approximatelyEqual (editor.getNotes()[0].vibrato, 0.0f));

    // Snap to the song's key (A minor): 57.3 -> A (57), 59.6 -> C (60).
    REQUIRE (editor.snapToKey ({ 0, 1 }));
    CHECK (editor.getNotes()[0].pitch + editor.getNotes()[0].shift == Catch::Approx (57.0f));
    CHECK (editor.getNotes()[1].pitch + editor.getNotes()[1].shift == Catch::Approx (60.0f));

    REQUIRE (editor.reset ({ 0, 1 }));
    CHECK (editor.getNotes()[0].isUnchanged());
    CHECK (! editor.reset ({ 0, 1 })); // nothing to change

    CHECK (
        v.session.getHistory().getUndoDescriptions().joinIntoString ("|") ==
        "Move note|Move note|Split note|Join notes|Change vibrato|Straighten drift|Snap notes to the key|Reset notes");
    v.session.getHistory().undo();
    CHECK (editor.getNotes()[1].pitch + editor.getNotes()[1].shift == Catch::Approx (60.0f)); // the window follows

    CHECK (editor.getStatusText().isNotEmpty());
}

TEST_CASE ("Spinus Tune editor screenshot for the manual", "[.screenshot][tune]")
{
    Vocal v;
    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    ui::SpinusTuneEditor editor (v.session, v.clip, nullptr);
    editor.setLookAndFeel (&lookAndFeel);
    editor.setSize (900, 420);
    editor.moveNotes ({ 1 }, -0.6f);
    editor.setVibrato ({ 0 }, 0.3f);
    editor.select ({ 0 });
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile ("docs/user/images/spinus-tune-editor.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    editor.setLookAndFeel (nullptr);
}

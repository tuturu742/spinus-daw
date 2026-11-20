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

TEST_CASE (
    "Spinus Tune's window: move, split, join, drift, vibrato, volume, formant, snap and reset, one undo step each",
    "[ui][tune]")
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
    REQUIRE (editor.setVolume ({ 0 }, -6.0f));
    REQUIRE (editor.setFormant ({ 1 }, 3.0f));
    CHECK (editor.getNotes()[0].gainDb == Catch::Approx (-6.0f));
    CHECK (editor.getNotes()[1].formant == Catch::Approx (3.0f));
    CHECK (editor.getNotes()[1].pitch + editor.getNotes()[1].shift ==
           Catch::Approx (59.35f).margin (0.01f)); // pitch kept
    CHECK (! editor.setVolume ({ 0 }, -6.0f));     // nothing to change
    CHECK (editor.setFormant ({ 0 }, 40.0f));
    CHECK (editor.getNotes()[0].formant == Catch::Approx (12.0f)); // at most an octave

    // Snap to the song's key (A minor): 57.3 -> A (57), 59.6 -> C (60).
    REQUIRE (editor.snapToKey ({ 0, 1 }));
    CHECK (editor.getNotes()[0].pitch + editor.getNotes()[0].shift == Catch::Approx (57.0f));
    CHECK (editor.getNotes()[1].pitch + editor.getNotes()[1].shift == Catch::Approx (60.0f));

    REQUIRE (editor.reset ({ 0, 1 }));
    CHECK (editor.getNotes()[0].isUnchanged());
    CHECK (! editor.reset ({ 0, 1 })); // nothing to change

    CHECK (
        v.session.getHistory().getUndoDescriptions().joinIntoString ("|") ==
        "Move note|Move note|Split note|Join notes|Change vibrato|Straighten drift|Change note volume|Change formant|"
        "Change formant|Snap notes to the key|Reset notes");
    v.session.getHistory().undo();
    CHECK (editor.getNotes()[1].pitch + editor.getNotes()[1].shift == Catch::Approx (60.0f)); // the window follows

    CHECK (editor.getStatusText().isNotEmpty());
}

TEST_CASE ("Spinus Tune's window: notes moved in time and made longer or shorter, their neighbours in place",
           "[ui][tune]")
{
    Vocal v; // notes at 0..1 s and 1..1.9 s of a 2 s clip
    ui::SpinusTuneEditor editor (v.session, v.clip, nullptr);
    editor.setSize (900, 480);
    REQUIRE (editor.isAnalysed());
    auto note = [&] (int i) { return editor.getNotes()[(size_t)i]; };

    REQUIRE (editor.moveInTime ({ 1 }, 0.05));
    CHECK (note (1).move == Catch::Approx (0.05));
    REQUIRE (editor.moveInTime ({ 1 }, 1.0)); // only as far as the clip's end
    CHECK (note (1).outputEnd() == Catch::Approx (2.0));
    REQUIRE (editor.moveInTime ({ 0 }, 0.3)); // only up to the next note
    CHECK (note (0).outputEnd() == Catch::Approx (note (1).outputStart()));
    REQUIRE (editor.moveInTime ({ 0 }, -1.0)); // only back to the clip's start
    CHECK (juce::exactlyEqual (note (0).move, 0.0));
    CHECK (! editor.moveInTime ({ 0 }, -1.0)); // nothing to change

    REQUIRE (editor.resizeNote (0, 0.0, 0.8)); // shorter
    CHECK (note (0).stretch == Catch::Approx (0.8));
    REQUIRE (editor.resizeNote (0, 0.0, 5.0)); // longer, up to the next note
    CHECK (note (0).outputEnd() == Catch::Approx (note (1).outputStart()));
    REQUIRE (editor.resizeNote (1, 1.6, 2.0)); // by its start: the end stays
    CHECK (note (1).outputStart() == Catch::Approx (1.6));
    CHECK (note (1).outputEnd() == Catch::Approx (2.0));
    CHECK (note (1).stretch == Catch::Approx (0.4 / 0.9));
    REQUIRE (editor.resizeNote (1, 1.9, 2.0)); // at most 4 times shorter
    CHECK (note (1).stretch == Catch::Approx (0.25));
    CHECK (note (1).outputEnd() == Catch::Approx (2.0));
    REQUIRE (editor.resizeNote (1, 1.6, 2.0));

    // Split where it sounds; joined again, it sounds where it did.
    REQUIRE (editor.splitNote (1, 1.8));
    REQUIRE (editor.getNotes().size() == 3);
    CHECK (note (2).outputStart() == Catch::Approx (1.8));
    CHECK (note (1).outputEnd() == Catch::Approx (1.8));
    CHECK (note (2).outputEnd() == Catch::Approx (2.0));
    REQUIRE (editor.joinNotes (1, 2));
    CHECK (note (1).outputStart() == Catch::Approx (1.6));
    CHECK (note (1).outputEnd() == Catch::Approx (2.0));

    REQUIRE (editor.reset ({ 0, 1 }));
    CHECK (note (0).isUnchanged());
    CHECK (note (1).isUnchanged());

    // With the mouse: sideways moves it in time (and not up or down); by its end edge, longer.
    auto event = [&] (juce::Point<float> p, juce::Point<float> down, bool dragged)
    {
        return juce::MouseEvent { juce::Desktop::getInstance().getMainMouseSource(),
                                  p,
                                  {},
                                  juce::MouseInputSource::defaultPressure,
                                  juce::MouseInputSource::defaultOrientation,
                                  juce::MouseInputSource::defaultRotation,
                                  juce::MouseInputSource::defaultTiltX,
                                  juce::MouseInputSource::defaultTiltY,
                                  &editor,
                                  &editor,
                                  juce::Time::getCurrentTime(),
                                  down,
                                  juce::Time::getCurrentTime(),
                                  1,
                                  dragged };
    };
    auto drag = [&] (juce::Point<float> from, juce::Point<float> to)
    {
        editor.mouseDown (event (from, from, false));
        editor.mouseDrag (event (to, from, true));
        editor.mouseUp (event (to, from, true));
    };
    const auto pixels = (float)editor.getPixelsPerSecond();
    const auto second = editor.getNoteBounds (1);
    drag (second.getCentre(), second.getCentre().translated (0.05f * pixels, 1.0f));
    CHECK (note (1).move == Catch::Approx (0.05).margin (0.01));
    CHECK (juce::exactlyEqual (note (1).shift, 0.0f));
    drag (second.getCentre(), second.getCentre().translated (-0.3f * pixels, 0.0f)); // not into the first note
    CHECK (juce::exactlyEqual (note (1).move, 0.0));

    const auto first = editor.getNoteBounds (0);
    drag ({ first.getRight() - 2.0f, first.getCentreY() },
          { first.getRight() - 2.0f - 0.2f * pixels, first.getCentreY() });
    CHECK (note (0).outputEnd() == Catch::Approx (0.8).margin (0.01));
    CHECK (juce::exactlyEqual (note (0).move, 0.0));

    CHECK (v.session.getHistory().getUndoDescriptions().joinIntoString ("|").endsWith (
        "Reset notes|Move note in time|Move note in time|Change note length"));
}

TEST_CASE ("Spinus Tune's window: a channel's melody as ghosts, and the vocal matched to it", "[ui][tune]")
{
    Vocal v; // notes at 0..1 s (A3, a little sharp) and 1..1.9 s (B3, sharp) of a clip at the song's start
    auto project = v.session.getProject();
    Channel melodyChannel;
    melodyChannel.id = project.allocateId<ChannelId>();
    melodyChannel.name = "Melody";
    project.channels.push_back (melodyChannel);
    Pattern melody;
    melody.id = project.allocateId<PatternId>();
    melody.notes[melodyChannel.id] = { { 0, 1920, 45 }, { 1920, 1440, 48 } }; // A2 for a second, C3 for 0.75 s
    project.patterns.push_back (melody);
    project.playlist.clips.push_back (
        { project.allocateId<ClipId>(), project.playlist.lanes[0].id, 0, 7680, false, PatternClip { melody.id, 0 } });
    v.session.getHistory().reset (project);

    ui::SpinusTuneEditor editor (v.session, v.clip, nullptr);
    editor.setSize (ui::SpinusTuneEditor::minimumWidth, 480);
    CHECK (! editor.getMelodyChannel());
    CHECK (! editor.matchToMelody ({}, true)); // no melody yet

    editor.setMelodyChannel (melodyChannel.id);
    REQUIRE (editor.getMelody().size() == 2);
    CHECK (editor.getMelody()[1].start == Catch::Approx (1.0));

    REQUIRE (editor.matchToMelody ({}, true));
    auto note = [&] (int i) { return editor.getNotes()[(size_t)i]; };
    CHECK (note (0).pitch + note (0).shift == Catch::Approx (57.0f)); // A, in the octave sung
    CHECK (note (1).pitch + note (1).shift == Catch::Approx (60.0f));
    CHECK (note (1).outputStart() == Catch::Approx (1.0));
    CHECK (note (1).outputEnd() == Catch::Approx (1.75));
    CHECK (v.session.getHistory().getUndoDescriptions().joinIntoString ("|") == "Match to melody");
    CHECK (editor.getStatusText().startsWith ("Matched 2 notes"));

    // The choice is kept for the clip: another window on it shows the same melody.
    ui::SpinusTuneEditor again (v.session, v.clip, nullptr);
    CHECK (again.getMelodyChannel() == melodyChannel.id);
}

TEST_CASE ("Spinus Tune's toolbar holds its controls, and the rows stay out of it", "[ui][tune]")
{
    Vocal v;
    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    ui::SpinusTuneEditor editor (v.session, v.clip, nullptr);
    editor.setLookAndFeel (&lookAndFeel);
    for (auto width : { ui::SpinusTuneEditor::minimumWidth, 1400 })
    {
        editor.setSize (width, 420);
        const auto bar = editor.getLocalBounds().removeFromTop (ui::SpinusTuneEditor::toolbarHeight);
        int inBar = 0;
        for (auto* child : editor.getChildren())
            if (child->isVisible() && child->getY() < bar.getBottom())
            {
                CHECK (bar.contains (child->getBounds()));
                ++inBar;
            }
        // Fit/zoom, Drift, Vibrato, Volume and Formant (each a label and a slider), Join, Snap to key, Reset; none
        // squeezed to nothing and none over another.
        CHECK (inBar == 12);
        for (auto* a : editor.getChildren())
            if (a->isVisible() && a->getY() < bar.getBottom())
            {
                CHECK (a->getWidth() >= 26);
                for (auto* b : editor.getChildren())
                    if (b != a && b->isVisible() && b->getY() < bar.getBottom())
                        CHECK (! a->getBounds().intersects (b->getBounds()));
            }

        // The bar is painted in the toolbar colour all the way down to its bottom edge, over no row of the grid.
        const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
        const auto header = lookAndFeel.getTheme().get ("panelHeader");
        for (auto y : { 0, bar.getBottom() - 1 })
        {
            CHECK (image.getPixelAt (1, y) == header); // over the keyboard, left of the zoom buttons
            CHECK (image.getPixelAt (bar.getCentreX() - 250, y) == header); // over the grid, between the controls
        }
        CHECK (image.getPixelAt (1, bar.getBottom()) != header);
    }
    editor.setLookAndFeel (nullptr);
}

TEST_CASE ("Spinus Tune editor screenshot for the manual", "[.screenshot][tune]")
{
    Vocal v;
    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    ui::SpinusTuneEditor editor (v.session, v.clip, nullptr);
    editor.setLookAndFeel (&lookAndFeel);
    editor.setSize (ui::SpinusTuneEditor::minimumWidth, 420);
    editor.moveNotes ({ 1 }, -0.6f);
    editor.setVibrato ({ 0 }, 0.3f);
    editor.setFormant ({ 0 }, 2.0f);
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

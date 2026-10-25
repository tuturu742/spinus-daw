#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"
#include "ui/AutomationEditor.h"
#include "ui/ChannelRack.h"
#include "ui/MainView.h"
#include "ui/PianoRoll.h"
#include "engine/Beats.h"
#include "ui/Playlist.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
struct Song
{
    Song() { playlist.setSize (820, 400); }

    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ChannelRack rack { session };
    Playlist playlist { session };

    const model::Playlist& model() const { return session.getProject().playlist; }
};
} // namespace

TEST_CASE ("Placing a pattern creates the tracks it needs", "[ui][playlist]")
{
    Song s;
    CHECK (! s.playlist.placePattern (0, 0)); // no pattern yet

    REQUIRE (s.rack.addChannel ({}, "Kick"));
    REQUIRE (s.rack.toggleStep (s.session.getSelectedChannel(), 0)); // creates a one-bar pattern

    REQUIRE (s.playlist.placePattern (2, 3840));
    REQUIRE (s.model().lanes.size() == 3);
    REQUIRE (s.model().clips.size() == 1);
    const auto& clip = s.model().clips[0];
    CHECK (clip.lane == s.model().lanes[2].id);
    CHECK (clip.start == 3840);
    CHECK (clip.length == 3840); // the pattern's length

    CHECK (s.session.getHistory().getUndoDescriptions().joinIntoString (" | ").endsWith (
        "Add playlist tracks | Add pattern to playlist"));

    // Placing on an existing track adds no tracks.
    REQUIRE (s.playlist.placePattern (0, 0));
    CHECK (s.model().lanes.size() == 3);
}

TEST_CASE ("Clips move, resize and delete; tracks mute", "[ui][playlist]")
{
    Song s;
    REQUIRE (s.rack.addChannel ({}, "Bass"));
    REQUIRE (s.rack.toggleStep (s.session.getSelectedChannel(), 0));
    REQUIRE (s.playlist.placePattern (0, 0));
    const auto clip = s.model().clips[0].id;

    REQUIRE (s.playlist.moveClip (clip, 1, 7680));
    CHECK (s.model().clips[0].lane == s.model().lanes[1].id);
    CHECK (s.model().clips[0].start == 7680);

    REQUIRE (s.playlist.resizeClip (clip, 1920));
    CHECK (s.model().clips[0].length == 1920);

    REQUIRE (s.playlist.setLaneMuted (1, true));
    CHECK (s.model().lanes[1].muted);
    REQUIRE (s.playlist.renameLane (1, "Drums"));
    REQUIRE (s.playlist.setLaneColour (1, 0xff4fb3a0));
    CHECK (s.model().lanes[1].name == "Drums");
    CHECK (s.model().lanes[1].colour == 0xff4fb3a0u);
    CHECK (! s.playlist.renameLane (99, "Nothing")); // only tracks that exist

    REQUIRE (s.playlist.deleteClip (clip));
    CHECK (s.model().clips.empty());

    s.session.getHistory().undo();
    REQUIRE (s.model().clips.size() == 1);
    CHECK (s.model().clips[0].length == 1920);
}

TEST_CASE ("Song mode plays the playlist where the clips are", "[ui][playlist]")
{
    Song s;
    REQUIRE (s.rack.addChannel ({}, "Lead"));
    const auto channel = s.session.getSelectedChannel();
    REQUIRE (s.rack.toggleStep (channel, 0));
    REQUIRE (s.playlist.placePattern (0, 3840)); // bar 2

    s.session.setMode (Session::Mode::song);
    const auto text = s.session.getAdapter().describe();
    CHECK (text.contains ("channel " + juce::String (channel.value) +
                          " on 'Lead': 4.000-8.000 muted=0 notes=[60@0.000+0.250v99]"));

    REQUIRE (s.playlist.setLaneMuted (0, true));
    CHECK (s.session.getAdapter().describe().contains ("4.000-8.000 muted=1"));
}

TEST_CASE ("Playlist grid coordinates", "[ui][playlist]")
{
    Song s;
    CHECK (s.playlist.rowAtY (0.0f) == 0);
    CHECK (s.playlist.rowAtY (50.0f) == 1);
    CHECK (std::abs (s.playlist.xToTick (s.playlist.tickToX (3840)) - 3840) <= 40);
    CHECK (s.playlist.snapTicks (1300) == 960);
    CHECK (s.playlist.getNumRows() >= Playlist::minimumRows);
}

TEST_CASE ("Right-dragging the Playlist's timeline loops part of the song; right-click clears it", "[ui][playlist]")
{
    Song s;
    auto* ruler = s.playlist.getChildComponent (2);
    REQUIRE (ruler != nullptr);
    auto event = [&] (float x, bool dragged, float downX)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), { x, 8.0f },
                                 juce::ModifierKeys::rightButtonModifier, juce::MouseInputSource::defaultPressure, 0.0f,
                                 0.0f, 0.0f, 0.0f, ruler, ruler, juce::Time::getCurrentTime(), { downX, 8.0f },
                                 juce::Time::getCurrentTime(), 1, dragged);
    };
    const auto from = s.playlist.tickToX (3840), to = s.playlist.tickToX (11520);
    ruler->mouseDown (event (from, false, from));
    ruler->mouseDrag (event (to, true, from));
    ruler->mouseUp (event (to, true, from));
    const auto region = s.session.getLoopRegion (Session::Mode::song);
    REQUIRE (region);
    const auto looped = region.value_or (Session::LoopRegion {});
    CHECK (std::abs (looped.start - 3840) <= 960);
    CHECK (std::abs (looped.end - 11520) <= 960);

    ruler->mouseDown (event (from, false, from));
    ruler->mouseUp (event (from, false, from));
    CHECK (! s.session.getLoopRegion (Session::Mode::song));
}

namespace
{
/** Mouse gestures on the Playlist's tracks (child 0), at song ticks and rows. */
struct LaneMouse
{
    explicit LaneMouse (Playlist& p) : playlist (p), lanes (p.getChildComponent (0)) {}

    juce::Point<float> at (model::Ticks t, int row) const
    {
        return { playlist.tickToX (t), (float)row * 44.0f + 22.0f };
    }

    juce::MouseEvent event (juce::Point<float> p, juce::ModifierKeys mods, juce::Point<float> down, bool dragged) const
    {
        return { juce::Desktop::getInstance().getMainMouseSource(),
                 p,
                 mods,
                 juce::MouseInputSource::defaultPressure,
                 juce::MouseInputSource::defaultOrientation,
                 juce::MouseInputSource::defaultRotation,
                 juce::MouseInputSource::defaultTiltX,
                 juce::MouseInputSource::defaultTiltY,
                 lanes,
                 lanes,
                 juce::Time::getCurrentTime(),
                 down,
                 juce::Time::getCurrentTime(),
                 1,
                 dragged };
    }

    void drag (juce::Point<float> from, std::initializer_list<juce::Point<float>> through, juce::ModifierKeys mods = {})
    {
        const auto button = mods.isPopupMenu() ? mods : mods.withFlags (juce::ModifierKeys::leftButtonModifier);
        lanes->mouseDown (event (from, button, from, false));
        auto last = from;
        for (auto p : through)
        {
            lanes->mouseDrag (event (p, button, from, true));
            last = p;
        }
        lanes->mouseUp (event (last, mods, from, last != from));
        playlist.handleUpdateNowIfNeeded();
    }

    Playlist& playlist;
    juce::Component* lanes;
};

/** Song with a one-bar pattern and clips at bars 1 and 3 on track 1, bar 2 on track 2. */
struct ThreeClips : Song
{
    ThreeClips()
    {
        REQUIRE (rack.addChannel ({}, "Kick"));
        REQUIRE (rack.toggleStep (session.getSelectedChannel(), 0));
        REQUIRE (playlist.placePattern (0, 0));
        REQUIRE (playlist.placePattern (0, 7680));
        REQUIRE (playlist.placePattern (1, 3840));
        for (auto& clip : model().clips)
            ids.push_back (clip.id);
    }

    const model::Clip& clip (size_t i) const { return *session.getProject().findClip (ids[i]); }
    int steps() { return session.getHistory().getUndoDescriptions().size(); }

    std::vector<model::ClipId> ids;
};
} // namespace

TEST_CASE ("Playlist selection: box, Shift+click, Ctrl+A, Escape; it follows undo", "[ui][playlist]")
{
    ThreeClips s;
    LaneMouse mouse (s.playlist);

    // Ctrl+drag a box over the first track's clips.
    mouse.drag (mouse.at (100, 0), { mouse.at (11000, 0) }, juce::ModifierKeys::ctrlModifier);
    CHECK (s.playlist.getSelection() == std::set<model::ClipId> { s.ids[0], s.ids[1] });

    // Shift+click adds or removes one.
    mouse.drag (mouse.at (5000, 1), {}, juce::ModifierKeys::shiftModifier);
    CHECK (s.playlist.getSelection().size() == 3);
    mouse.drag (mouse.at (1000, 0), {}, juce::ModifierKeys::shiftModifier);
    CHECK (s.playlist.getSelection() == std::set<model::ClipId> { s.ids[1], s.ids[2] });

    CHECK (mouse.lanes->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    CHECK (s.playlist.getSelection().empty());
    CHECK (mouse.lanes->keyPressed (juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0)));
    CHECK (s.playlist.getSelection().size() == 3);

    // Delete removes them (one step); undo brings them back, and deleted clips leave the selection.
    CHECK (mouse.lanes->keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));
    CHECK (s.model().clips.empty());
    s.playlist.handleUpdateNowIfNeeded();
    CHECK (s.playlist.getSelection().empty());
    REQUIRE (s.session.getHistory().undo());
    CHECK (s.model().clips.size() == 3);
}

TEST_CASE ("Playlist keys: copy, cut, paste, duplicate, and Shift+arrows move the selection", "[ui][playlist]")
{
    ThreeClips s;
    LaneMouse mouse (s.playlist);
    using KP = juce::KeyPress;
    constexpr auto ctrl = juce::ModifierKeys::ctrlModifier, shift = juce::ModifierKeys::shiftModifier;
    const auto bar = (model::Ticks)3840;

    // Ctrl+B: the first track's two clips (bars 1 and 3) again right after themselves (three bars on).
    s.playlist.setSelection ({ s.ids[0], s.ids[1] });
    const auto before = s.steps();
    CHECK (mouse.lanes->keyPressed (KP ('b', ctrl, 0)));
    REQUIRE (s.model().clips.size() == 5);
    CHECK (s.steps() == before + 1);
    CHECK (s.session.getHistory().getUndoDescriptions().strings.getLast() == "Duplicate clips");
    std::set<model::Ticks> copies;
    for (auto id : s.playlist.getSelection())
    {
        const auto* copy = s.session.getProject().findClip (id);
        REQUIRE (copy != nullptr);
        CHECK (copy->lane == s.clip (0).lane);
        CHECK (copy->content == s.clip (0).content);
        copies.insert (copy->start);
    }
    CHECK (copies == std::set<model::Ticks> { bar * 3, bar * 5 });

    // Shift+arrows: a grid step later, then a track down; the selection moves, one step each.
    s.playlist.setSnap (bar);
    CHECK (mouse.lanes->keyPressed (KP (KP::rightKey, shift, 0)));
    CHECK (mouse.lanes->keyPressed (KP (KP::downKey, shift, 0)));
    for (auto id : s.playlist.getSelection())
    {
        const auto* moved = s.session.getProject().findClip (id);
        CHECK ((moved->start == bar * 4 || moved->start == bar * 6));
        CHECK (moved->lane == s.clip (2).lane);
    }
    CHECK (mouse.lanes->keyPressed (KP (KP::upKey, shift, 0)));
    CHECK (mouse.lanes->keyPressed (KP (KP::upKey, shift, 0))); // above the first track: stays
    for (auto id : s.playlist.getSelection())
        CHECK (s.session.getProject().findClip (id)->lane == s.clip (0).lane);

    // Ctrl+C then Ctrl+V on the same clips: right after them. Ctrl+X then Ctrl+V: back where they were.
    s.playlist.setSelection ({ s.ids[2] });
    CHECK (mouse.lanes->keyPressed (KP ('c', ctrl, 0)));
    CHECK (Playlist::getCopiedClips().size() == 1);
    CHECK (mouse.lanes->keyPressed (KP ('v', ctrl, 0)));
    REQUIRE (s.playlist.getSelection().size() == 1);
    CHECK (s.session.getProject().findClip (*s.playlist.getSelection().begin())->start == bar * 2);

    s.playlist.setSelection ({ s.ids[2] });
    CHECK (mouse.lanes->keyPressed (KP ('x', ctrl, 0)));
    CHECK (s.session.getProject().findClip (s.ids[2]) == nullptr);
    CHECK (s.session.getHistory().getUndoDescriptions().strings.getLast() == "Cut clip");
    CHECK (mouse.lanes->keyPressed (KP ('v', ctrl, 0)));
    const auto* back = s.session.getProject().findClip (*s.playlist.getSelection().begin());
    CHECK (back->start == bar);
    CHECK (back->lane == s.model().lanes[1].id);

    // Ctrl+D selects nothing.
    CHECK (mouse.lanes->keyPressed (KP ('d', ctrl, 0)));
    CHECK (s.playlist.getSelection().empty());
}

TEST_CASE ("Playlist: dragging a selected clip moves and resizes the selection, one step each", "[ui][playlist]")
{
    ThreeClips s;
    LaneMouse mouse (s.playlist);
    s.playlist.setSelection ({ s.ids[0], s.ids[2] });
    const auto before = s.steps();

    // Drag the first clip one bar right and one track down: the other selected clip goes along.
    mouse.drag (mouse.at (1000, 0), { mouse.at (1000 + 3840, 1) });
    CHECK (s.clip (0).start == 3840);
    CHECK (s.clip (0).lane == s.model().lanes[1].id);
    CHECK (s.clip (2).start == 7680);
    CHECK (s.clip (2).lane == s.model().lanes[2].id); // a new track below
    CHECK (s.clip (1).start == 7680);                 // not selected: stays
    CHECK (s.clip (1).lane == s.model().lanes[0].id);
    CHECK (s.session.getHistory().getUndoDescriptions().strings.getLast() == "Move clips");

    // Drag the right edge of a selected clip: both get half a bar shorter.
    const auto edge = juce::Point<float> (s.playlist.tickToX (3840 + 3840) - 3.0f, 44.0f + 22.0f);
    mouse.drag (edge, { edge.translated (s.playlist.tickToX (1920) - s.playlist.tickToX (3840), 0.0f) });
    CHECK (s.clip (0).length == 1920);
    CHECK (s.clip (2).length == 1920);
    CHECK (s.clip (1).length == 3840);
    CHECK (s.steps() == before + 3); // new track, move, resize

    // A clip that is not selected moves on its own.
    mouse.drag (mouse.at (8000, 0), { mouse.at (8000 + 3840, 0) });
    CHECK (s.clip (1).start == 11520);
    CHECK (s.playlist.getSelection() == std::set<model::ClipId> { s.ids[1] });
}

TEST_CASE ("Playlist: right-drag deletes every clip it touches, Slice cuts, Paint repeats the pattern",
           "[ui][playlist]")
{
    ThreeClips s;
    LaneMouse mouse (s.playlist);

    // Right-drag across the first track: both its clips go, in one step.
    mouse.drag (mouse.at (1000, 0), { mouse.at (5000, 0), mouse.at (9000, 0) },
                juce::ModifierKeys::rightButtonModifier);
    CHECK (s.model().clips.size() == 1);
    CHECK (s.session.getHistory().getUndoDescriptions().strings.getLast() == "Delete clips");
    REQUIRE (s.session.getHistory().undo());

    // Slice: a line down both tracks at beat 3 of bar 2 cuts only the clip it crosses there.
    s.playlist.setTool (Playlist::Tool::slice);
    mouse.drag (mouse.at (3840 + 1920, 0), { mouse.at (3840 + 1920, 1) });
    REQUIRE (s.model().clips.size() == 4);
    CHECK (s.clip (2).length == 1920);
    const auto& cut = s.model().clips.back();
    CHECK (cut.start == 3840 + 1920);
    CHECK (std::get<model::PatternClip> (cut.content).offset == 1920);

    // Paint: the pattern again and again along track 4, as far as the mouse goes.
    s.playlist.setTool (Playlist::Tool::paint);
    mouse.drag (mouse.at (0, 3), { mouse.at (5000, 3), mouse.at (12000, 3) });
    int onTrack4 = 0;
    for (auto& clip : s.model().clips)
        if (clip.lane == s.model().lanes[3].id)
            ++onTrack4;
    CHECK (onTrack4 == 4); // bars 1 to 4
    CHECK (s.session.getHistory().getUndoDescriptions().strings.getLast() == "Paint pattern");

    // No grid: Alt-free placement, and the grid still draws.
    s.playlist.setSnap (0);
    CHECK (s.playlist.snapTicks (1301) == 1301);
    CHECK (s.playlist.createComponentSnapshot (s.playlist.getLocalBounds()).isValid());
}

TEST_CASE ("Choosing a tool lights only its button (Playlist, Piano Roll, Automation editor)", "[ui][playlist]")
{
    Song s;
    auto button = [] (juce::Component& parent, const juce::String& text) -> juce::Button*
    {
        for (auto* c : parent.getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == text)
                return b;
        return nullptr;
    };

    PianoRoll roll (s.session);
    AutomationEditor automation (s.session);
    for (auto* panel : std::initializer_list<juce::Component*> { &s.playlist, &roll, &automation })
    {
        auto* draw = button (*panel, "Draw");
        auto* select = button (*panel, "Select");
        REQUIRE ((draw != nullptr && select != nullptr));
        CHECK (draw->getToggleState());
        select->setToggleState (true, juce::sendNotification); // what a click does
        CHECK (select->getToggleState());
        CHECK (! draw->getToggleState());
    }
    CHECK (s.playlist.getTool() == Playlist::Tool::select);
    CHECK (roll.getTool() == PianoRoll::Tool::select);
    CHECK (automation.getTool() == AutomationEditor::Tool::select);
}

TEST_CASE ("A recorded take goes where it was played, even if it began before the song", "[ui][playlist][audiorecord]")
{
    Song s;
    const auto file = juce::File::createTempFile (".wav");
    {
        juce::AudioBuffer<float> audio (1, 96000); // 2 s at 48 kHz
        audio.clear();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    // 120 BPM: 1 s is two beats (1920 ticks), 2 s four.
    REQUIRE (s.playlist.addRecordedTake (file, 1.0));
    REQUIRE (s.model().clips.size() == 1);
    CHECK (s.model().clips[0].start == 1920);
    CHECK (s.model().clips[0].length == 3840);

    // Begun a quarter of a second before the song's start: from the start, without that quarter second.
    REQUIRE (s.playlist.addRecordedTake (file, -0.25));
    REQUIRE (s.model().clips.size() == 2);
    const auto& early = s.model().clips[1];
    CHECK (early.start == 0);
    CHECK (std::get<model::AudioClip> (early.content).offset == 480);
    CHECK (early.length == 3840 - 480);
    CHECK (early.lane != s.model().clips[0].lane); // each take on its own track
    file.deleteFile();
}

TEST_CASE ("Make unique gives a clip its own copy of the pattern, the one edited now", "[ui][playlist]")
{
    Song s;
    REQUIRE (s.rack.addChannel ({}, "Kick"));
    REQUIRE (s.rack.toggleStep (s.session.getSelectedChannel(), 0));
    REQUIRE (s.playlist.placePattern (0, 0));
    REQUIRE (s.playlist.placePattern (0, 3840));
    const auto original = s.session.getCurrentPattern();
    const auto second = s.model().clips[1].id;
    REQUIRE (s.playlist.makeClipUnique (second));
    const auto copy = std::get<model::PatternClip> (s.session.getProject().findClip (second)->content).pattern;
    CHECK (copy != original);
    CHECK (s.session.getCurrentPattern() == copy);
    CHECK (std::get<model::PatternClip> (s.model().clips[0].content).pattern == original);
}

TEST_CASE ("Markers are added, renamed, moved and removed from the Playlist, and jumped to", "[ui][playlist]")
{
    Song s;
    REQUIRE (s.playlist.addMarker (3840, "Chorus"));
    REQUIRE (s.playlist.addMarker (0));
    REQUIRE (s.model().markers.size() == 2);
    const auto chorus = s.model().markers[1].id;
    CHECK (s.model().markers[0].name == "Marker 2");
    REQUIRE (s.playlist.renameMarker (chorus, "Drop"));
    REQUIRE (s.playlist.moveMarker (chorus, 7680));
    CHECK (s.playlist.findMarker (chorus)->name == "Drop");
    CHECK (s.playlist.findMarker (chorus)->at == 7680);
    s.playlist.jumpTo (7680);
    auto& edit = s.session.getEdit();
    CHECK (std::abs (edit.getTransport().getPosition().inSeconds() - engine::ticksToTime (edit, 7680.0).inSeconds()) <
           0.001);
    REQUIRE (s.playlist.removeMarker (chorus));
    CHECK (s.model().markers.size() == 1);
    s.session.getHistory().undo();
    CHECK (s.model().markers.size() == 2);
}

TEST_CASE ("Track groups: grouped tracks fold away and mute with their group", "[ui][playlist][groups]")
{
    Song s;
    REQUIRE (s.rack.addChannel ({}, "Kick"));
    REQUIRE (s.rack.toggleStep (s.session.getSelectedChannel(), 0));
    for (int row = 0; row < 4; ++row) // Drums, Kick, Snare, Bass
        REQUIRE (s.playlist.placePattern (row, 0));
    REQUIRE (s.playlist.renameLane (0, "Drums"));
    REQUIRE (s.playlist.renameLane (3, "Bass"));
    const auto lane = [&s] (int i) { return s.model().lanes[(size_t)i].id; };
    const auto& project = s.session.getProject();

    // Kick under Drums; Snare joins the same group (the track above it is in it).
    REQUIRE (s.playlist.groupLaneWithAbove (1));
    REQUIRE (s.playlist.groupLaneWithAbove (2));
    CHECK (s.model().lanes[1].group == lane (0));
    CHECK (s.model().lanes[2].group == lane (0));
    CHECK (project.getLaneDepth (lane (2)) == 1);

    // Folded: Bass moves up to the second row; a pattern placed there goes on Bass.
    REQUIRE (s.playlist.setLaneCollapsed (0, true));
    CHECK (s.playlist.getVisibleLanes() == std::vector<int> { 0, 3 });
    CHECK (s.playlist.laneIndexOfRow (1) == 3);
    CHECK (s.playlist.rowOfLaneIndex (2) == -1);
    REQUIRE (s.playlist.placePattern (1, 3840));
    CHECK (s.model().clips.back().lane == lane (3));
    // Below the shown tracks, new tracks are made after all of them (hidden ones included).
    REQUIRE (s.playlist.placePattern (2, 0));
    CHECK (s.model().lanes.size() == 5);
    CHECK (s.model().clips.back().lane == lane (4));

    // Muting the group mutes what is in it.
    REQUIRE (s.playlist.setLaneMuted (0, true));
    CHECK (project.isLaneMuted (lane (1)));
    CHECK (project.isLaneMuted (lane (2)));
    CHECK (! project.isLaneMuted (lane (3)));
    const auto playing = s.session.getAdapter().describe();
    CHECK (playing.contains ("muted"));

    // A track can't go under one of its own; removing a group frees what was in it.
    REQUIRE (s.playlist.setLaneCollapsed (0, false));
    juce::String error;
    auto loop = commands::CommandRegistry::get().create (
        "lane.set",
        juce::JSON::parse ("{\"lane\": " + juce::String (lane (0).value) +
                           ", \"group\": " + juce::String (lane (1).value) + "}"),
        error);
    REQUIRE (loop != nullptr);
    CHECK (s.session.getHistory().perform (std::move (loop)).failed());
    REQUIRE (s.playlist.ungroupLane (2));
    CHECK (! s.model().lanes[2].group.isValid());
    REQUIRE (s.playlist.removeLane (0));
    CHECK (! s.model().lanes[0].group.isValid()); // Kick, out of the removed group

    // Saved with the project.
    REQUIRE (s.playlist.groupLaneWithAbove (1));
    REQUIRE (s.playlist.setLaneCollapsed (0, true));
    const auto text = model::toJsonText (project);
    model::Project loaded;
    REQUIRE (model::fromJsonText (text, loaded).wasOk());
    CHECK (loaded.playlist.lanes == project.playlist.lanes);
}

TEST_CASE ("Clicking a pattern clip makes its pattern the current one, with any tool", "[ui][playlist]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    MainView view { session };
    auto& workspace = view.getWorkspace();
    auto& playlist = *dynamic_cast<Playlist*> (workspace.getPanel ("playlist"));
    auto& rack = *dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack"));
    workspace.showPanel ("playlist");
    workspace.showPanel ("channelRack");
    playlist.setSize (820, 400);
    const auto& clips = session.getProject().playlist.clips;
    auto patternOf = [] (const model::Clip& clip) { return std::get<model::PatternClip> (clip.content).pattern; };

    // The top bar's pattern list.
    std::function<juce::ComboBox*(juce::Component&)> findPatternBox = [&] (juce::Component& c) -> juce::ComboBox*
    {
        if (auto* box = dynamic_cast<juce::ComboBox*> (&c);
            box != nullptr && box->getHelpText().startsWith ("Current pattern"))
            return box;
        for (auto* child : c.getChildren())
            if (auto* found = findPatternBox (*child))
                return found;
        return nullptr;
    };
    auto* patternBox = findPatternBox (view);
    REQUIRE (patternBox != nullptr);
    auto shownInTopBar = [&]
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20); // the top bar updates on change
        return model::PatternId { (uint32_t)patternBox->getSelectedId() };
    };

    // Pattern A has a step on 1, pattern B a step on 5. A at bar 1, B at bar 3 of track 1.
    REQUIRE (rack.addChannel ({}, "Kick"));
    const auto kick = session.getSelectedChannel();
    REQUIRE (rack.toggleStep (kick, 0)); // creates pattern A
    const auto a = session.getCurrentPattern();
    REQUIRE (playlist.placePattern (0, 0));
    juce::String error;
    auto add =
        commands::CommandRegistry::get().create ("pattern.add", juce::JSON::parse (R"({"length": 3840})"), error);
    REQUIRE (add != nullptr);
    auto* raw = add.get();
    REQUIRE (session.getHistory().perform (std::move (add)).wasOk());
    const model::PatternId b { (uint32_t)(int)raw->getResult()["pattern"] };
    session.setCurrentPattern (b);
    REQUIRE (rack.toggleStep (kick, 4));
    REQUIRE (playlist.placePattern (0, 7680));
    REQUIRE (clips.size() == 2);
    const auto clipA = clips[0].id, clipB = clips[1].id;
    CHECK (shownInTopBar() == b);

    // An audio clip on track 2.
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-click-pattern-" + juce::Uuid().toString() + ".wav");
    {
        juce::AudioBuffer<float> audio (1, 48000);
        audio.clear();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    REQUIRE (playlist.addAudioFile (1, 0, file));
    session.setCurrentPattern (b);

    LaneMouse mouse (playlist);
    const auto steps = session.getHistory().getUndoDescriptions().size();

    // Draw: a click on A's clip makes A current: the top bar and the Channel Rack show it. Nothing moves,
    // nothing is added to the history, and the clip is selected as before.
    mouse.drag (mouse.at (1000, 0), {});
    CHECK (session.getCurrentPattern() == a);
    CHECK (shownInTopBar() == a);
    CHECK (rack.isStepOn (kick, 0));
    CHECK (! rack.isStepOn (kick, 4));
    CHECK (session.getProject().findClip (clipA)->start == 0);
    CHECK (session.getHistory().getUndoDescriptions().size() == steps);
    CHECK (playlist.getSelection() == std::set<model::ClipId> { clipA });

    // An audio clip leaves the current pattern alone.
    mouse.drag (mouse.at (1000, 1), {});
    CHECK (session.getCurrentPattern() == a);

    // Dragging B's clip moves it and makes B current.
    mouse.drag (mouse.at (8000, 0), { mouse.at (8000 + 3840, 0) });
    CHECK (session.getCurrentPattern() == b);
    CHECK (shownInTopBar() == b);
    CHECK (rack.isStepOn (kick, 4));
    CHECK (session.getProject().findClip (clipB)->start == 11520);

    // Clicking empty space still places the current pattern (B).
    mouse.drag (mouse.at (0, 2), {});
    REQUIRE (clips.size() == 4);
    CHECK (patternOf (clips.back()) == b);

    // Paint: a click on A's clip makes A current.
    playlist.setTool (Playlist::Tool::paint);
    mouse.drag (mouse.at (1000, 0), {});
    CHECK (session.getCurrentPattern() == a);

    // Select: a click on B's clip makes B current and selects it.
    playlist.setTool (Playlist::Tool::select);
    mouse.drag (mouse.at (12000, 0), {});
    CHECK (session.getCurrentPattern() == b);
    CHECK (playlist.getSelection() == std::set<model::ClipId> { clipB });

    // Slice: cutting A's clip still cuts it, and makes A current.
    playlist.setTool (Playlist::Tool::slice);
    mouse.drag (mouse.at (1920, 0), { mouse.at (1920, 0) });
    CHECK (session.getCurrentPattern() == a);
    CHECK (clips.size() == 5);
    CHECK (session.getProject().findClip (clipA)->length == 1920);

    // Right-click deletes B's clip without making B current.
    playlist.setTool (Playlist::Tool::draw);
    mouse.drag (mouse.at (12000, 0), {}, juce::ModifierKeys::rightButtonModifier);
    CHECK (session.getProject().findClip (clipB) == nullptr);
    CHECK (session.getCurrentPattern() == a);

    file.deleteFile();
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "ui/PianoRoll.h"
#include "ui/Playlist.h"
#include "ui/TimelineControls.h"

using namespace spinus;
using namespace spinus::ui;
using Catch::Approx;

TEST_CASE ("Zooming keeps the time under the mouse in place", "[ui][zoom]")
{
    double pixelsPerBeat = 24.0;
    const auto tickAt = [&] (double scroll, double x) { return x / pixelsPerBeat * 960.0 + scroll; };
    const auto before = tickAt (1920.0, 300.0);
    const auto scroll = timeline::zoomAround (pixelsPerBeat, 1920.0, 300.0, 2.0, { 2.0, 400.0 }, 960.0);
    CHECK (pixelsPerBeat == Approx (48.0));
    CHECK (tickAt (scroll, 300.0) == Approx (before));

    // Within the limits, and never before the start.
    timeline::zoomAround (pixelsPerBeat, 0.0, 0.0, 100.0, { 2.0, 400.0 }, 960.0);
    CHECK (pixelsPerBeat == Approx (400.0));
    CHECK (timeline::zoomAround (pixelsPerBeat, 0.0, 400.0, 0.01, { 2.0, 400.0 }, 960.0) == Approx (0.0));
    CHECK (pixelsPerBeat == Approx (4.0));
}

TEST_CASE ("Following turns the page when the play position leaves the view", "[ui][zoom]")
{
    CHECK_FALSE (timeline::followPlayhead (500.0, 0.0, 1000.0).has_value());
    CHECK_FALSE (timeline::followPlayhead (-1.0, 0.0, 1000.0).has_value()); // not playing
    REQUIRE (timeline::followPlayhead (990.0, 0.0, 1000.0).has_value());    // at the edge: the next page
    CHECK (timeline::followPlayhead (990.0, 0.0, 1000.0).value_or (-1.0) == Approx (960.0));
    // Looping back to the start: back to the start.
    CHECK (timeline::followPlayhead (0.0, 4000.0, 1000.0).value_or (-1.0) == Approx (0.0));
}

TEST_CASE ("The zoom keys", "[ui][zoom]")
{
    const auto ctrl = juce::ModifierKeys::commandModifier;
    CHECK (timeline::zoomKeyOf (juce::KeyPress ('=', ctrl, '=')) == timeline::ZoomKey::in);
    CHECK (timeline::zoomKeyOf (juce::KeyPress ('+', ctrl, '+')) == timeline::ZoomKey::in);
    CHECK (timeline::zoomKeyOf (juce::KeyPress ('-', ctrl, '-')) == timeline::ZoomKey::out);
    CHECK (timeline::zoomKeyOf (juce::KeyPress (juce::KeyPress::pageUpKey)) == timeline::ZoomKey::in);
    CHECK (timeline::zoomKeyOf (juce::KeyPress (juce::KeyPress::pageDownKey)) == timeline::ZoomKey::out);
    CHECK (timeline::zoomKeyOf (juce::KeyPress (juce::KeyPress::pageUpKey, ctrl, 0)) == timeline::ZoomKey::none);
    CHECK (timeline::zoomKeyOf (juce::KeyPress ('0', ctrl, '0')) == timeline::ZoomKey::fit);
    // Without Ctrl, - and = belong to the typing keyboard (octave down and up).
    CHECK (timeline::zoomKeyOf (juce::KeyPress ('-', 0, '-')) == timeline::ZoomKey::none);
    CHECK (timeline::zoomKeyOf (juce::KeyPress ('a', ctrl, 'a')) == timeline::ZoomKey::none);
}

namespace
{
struct Editors
{
    Editors()
    {
        playlist.setSize (820, 400);
        roll.setSize (760, 420);
    }

    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ChannelRack rack { session };
    Playlist playlist { session };
    PianoRoll roll { session };
};

const auto ctrl = juce::ModifierKeys::commandModifier;
} // namespace

TEST_CASE ("The Playlist zooms with its buttons' keys, fits the song and follows it", "[ui][zoom][playlist]")
{
    Editors e;
    const auto start = e.playlist.getPixelsPerBeat();
    REQUIRE (e.playlist.keyPressed (juce::KeyPress ('=', ctrl, '=')));
    CHECK (e.playlist.getPixelsPerBeat() == Approx (start * timeline::zoomStep));
    REQUIRE (e.playlist.keyPressed (juce::KeyPress ('-', ctrl, '-')));
    CHECK (e.playlist.getPixelsPerBeat() == Approx (start));

    // Taller and shorter tracks.
    const auto rows = e.playlist.getRowHeight();
    e.playlist.zoomRows (2.0);
    CHECK (e.playlist.getRowHeight() == rows * 2);
    e.playlist.zoomRows (0.01);
    CHECK (e.playlist.getRowHeight() == 22);

    // Fit: a clip 32 bars in fills the view.
    REQUIRE (e.rack.addChannel ({}, "Kick"));
    REQUIRE (e.rack.toggleStep (e.session.getSelectedChannel(), 0));
    REQUIRE (e.playlist.placePattern (0, (model::Ticks)3840 * 31));
    REQUIRE (e.playlist.keyPressed (juce::KeyPress ('0', ctrl, '0')));
    CHECK (e.playlist.getScrollTicks() == Approx (0.0));
    CHECK (e.playlist.getVisibleTicks() >= 3840.0 * 32);
    CHECK (e.playlist.getVisibleTicks() < 3840.0 * 34);

    // Following: the page turns when the song position passes the view's end; not when it is off.
    e.playlist.zoom (8.0, 0.0f); // around the start: the view still begins there
    CHECK (e.playlist.getScrollTicks() == Approx (0.0));
    const auto visible = e.playlist.getVisibleTicks();
    e.playlist.setFollowing (false);
    e.playlist.follow (visible * 1.5);
    CHECK (e.playlist.getScrollTicks() == Approx (0.0));
    e.playlist.setFollowing (true);
    e.playlist.follow (visible * 0.5);
    CHECK (e.playlist.getScrollTicks() == Approx (0.0));
    e.playlist.follow (visible * 1.5);
    CHECK (e.playlist.getScrollTicks() > visible);
    CHECK (e.playlist.getScrollTicks() < visible * 1.5);
}

TEST_CASE ("The Piano Roll zooms, fits the pattern and follows it", "[ui][zoom][piano-roll]")
{
    Editors e;
    REQUIRE (e.rack.addChannel ({}, "Lead"));
    REQUIRE (e.rack.toggleStep (e.session.getSelectedChannel(), 0));
    e.roll.handleUpdateNowIfNeeded();

    const auto start = e.roll.getPixelsPerBeat();
    REQUIRE (e.roll.keyPressed (juce::KeyPress ('+', ctrl, '+')));
    CHECK (e.roll.getPixelsPerBeat() == Approx (start * timeline::zoomStep));

    const auto keys = e.roll.getKeyHeight();
    e.roll.zoomKeys (timeline::zoomStep);
    CHECK (e.roll.getKeyHeight() > keys);
    e.roll.zoomKeys (0.01);
    CHECK (e.roll.getKeyHeight() == 6);

    // Fit: the one-bar pattern across the grid.
    REQUIRE (e.roll.keyPressed (juce::KeyPress ('0', ctrl, '0')));
    CHECK (e.roll.getVisibleTicks() >= 3840.0);
    CHECK (e.roll.getVisibleTicks() < 3840.0 * 1.1);

    e.roll.zoom (4.0, 0.0f);
    const auto visible = e.roll.getVisibleTicks();
    e.roll.setFollowing (true);
    e.roll.follow (visible * 2.0);
    CHECK (e.roll.getScrollTicks() > visible);
    e.roll.follow (0.0); // the pattern loops back
    CHECK (e.roll.getScrollTicks() == Approx (0.0));
}

TEST_CASE ("Zoom controls: the buttons zoom and fit, Follow is remembered", "[ui][zoom]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto& settings = host.getEngine().getPropertyStorage().getPropertiesFile();
    const auto key = "zoomTestFollows";
    settings.setValue (key, false);
    {
        TimelineControls controls (&settings, key);
        CHECK_FALSE (controls.isFollowing());
        controls.setFollowing (true);
        CHECK (settings.getBoolValue (key, false));

        double zoomed = 1.0;
        int fits = 0;
        controls.onZoom = [&] (double factor) { zoomed *= factor; };
        controls.onFit = [&] { ++fits; };
        controls.setSize (controls.getPreferredWidth(), 24);
        for (auto* child : controls.getChildren())
            if (auto* button = dynamic_cast<juce::Button*> (child);
                button != nullptr && button->getButtonText() != "Follow")
                button->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        CHECK (zoomed == Approx (1.0)); // in and out
        CHECK (fits == 1);
    }
    {
        TimelineControls controls (&settings, key);
        CHECK (controls.isFollowing());
    }
    settings.removeValue (key);

    TimelineControls noFollow (nullptr, {});
    CHECK_FALSE (noFollow.isFollowing());
}

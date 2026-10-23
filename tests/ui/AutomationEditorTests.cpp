#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/Automation.h"
#include "ui/AutomationEditor.h"
#include "ui/ChannelRack.h"

using namespace spinus;
using namespace spinus::ui;
namespace te = tracktion;

namespace
{
using Kind = model::ParameterTarget::Kind;

struct Fixture
{
    Fixture()
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, host.getEngine());
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        rack = std::make_unique<ChannelRack> (*session);
        editor = std::make_unique<AutomationEditor> (*session);
        editor->setSize (800, 330);
        REQUIRE (rack->addChannel ("spinus.3x-osc", "Lead"));
        target.kind = Kind::insertVolume;
        target.insert = session->getProject().channels.front().targetInsert;
        clip = session->createAutomationClip (target);
        REQUIRE (clip.isValid());
        editor->setClip (clip);
    }

    ~Fixture()
    {
        editor = nullptr;
        rack = nullptr;
        session = nullptr;
    }

    const std::vector<model::AutomationPoint>& points() const
    {
        return std::get<model::AutomationClip> (session->getProject().findClip (clip)->content).points;
    }

    bool perform (const juce::String& name, const juce::String& json)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
        INFO (error);
        return command != nullptr && session->getHistory().perform (std::move (command)).wasOk();
    }

    size_t undoSteps() const { return (size_t)session->getHistory().getUndoDescriptions().size(); }

    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<ChannelRack> rack;
    std::unique_ptr<AutomationEditor> editor;
    model::ParameterTarget target;
    model::ClipId clip;
};

struct Mouse
{
    explicit Mouse (AutomationEditor& e) : editor (e), canvas (e.getChildComponent (0)) {}

    juce::Point<float> at (model::Ticks t, float value) const
    {
        return { editor.tickToX ((double)t), editor.valueToY (value) };
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
                 canvas,
                 canvas,
                 juce::Time::getCurrentTime(),
                 down,
                 juce::Time::getCurrentTime(),
                 1,
                 dragged };
    }

    void drag (juce::Point<float> from, std::initializer_list<juce::Point<float>> through, juce::ModifierKeys mods = {},
               juce::ModifierKeys during = {})
    {
        const auto button = mods.isPopupMenu() ? mods : mods.withFlags (juce::ModifierKeys::leftButtonModifier);
        canvas->mouseDown (event (from, button, from, false));
        auto last = from;
        for (auto p : through)
        {
            canvas->mouseDrag (event (p, button.withFlags (during.getRawFlags()), from, true));
            last = p;
        }
        canvas->mouseUp (event (last, mods.withFlags (during.getRawFlags()), from, last != from));
    }

    AutomationEditor& editor;
    juce::Component* canvas;
};
} // namespace

TEST_CASE ("Automation curves are drawn as the engine plays them, bends included", "[ui][automation]")
{
    Fixture f;
    f.session->setMode (Session::Mode::song);
    // Rising and falling parts, straight and bent a little and a lot, both ways.
    REQUIRE (f.perform ("clip.setPoints", "{\"clip\": " + juce::String ((int)f.clip.value) + R"(, "points": [
        {"time": 0, "value": 0.1, "curve": 0.3}, {"time": 1920, "value": 0.9, "curve": -0.8},
        {"time": 3840, "value": 0.2, "curve": 0.8}, {"time": 5760, "value": 0.7, "curve": -0.3},
        {"time": 7680, "value": 0.4}, {"time": 9600, "value": 0.6}]})"));

    auto* parameter = f.session->getAdapter().findParameter (f.target);
    REQUIRE (parameter != nullptr);
    auto& curve = parameter->getCurve();
    for (int eighth = 0; eighth <= 96; ++eighth)
    {
        const auto beats = eighth * 0.125;
        const auto time = f.session->getEdit().tempoSequence.toTime (te::BeatPosition::fromBeats (beats));
        const auto engine = parameter->valueRange.convertTo0to1 (curve.getValueAt (time, 0.0f));
        const auto shown = model::automationValueAt (f.points(), beats * model::ticksPerQuarterNote);
        INFO ("beat " << beats);
        CHECK (std::abs (engine - shown) < 0.01f);
    }
}

TEST_CASE ("Automation shapes", "[ui][automation]")
{
    using Shape = model::AutomationShape;
    const auto ramp = model::makeAutomationShape (Shape::rampUp, 0, 3840, 0, 0.2f, 0.9f);
    REQUIRE (ramp.size() == 2);
    CHECK (juce::approximatelyEqual (ramp.back().value, 0.9f));

    // A sine every beat over a bar: 4 cycles, between the two values, ending on the middle.
    const auto sine = model::makeAutomationShape (Shape::sine, 0, 3840, 960, 0.0f, 1.0f);
    CHECK (sine.size() == 33);
    CHECK (std::abs (model::automationValueAt (sine, 240.0) - 1.0f) < 0.001f);
    CHECK (std::abs (model::automationValueAt (sine, 720.0) - 0.0f) < 0.001f);

    // Square: jumps (two points at one time) every half cycle.
    const auto square = model::makeAutomationShape (Shape::square, 0, 1920, 960, 0.25f, 0.75f);
    CHECK (std::abs (model::automationValueAt (square, 400.0) - 0.75f) < 0.001f);
    CHECK (std::abs (model::automationValueAt (square, 700.0) - 0.25f) < 0.001f);
    CHECK (std::is_sorted (square.begin(), square.end(), [] (auto& a, auto& b) { return a.time < b.time; }));
}

TEST_CASE ("The Automation editor edits points, one undo step each", "[ui][automation]")
{
    Fixture f;
    CHECK (f.editor->getClipLength() == 4 * 3840);
    CHECK (f.editor->getPoints().size() == 2);
    const auto steps = f.undoSteps();

    REQUIRE (f.editor->addPoint (3840, 0.25f));
    REQUIRE (f.points().size() == 3);
    CHECK (f.points()[1].time == 3840);
    CHECK (f.editor->getSelection() == std::set<int> { 1 }); // the new point, in time order

    // Moving keeps points inside the clip and 0..1; the selection follows the point.
    REQUIRE (f.editor->movePoints ({ 1 }, 20000, 0.9f));
    CHECK (f.points()[1].time == 4 * 3840);
    CHECK (juce::approximatelyEqual (f.points()[1].value, 1.0f));
    CHECK (f.editor->getSelection() == std::set<int> { 1 });

    REQUIRE (f.editor->setPoint (1, 1920, 0.5f));
    CHECK (f.points()[1].time == 1920);
    REQUIRE (f.editor->setCurve (0, 0.4f));
    CHECK (juce::approximatelyEqual (f.points()[0].curve, 0.4f));

    // A straight line replaces the points it spans.
    REQUIRE (f.editor->drawLine (960, 0.0f, 7680, 1.0f));
    CHECK (f.points().size() == 4); // 0, 960, 7680, the end
    CHECK (std::none_of (f.points().begin(), f.points().end(), [] (auto& p) { return p.time == 1920; }));

    // Deleting keeps one point.
    f.editor->selectAll();
    CHECK (! f.editor->deleteSelection());
    REQUIRE (f.editor->deletePoints ({ 1, 2 }));
    CHECK (f.points().size() == 2);

    CHECK (f.undoSteps() == steps + 6);
    for (int i = 0; i < 6; ++i)
        REQUIRE (f.session->getHistory().undo());
    f.editor->setClip (f.clip);
    CHECK (f.editor->getPoints().size() == 2);
}

TEST_CASE ("Automation editor shapes fill the selection's span, or the clip", "[ui][automation]")
{
    Fixture f;
    f.editor->applyShape (model::AutomationShape::triangle, 3840);
    CHECK (f.points().size() == 9); // 4 cycles, and the end
    CHECK (std::abs (model::automationValueAt (f.points(), 1920.0) - 1.0f) < 0.001f);

    // Two points select the span and the range.
    REQUIRE (f.editor->drawLine (0, 0.2f, 3840, 0.6f));
    std::set<int> firstTwo { 0, 1 };
    f.editor->setSelection (firstTwo);
    REQUIRE (f.editor->applyShape (model::AutomationShape::rampDown, 0));
    CHECK (std::abs (model::automationValueAt (f.points(), 0.0) - 0.6f) < 0.001f);
    CHECK (std::abs (model::automationValueAt (f.points(), 3840.0) - 0.2f) < 0.001f);
}

TEST_CASE ("Automation editor mouse: add, move with fine control, bend, delete, select", "[ui][automation]")
{
    Fixture f;
    Mouse mouse (*f.editor);
    f.editor->setSnap (960);
    const auto steps = f.undoSteps();

    // Click and drag on empty space: a new point, snapped, where the mouse is let go. One step.
    mouse.drag (mouse.at (1000, 0.3f), { mouse.at (1900, 0.5f) });
    REQUIRE (f.points().size() == 3);
    CHECK (f.points()[1].time == 1920);
    CHECK (std::abs (f.points()[1].value - 0.5f) < 0.01f);
    CHECK (f.undoSteps() == steps + 1);

    // Shift during the drag: a tenth of the movement up and down.
    mouse.drag (mouse.at (1920, f.points()[1].value), { mouse.at (1920, 0.9f) }, {}, juce::ModifierKeys::shiftModifier);
    CHECK (std::abs (f.points()[1].value - 0.54f) < 0.01f);

    // The diamond between two points bends that part (dragging up bends a rising part up).
    REQUIRE (f.editor->setPoint (1, 1920, 1.0f));
    const auto middle = (double)f.points()[0].time / 2.0 + 1920.0 / 2.0;
    const auto handle = juce::Point<float> (f.editor->tickToX (middle),
                                            f.editor->valueToY (model::automationValueAt (f.points(), middle)));
    mouse.drag (handle, { handle.translated (0.0f, -40.0f) });
    CHECK (f.points()[0].curve < -0.4f);
    CHECK (model::automationValueAt (f.points(), middle) > (f.points()[0].value + 1.0f) / 2.0f);

    // Right-drag deletes what it touches.
    mouse.drag (mouse.at (1920, 1.0f), {}, juce::ModifierKeys::rightButtonModifier);
    CHECK (f.points().size() == 2);

    // Ctrl+drag selects; the arrows nudge the selection (Shift: finer).
    mouse.drag (mouse.at (-200, 1.0f), { mouse.at (200, 0.0f) }, juce::ModifierKeys::ctrlModifier);
    CHECK (f.editor->getSelection() == std::set<int> { 0 });
    const auto before = f.points()[0].value;
    auto* canvas = mouse.canvas;
    CHECK (canvas->keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
    CHECK (std::abs (f.points()[0].value - (before + 0.01f)) < 0.0005f);
    CHECK (canvas->keyPressed (juce::KeyPress (juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK (std::abs (f.points()[0].value - (before + 0.009f)) < 0.0005f);
    CHECK (canvas->keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (f.points()[0].time == 960);

    // The Line tool draws a ramp; the Paint tool a point per grid step.
    f.editor->setTool (AutomationEditor::Tool::line);
    mouse.drag (mouse.at (3840, 0.0f), { mouse.at (7680, 1.0f) });
    CHECK (std::abs (model::automationValueAt (f.points(), 5760.0) - 0.5f) < 0.02f);
    f.editor->setTool (AutomationEditor::Tool::paint);
    mouse.drag (mouse.at (8640, 0.2f), { mouse.at (9600, 0.4f), mouse.at (10560, 0.6f) });
    CHECK (std::abs (model::automationValueAt (f.points(), 9600.0) - 0.4f) < 0.02f);
    CHECK (std::abs (model::automationValueAt (f.points(), 10560.0) - 0.6f) < 0.02f);

    CHECK (f.editor->createComponentSnapshot (f.editor->getLocalBounds()).isValid());
}

TEST_CASE ("Automation values read and typed in the control's units", "[ui][automation]")
{
    Fixture f;
    auto* parameter = f.session->getAdapter().findParameter (f.target);
    REQUIRE (parameter != nullptr);

    const auto text = f.session->describeTargetValue (f.target, parameter->getCurrentNormalisedValue());
    CHECK (text.contains ("dB"));
    const auto typed = f.session->parseTargetValue (f.target, text);
    REQUIRE (typed.has_value());
    CHECK (std::abs (typed.value_or (-1.0f) - parameter->getCurrentNormalisedValue()) < 0.01f);
    const auto sixDown = f.session->parseTargetValue (f.target, "-6 dB");
    REQUIRE (sixDown.has_value());
    CHECK (f.session->describeTargetValue (f.target, sixDown.value_or (0.0f)).startsWith ("-6.0"));
    CHECK (f.session->parseTargetValue (f.target, "-6") == sixDown);
    CHECK (! f.session->parseTargetValue (f.target, "50%").has_value()); // a volume is in dB, not percent
    CHECK (! f.session->parseTargetValue (f.target, "loud").has_value());

    // Times: bar.beat.tick of the song, from 1.
    CHECK (f.editor->formatTime (0) == "1.1.000");
    CHECK (f.editor->formatTime (3840 + 960 + 120) == "2.2.120");
    CHECK (f.editor->parseTime ("2.2.120") == 3840 + 960 + 120);
    CHECK (f.editor->parseTime ("3") == 7680);
    CHECK (! f.editor->parseTime ("soon").has_value());
}

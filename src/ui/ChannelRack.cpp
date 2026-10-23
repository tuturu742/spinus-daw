#include "ChannelRack.h"
#include "AskForName.h"
#include "ColourMenu.h"
#include "project/AudioImport.h"
#include "engine/Beats.h"

#include <set>

#include "Browser.h"
#include "DeviceMenu.h"
#include "SliderDrag.h"
#include "Theme.h"
#include "commands/CommandRegistry.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "dsp/slicer/BeatSlicerProcessor.h"
#include "engine/BundledDevices.h"
#include "model/ModelJson.h"
#include "session/SlicesToPattern.h"

namespace spinus::ui
{

namespace
{
constexpr int rowHeight = 30, nameWidth = 116, knobWidth = 28, stepWidth = 22;
constexpr int stepsLeft = 8 + 18 + nameWidth + 2 * knobWidth + 16;

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}
} // namespace

//==============================================================================
class ChannelRack::Row final : public juce::Component
{
public:
    Row (ChannelRack& r, const model::Channel& channel) : rack (r), id (channel.id)
    {
        for (auto* knob : { &volume, &pan })
        {
            knob->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            addAndMakeVisible (knob);
        }

        // Heard while dragging; one drag is one undo step.
        volume.setRange (-60.0, 6.0, 0.1);
        volume.setSkewFactorFromMidPoint (-12.0);
        volume.setDoubleClickReturnValue (true, 0.0);
        volume.setHelpText ("Channel volume (double-click for 0 dB)");
        bindDrag (volume, [this] (double v, bool joined) { rack.setChannelVolume (id, v, joined); });

        pan.setRange (-1.0, 1.0, 0.01);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setHelpText ("Channel pan (double-click to centre)");
        bindDrag (pan, [this] (double v, bool joined) { rack.setChannelPan (id, v, joined); });

        update (channel);
    }

    model::ChannelId getId() const { return id; }

    /** The knobs move with their automation while it plays. */
    void followAutomation()
    {
        using Kind = model::ParameterTarget::Kind;
        const auto* channel = rack.session.getProject().findChannel (id);
        if (channel == nullptr)
            return;
        auto& adapter = rack.session.getAdapter();
        ui::followAutomation (
            volume, adapter.findParameter ({ .kind = Kind::channelVolume, .channel = id }), [] (float position)
            { return (double)tracktion::volumeFaderPositionToDB (position); }, channel->volumeDb, volumeAutomated);
        ui::followAutomation (
            pan, adapter.findParameter ({ .kind = Kind::channelPan, .channel = id }),
            [] (float value) { return (double)value; }, channel->pan, panAutomated);
    }

    /** Shows the channel's current state, without rebuilding (a knob may be in the middle of a drag). */
    void update (const model::Channel& channel)
    {
        if (! volume.isMouseButtonDown())
            volume.setValue (channel.volumeDb, juce::dontSendNotification);
        if (! pan.isMouseButtonDown())
            pan.setValue (channel.pan, juce::dontSendNotification);

        name = channel.name.isNotEmpty() ? channel.name : "Channel";
        colour = juce::Colour (channel.colour);
        hasInstrument = channel.instrument.has_value();
        muted = channel.muted;
        frozen = channel.frozen;
        setHelpText (name + (frozen ? " (frozen: right-click its name and Unfreeze to edit the instrument)" : "") +
                     ": click the steps to add or remove notes; click the light to mute; double-click the name to open "
                     "the instrument; right-click it for options");
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.setColour (theme.get ("border"));
        g.fillRect (getLocalBounds().removeFromBottom (1));

        // Mute light.
        const auto light = juce::Rectangle<float> (8.0f, (float)rowHeight / 2.0f - 5.0f, 10.0f, 10.0f);
        g.setColour (muted ? theme.get ("textDim").withAlpha (0.3f) : juce::Colour (0xff6ccf6a));
        g.fillEllipse (light);

        // Name, in the channel's colour; the selected channel (edited in the Piano Roll) is outlined.
        auto nameBox = juce::Rectangle<int> (8 + 18, 4, nameWidth - 6, rowHeight - 8);
        const auto nameFill = ColourMenu::tint (theme.get ("panelHeader"), colour);
        g.setColour (nameFill);
        g.fillRoundedRectangle (nameBox.toFloat(), 3.0f);
        if (rack.session.getSelectedChannel() == id)
        {
            g.setColour (theme.get ("accentAlt"));
            g.drawRoundedRectangle (nameBox.toFloat().reduced (1.0f), 3.0f, 2.0f);
        }
        auto textBox = nameBox.reduced (6, 0);
        if (frozen)
        {
            // Frozen: a snowflake; while rendering, faint with the progress along the bottom.
            const auto status = rack.session.getFreezer().getStatus (id);
            const auto icon = textBox.removeFromRight (14).withSizeKeepingCentre (12, 12).toFloat();
            using State = Freezer::State;
            const bool rendering = status.state == State::rendering;
            const auto iconColour = status.state == State::failed || status.state == State::cannotFreeze
                                        ? theme.get ("danger")
                                        : theme.get ("frozen").withAlpha (rendering ? 0.45f : 1.0f);
            drawFrozenIcon (g, icon, iconColour);
            if (rendering)
            {
                g.setColour (theme.get ("frozen"));
                g.fillRect (nameBox.getX(), nameBox.getBottom() - 2,
                            juce::roundToInt ((float)nameBox.getWidth() * status.progress), 2);
            }
        }

        // An instrument in a separate process that stopped, or one that could not be loaded: its name in red.
        const bool missing = rack.isInstrumentMissing (id);
        g.setColour (rack.isInstrumentStopped (id) || missing ? theme.get ("danger")
                     : muted                                  ? ColourMenu::textOn (nameFill).withAlpha (0.5f)
                                                              : ColourMenu::textOn (nameFill));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (missing ? "Missing: " + name : name, textBox, juce::Justification::centredLeft, true);

        // Notes the steps cannot show: a small picture of them instead (FL), opening the Piano Roll.
        if (rack.hasPianoRollNotes (id))
        {
            paintNotes (g);
            return;
        }

        // Steps, shaded in groups of four (one beat).
        for (int step = 0; step < rack.getNumSteps(); ++step)
        {
            auto box = stepBounds (step).toFloat().reduced (2.0f, 4.0f);
            const bool on = rack.isStepOn (id, step);
            const auto stepsPerBeat = juce::jmax<model::Ticks> (
                1, rack.session.getProject().getTimeSignature (rack.session.getCurrentPattern()).beatTicks() /
                       rack.getStepTicks());
            const bool shaded = (step / stepsPerBeat) % 2 == 1;
            g.setColour (on ? theme.get ("accent") : theme.get ("panelHeader").brighter (shaded ? 0.35f : 0.0f));
            g.fillRoundedRectangle (box, 2.5f);

            if (step == rack.getPlayingStep())
            {
                g.setColour (theme.get ("text").withAlpha (0.8f));
                g.drawRoundedRectangle (box, 2.5f, 1.5f);
            }
        }
    }

    void resized() override
    {
        volume.setBounds (8 + 18 + nameWidth, 1, knobWidth, rowHeight - 2);
        pan.setBounds (8 + 18 + nameWidth + knobWidth, 1, knobWidth, rowHeight - 2);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.x < 8 + 18)
        {
            rack.setChannelMuted (id, ! muted);
            return;
        }

        if (e.x < 8 + 18 + nameWidth)
        {
            if (e.mods.isPopupMenu())
                showMenu();
            else
                rack.session.setSelectedChannel (id); // the Piano Roll edits this channel
            return;
        }

        if (e.x >= stepsLeft && rack.hasPianoRollNotes (id))
        {
            rack.openNotes (id);
            return;
        }

        if (e.x >= stepsLeft)
            if (auto step = (e.x - stepsLeft) / stepWidth; step < rack.getNumSteps())
                rack.toggleStep (id, step);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.x >= 8 + 18 && e.x < 8 + 18 + nameWidth)
            rack.showInstrumentWindow (id);
    }

    void paintNotes (juce::Graphics& g) const
    {
        const auto& theme = themeOf (*this);
        const auto* notes = rack.notesOf (id);
        const auto* pattern = rack.session.getProject().findPattern (rack.session.getCurrentPattern());
        auto area = juce::Rectangle<int> (stepsLeft, 3, rack.getNumSteps() * stepWidth, rowHeight - 6).toFloat();
        g.setColour (theme.get ("panelHeader"));
        g.fillRoundedRectangle (area, 3.0f);
        if (notes == nullptr || notes->empty() || pattern == nullptr || pattern->length <= 0)
            return;
        int low = 127, high = 0;
        for (auto& n : *notes)
        {
            low = juce::jmin (low, n.pitch);
            high = juce::jmax (high, n.pitch);
        }
        area = area.reduced (2.0f, 3.0f);
        const auto rows = (float)juce::jmax (1, high - low + 1);
        const auto noteHeight = juce::jlimit (3.0f, 5.0f, area.getHeight() / rows);
        const auto scale = area.getWidth() / (float)pattern->length;
        g.setColour (theme.get ("accent"));
        for (auto& n : *notes)
        {
            const auto y =
                area.getBottom() - noteHeight -
                (high == low ? area.getHeight() / 2.0f - noteHeight / 2.0f
                             : (float)(n.pitch - low) / (float)(high - low) * (area.getHeight() - noteHeight));
            g.fillRect (juce::Rectangle<float> (area.getX() + (float)n.start * scale, y,
                                                juce::jmax (1.5f, (float)n.length * scale - 1.0f), noteHeight));
        }
    }

    static juce::Rectangle<int> stepBounds (int step)
    {
        return { stepsLeft + step * stepWidth, 0, stepWidth, rowHeight };
    }

private:
    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (name);
        menu.addItem (
            "Rename...",
            [this]
            {
                askForName ("Rename channel", name,
                            [safe = juce::Component::SafePointer<ChannelRack> (&rack), id = id] (const juce::String& n)
                            {
                                if (safe != nullptr)
                                    safe->renameChannel (id, n);
                            });
            });
        ColourMenu::addTo (menu, colour.getARGB(),
                           [safe = juce::Component::SafePointer<ChannelRack> (&rack), id = id] (juce::uint32 c)
                           {
                               if (safe != nullptr)
                                   safe->setChannelColour (id, c);
                           });
        {
            // Groups (FL's filter groups): the rack can show one at a time.
            juce::PopupMenu groups;
            const auto* channel = rack.session.getProject().findChannel (id);
            const auto current = channel != nullptr ? channel->group : juce::String();
            auto setGroup = [safe = juce::Component::SafePointer<ChannelRack> (&rack), id = id] (const juce::String& g)
            {
                if (safe != nullptr)
                    safe->setChannelGroup (id, g);
            };
            groups.addItem ("No group", true, current.isEmpty(), [setGroup] { setGroup ({}); });
            for (const auto& g : rack.getGroups())
                groups.addItem (g, true, g == current, [setGroup, g] { setGroup (g); });
            if (current.isNotEmpty())
                groups.addItem ("Send group \"" + current + "\" through a new mixer bus",
                                [safe = juce::Component::SafePointer<ChannelRack> (&rack), current]
                                {
                                    if (safe != nullptr)
                                        safe->routeGroupToBus (current);
                                });
            groups.addSeparator();
            groups.addItem (
                "New group...", [setGroup]
                { askForName ("New channel group", "Drums", [setGroup] (const juce::String& g) { setGroup (g); }); });
            menu.addSubMenu ("Group", groups);
        }
        menu.addSeparator();
        menu.addItem (frozen ? "Open instrument window (unfreeze to edit)" : "Open instrument window",
                      hasInstrument && ! frozen, false, [this] { rack.showInstrumentWindow (id); });
        menu.addSeparator();
        using Kind = model::ParameterTarget::Kind;
        for (auto [kind, text] : { std::pair { Kind::channelVolume, "Create automation clip: volume" },
                                   std::pair { Kind::channelPan, "Create automation clip: pan" } })
            menu.addItem (text,
                          [this, kind]
                          {
                              model::ParameterTarget target;
                              target.kind = kind;
                              target.channel = id;
                              createAutomationClip (rack, rack.session, target);
                          });
        if (const auto* channel = rack.session.getProject().findChannel (id); channel != nullptr && channel->instrument)
            menu.addSubMenu ("Create automation clip: instrument",
                             makeAutomateParameterMenu (rack, rack.session, channel->instrument->id));
        if (hasSlices (rack.session, id))
        {
            menu.addSeparator();
            menu.addItem ("Slices to pattern", [this] { rack.slicesToPattern (id); });
        }
        if (rack.hasSamplerSample (id))
        {
            menu.addSeparator();
            menu.addItem ("Open in Beat Slicer (map its hits to keys)", [this] { rack.sliceSamplerChannel (id); });
        }
        menu.addSeparator();
        if (frozen)
            menu.addItem ("Unfreeze", [this] { rack.setChannelFrozen (id, false); });
        else
            menu.addItem ("Freeze (play as audio to save CPU)", hasInstrument, false,
                          [this] { rack.setChannelFrozen (id, true); });
        if (frozen)
            if (auto status = rack.session.getFreezer().getStatus (id); status.message.isNotEmpty())
                menu.addItem ("Freeze failed: " + status.message, false, false, nullptr);
        if (const auto* channel = rack.session.getProject().findChannel (id);
            channel != nullptr && channel->instrument && channel->instrument->kind == model::Device::Kind::plugin &&
            rack.session.getSandboxList() != nullptr)
        {
            const auto device = channel->instrument->id;
            const bool separate = rack.session.runsSeparately (device);
            menu.addItem ("Run in a separate process (safe from crashes)", true, separate,
                          [this, device, separate] { rack.session.setRunSeparately (device, ! separate); });
        }
        menu.addItem ("Delete channel", [this] { rack.removeChannel (id); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    ChannelRack& rack;
    model::ChannelId id;
    juce::String name;
    juce::Colour colour;
    bool muted = false, hasInstrument = false, frozen = false;
    bool volumeAutomated = false, panAutomated = false;
    juce::Slider volume, pan;
};

//==============================================================================
ChannelRack::ChannelRack (Session& s) : Panel ("channelRack", "Channel Rack"), session (s)
{
    setHelpText ("Channel Rack (F6): your instruments and the step sequencer for the current pattern");

    addButton.setButtonText ("+ Add channel");
    addButton.setHelpText ("Add a channel with an instrument");
    addButton.onClick = [this] { showAddMenu(); };
    tripletButton.setButtonText ("Triplet steps");
    tripletButton.setHelpText ("This pattern's steps: sixteenth-note triplets (on) or sixteenths (off)");
    tripletButton.setClickingTogglesState (false);
    tripletButton.onClick = [this] { setTripletSteps (! hasTripletSteps()); };

    swingKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    swingKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    swingKnob.setRange (0.0, 100.0, 1.0);
    swingKnob.setDoubleClickReturnValue (true, 0.0);
    swingKnob.setHelpText ("Swing: every second step plays later, for a shuffled groove (double-click: straight)");
    bindDrag (swingKnob, [this] (double percent, bool joined) { setSwing (percent / 100.0, joined); });
    swingLabel.setJustificationType (juce::Justification::centredLeft);
    swingLabel.setInterceptsMouseClicks (false, false);

    groupFilter.setHelpText ("Show all channels, one group, or those in no group (right-click a channel to put it "
                             "in a group)");
    groupFilter.onChange = [this]
    {
        const auto id = groupFilter.getSelectedId();
        const auto groups = getGroups();
        showGroup (id <= 1                  ? juce::String (allGroups)
                   : id - 2 < groups.size() ? groups[id - 2]
                                            : juce::String (noGroup));
    };

    viewport.setViewedComponent (&rowHolder, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);
    rowHolder.addAndMakeVisible (addButton);
    rowHolder.addAndMakeVisible (tripletButton);
    rowHolder.addAndMakeVisible (swingKnob);
    rowHolder.addAndMakeVisible (swingLabel);
    rowHolder.addChildComponent (groupFilter);

    session.addChangeListener (this);
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    rebuild();
    startTimerHz (30);
}

ChannelRack::~ChannelRack()
{
    cancelPendingUpdate();
    session.getHistory().removeListener (historyListener);
    session.removeChangeListener (this);
}

void ChannelRack::rebuild()
{
    // Audio tracks (vocals, stems) have their own panel (AudioTracks).
    updateGroupFilter();
    std::vector<const model::Channel*> channels;
    for (auto& channel : session.getProject().channels)
        if (! channel.audioTrack && (shownGroup == allGroups ||
                                     (shownGroup == noGroup ? channel.group.isEmpty() : channel.group == shownGroup)))
            channels.push_back (&channel);

    // The same channels: each row shows the change in place.
    if (rows.size() == channels.size() && std::equal (rows.begin(), rows.end(), channels.begin(),
                                                      [] (auto& row, auto* c) { return row->getId() == c->id; }))
    {
        for (size_t i = 0; i < rows.size(); ++i)
            rows[i]->update (*channels[i]);
        resized(); // the step count may have changed
        repaint();
        return;
    }

    rows.clear();
    for (auto* channelPointer : channels)
    {
        const auto& channel = *channelPointer;
        rows.push_back (std::make_unique<Row> (*this, channel));
        rowHolder.addAndMakeVisible (*rows.back());
    }
    resized();
    repaint();
}

void ChannelRack::resized()
{
    viewport.setBounds (getLocalBounds());
    const auto width = juce::jmax (viewport.getMaximumVisibleWidth(), stepsLeft + getNumSteps() * stepWidth + 8);
    int y = 0;
    for (auto& row : rows)
    {
        row->setBounds (0, y, width, rowHeight);
        y += rowHeight;
    }
    addButton.setBounds (8, y + 8, 140, 24);
    tripletButton.setBounds (addButton.getRight() + 8, y + 8, 110, 24);
    tripletButton.setToggleState (hasTripletSteps(), juce::dontSendNotification);
    swingKnob.setBounds (tripletButton.getRight() + 12, y + 6, 28, 28);
    swingLabel.setBounds (swingKnob.getRight() + 2, y + 8, 110, 24);
    const auto swing = getSwing();
    if (! swingKnob.isMouseButtonDown())
        swingKnob.setValue (swing * 100.0, juce::dontSendNotification);
    swingLabel.setText ("Swing " + juce::String (juce::roundToInt (swing * 100.0)) + "%", juce::dontSendNotification);
    groupFilter.setBounds (swingLabel.getRight() + 4, y + 8, 150, 24);
    rowHolder.setSize (width, y + 40);
}

void ChannelRack::paint (juce::Graphics& g) { g.fillAll (themeOf (*this).get ("panel")); }

void ChannelRack::paintOverChildren (juce::Graphics& g)
{
    // An instrument dragged from the Browser is over the rack: dropping adds a channel.
    if (dropHighlight)
    {
        g.setColour (themeOf (*this).get ("accent"));
        g.drawRect (getLocalBounds(), 2);
    }
}

//==============================================================================
bool ChannelRack::perform (const juce::String& command, const juce::var& parameters, bool joined)
{
    juce::String error;
    auto c = commands::CommandRegistry::get().create (command, parameters, error);
    if (c == nullptr)
        return false;
    auto& history = session.getHistory();
    return (joined ? history.performJoined (std::move (c)) : history.perform (std::move (c))).wasOk();
}

const std::vector<model::Note>* ChannelRack::notesOf (model::ChannelId channel) const
{
    if (auto* pattern = session.getProject().findPattern (session.getCurrentPattern()))
        if (auto it = pattern->notes.find (channel); it != pattern->notes.end())
            return &it->second;
    return nullptr;
}

int ChannelRack::getNumSteps() const
{
    // One bar of the pattern's time signature (16 in 4/4, 12 in 3/4), however long the pattern is: notes
    // further on show as a picture of the notes instead (hasPianoRollNotes).
    const auto bar = session.getProject().getTimeSignature (session.getCurrentPattern()).barTicks();
    return juce::jlimit (1, 192, (int)(bar / getStepTicks()));
}

model::Ticks ChannelRack::getStepTicks() const
{
    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    return pattern != nullptr ? pattern->getStepTicks() : model::Pattern().getStepTicks();
}

bool ChannelRack::hasTripletSteps() const
{
    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    return pattern != nullptr && pattern->tripletSteps;
}

bool ChannelRack::setTripletSteps (bool triplets)
{
    // No pattern yet: make a one-bar pattern first, as the first step does.
    if (! session.getCurrentPattern().isValid())
        if (! perform ("pattern.add",
                       object ({ { "length", (int)session.getProject().settings.timeSignature.barTicks() } })))
            return false;

    return perform ("pattern.setTripletSteps",
                    object ({ { "pattern", (int)session.getCurrentPattern().value }, { "triplets", triplets } }));
}

double ChannelRack::getSwing() const
{
    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    return pattern != nullptr ? (double)pattern->swing : 0.0;
}

bool ChannelRack::setSwing (double swing, bool joined)
{
    if (! session.getCurrentPattern().isValid())
        if (! perform ("pattern.add",
                       object ({ { "length", (int)session.getProject().settings.timeSignature.barTicks() } })))
            return false;
    return perform (
        "pattern.setSwing",
        object ({ { "pattern", (int)session.getCurrentPattern().value }, { "swing", juce::jlimit (0.0, 1.0, swing) } }),
        joined);
}

bool ChannelRack::openNotes (model::ChannelId channel)
{
    if (session.getProject().findChannel (channel) == nullptr)
        return false;
    session.setSelectedChannel (channel);
    if (onOpenPianoRoll)
        onOpenPianoRoll();
    return true;
}

bool ChannelRack::hasPianoRollNotes (model::ChannelId channel) const
{
    const auto* notes = notesOf (channel);
    if (notes == nullptr)
        return false;
    std::set<model::Ticks> starts;
    const auto stepTicks = getStepTicks();
    const auto end = (model::Ticks)getNumSteps() * stepTicks;
    return std::any_of (notes->begin(), notes->end(),
                        [&] (auto& n)
                        {
                            return n.start % stepTicks != 0 || n.length > stepTicks || n.pitch != stepPitch ||
                                   n.start >= end || ! starts.insert (n.start).second;
                        });
}

bool ChannelRack::isStepOn (model::ChannelId channel, int step) const
{
    if (auto* notes = notesOf (channel))
        return std::any_of (notes->begin(), notes->end(),
                            [start = (model::Ticks)step * getStepTicks()] (auto& n) { return n.start == start; });
    return false;
}

bool ChannelRack::toggleStep (model::ChannelId channel, int step)
{
    if (session.getProject().findChannel (channel) == nullptr || step < 0)
        return false;

    // No pattern yet: make a one-bar pattern first, like FL's "Pattern 1".
    if (! session.getCurrentPattern().isValid())
    {
        const auto bar = session.getProject().settings.timeSignature.barTicks();
        if (! perform ("pattern.add", object ({ { "length", (int)bar } })))
            return false;
    }

    std::vector<model::Note> notes;
    if (auto* existing = notesOf (channel))
        notes = *existing;

    const auto stepTicks = getStepTicks();
    const auto start = (model::Ticks)step * stepTicks;
    const bool wasOn = std::erase_if (notes, [start] (auto& n) { return n.start == start; }) > 0;
    if (! wasOn)
        notes.push_back ({ start, stepTicks, stepPitch, stepVelocity, 0.5f, 0.0f, 0.0f, {} });

    std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });

    juce::Array<juce::var> list;
    for (auto& n : notes)
        list.add (object ({ { "start", (juce::int64)n.start },
                            { "length", (juce::int64)n.length },
                            { "pitch", n.pitch },
                            { "velocity", n.velocity } }));

    return perform ("pattern.setNotes", object ({ { "pattern", (int)session.getCurrentPattern().value },
                                                  { "channel", (int)channel.value },
                                                  { "notes", list },
                                                  { "description", wasOn ? "Remove step" : "Add step" } }));
}

bool ChannelRack::addChannel (const juce::String& stockType, const juce::String& name)
{
    if (stockType.isEmpty())
        return addChannelWith (model::Device {}, name);

    model::Device device;
    device.stockType = stockType;
    return addChannelWith (device, name);
}

bool ChannelRack::addChannelWith (const model::Device& instrument, const juce::String& name)
{
    auto params = object ({ { "name", name } });
    if (instrument.kind == model::Device::Kind::plugin || instrument.stockType.isNotEmpty())
        params.getDynamicObject()->setProperty ("instrument", model::deviceToJson (instrument));

    juce::String error;
    auto command = commands::CommandRegistry::get().create ("channel.add", params, error);
    auto* raw = command.get();
    if (command == nullptr || session.getHistory().perform (std::move (command)).failed())
        return false;

    // A new channel becomes the selected one, ready for the Piano Roll.
    session.setSelectedChannel (model::ChannelId { (uint32_t)(int)raw->getResult()["channel"] });
    return true;
}

bool ChannelRack::setChannelVolume (model::ChannelId channel, double db, bool joined)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "volumeDb", db } }), joined);
}

bool ChannelRack::setChannelPan (model::ChannelId channel, double pan, bool joined)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "pan", pan } }), joined);
}

bool ChannelRack::setChannelMuted (model::ChannelId channel, bool muted)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "muted", muted } }));
}

bool ChannelRack::renameChannel (model::ChannelId channel, const juce::String& newName)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "name", newName } }));
}

bool ChannelRack::setChannelGroup (model::ChannelId channel, const juce::String& group)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "group", group } }));
}

juce::StringArray ChannelRack::getGroups() const
{
    juce::StringArray groups;
    for (const auto& channel : session.getProject().channels)
        if (! channel.audioTrack && channel.group.isNotEmpty())
            groups.addIfNotAlreadyThere (channel.group);
    return groups;
}

bool ChannelRack::routeGroupToBus (const juce::String& group)
{
    juce::Array<juce::var> inserts;
    for (const auto& channel : session.getProject().channels)
        if (channel.group == group && group.isNotEmpty() && channel.targetInsert != model::masterInsertId &&
            ! inserts.contains ((int)channel.targetInsert.value))
            inserts.add ((int)channel.targetInsert.value);
    return ! inserts.isEmpty() && perform ("insert.addBus", object ({ { "name", group }, { "inserts", inserts } }));
}

void ChannelRack::showGroup (const juce::String& group)
{
    shownGroup = group;
    rebuild();
}

void ChannelRack::updateGroupFilter()
{
    // All channels, each group, and those in none; only shown once there is a group.
    const auto groups = getGroups();
    if (shownGroup != allGroups && shownGroup != noGroup && ! groups.contains (shownGroup))
        shownGroup = allGroups; // its last channel left it
    groupFilter.clear (juce::dontSendNotification);
    groupFilter.addItem ("All channels", 1);
    for (int i = 0; i < groups.size(); ++i)
        groupFilter.addItem (groups[i], i + 2);
    groupFilter.addItem ("Not in a group", groups.size() + 2);
    groupFilter.setSelectedId (shownGroup == allGroups ? 1
                               : shownGroup == noGroup ? groups.size() + 2
                                                       : groups.indexOf (shownGroup) + 2,
                               juce::dontSendNotification);
    groupFilter.setVisible (! groups.isEmpty());
}

bool ChannelRack::setChannelColour (model::ChannelId channel, juce::uint32 argb)
{
    return perform ("channel.set",
                    object ({ { "channel", (int)channel.value }, { "colour", ColourMenu::toText (argb) } }));
}

bool ChannelRack::isInstrumentStopped (model::ChannelId channel) const
{
    const auto* c = session.getProject().findChannel (channel);
    return c != nullptr && c->instrument.has_value() &&
           std::find (stoppedDevices.begin(), stoppedDevices.end(), c->instrument->id) != stoppedDevices.end();
}

bool ChannelRack::isInstrumentMissing (model::ChannelId channel) const
{
    const auto* c = session.getProject().findChannel (channel);
    if (c == nullptr || ! c->instrument.has_value())
        return false;
    const auto missing = session.getAdapter().getMissingDevices();
    return std::find (missing.begin(), missing.end(), c->instrument->id) != missing.end();
}

bool ChannelRack::setChannelFrozen (model::ChannelId channel, bool frozen)
{
    return perform ("channel.freeze", object ({ { "channel", (int)channel.value }, { "frozen", frozen } }));
}

bool ChannelRack::removeChannel (model::ChannelId channel)
{
    return perform ("channel.remove", object ({ { "channel", (int)channel.value } }));
}

void ChannelRack::showAddMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Instruments");
    addDeviceItems (menu, session.getDeviceList(), true,
                    [safe = juce::Component::SafePointer<ChannelRack> (this)] (const plugins::DeviceList::Entry& e)
                    {
                        if (safe != nullptr)
                            safe->addChannelWith (e.device, e.name);
                    });
    menu.addSeparator();
    menu.addItem ("Empty channel", [this] { addChannel ({}, "Channel"); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton));
}

bool ChannelRack::addSamplerChannel (const juce::File& file)
{
    // The Sampler reads WAV, FLAC, MP3, Ogg and Opus itself; M4A and others are decoded with ffmpeg first.
    auto source = file;
    if (! file.hasFileExtension (project::ownAudioFormats) &&
        (! project::isImportableAudio (file) ||
         project::makePlayable (file, project::getDecodedAudioFolder(), source).failed()))
        return false;
    auto& formats = dsp::SamplerProcessor::getFormats();
    if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source)); reader == nullptr)
        return false;

    if (! addChannel ("spinus.sampler", file.getFileNameWithoutExtension()))
        return false;

    // The sample goes into the Sampler's own state (saved with the project).
    const auto* channel = session.getProject().findChannel (session.getSelectedChannel());
    if (channel == nullptr || ! channel->instrument)
        return false;
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
        session.getAdapter().getDevicePlugin (channel->instrument.value_or (model::Device {}).id));
    auto* sampler =
        plugin != nullptr ? dynamic_cast<dsp::SamplerProcessor*> (plugin->getAudioPluginInstance()) : nullptr;
    return sampler != nullptr && sampler->loadFiles ({ source }).wasOk();
}

bool ChannelRack::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (project::isImportableAudio (juce::File (f)))
            return true;
    return false;
}

void ChannelRack::fileDragEnter (const juce::StringArray&, int, int)
{
    dropHighlight = true;
    repaint();
}

void ChannelRack::fileDragExit (const juce::StringArray&)
{
    dropHighlight = false;
    repaint();
}

namespace
{
/** The channel's instrument, if it is a T (a Spinus instrument running in Spinus). */
template <typename T> T* instrumentOf (Session& session, model::ChannelId id)
{
    const auto* channel = session.getProject().findChannel (id);
    if (channel == nullptr || ! channel->instrument)
        return nullptr;
    auto* plugin =
        dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (channel->instrument->id));
    return plugin != nullptr ? dynamic_cast<T*> (plugin->getAudioPluginInstance()) : nullptr;
}
} // namespace

bool ChannelRack::addBeatSlicerChannel (const juce::File& file)
{
    auto source = file;
    if (! file.hasFileExtension (project::ownAudioFormats) &&
        (! project::isImportableAudio (file) ||
         project::makePlayable (file, project::getDecodedAudioFolder(), source).failed()))
        return false;
    if (! addChannel ("spinus.beat-slicer", file.getFileNameWithoutExtension()))
        return false;
    const auto channel = session.getSelectedChannel();
    auto* slicer = instrumentOf<dsp::BeatSlicerProcessor> (session, channel);
    if (slicer == nullptr || slicer->loadFile (source).failed())
        return false;
    showInstrumentWindow (channel);
    return true;
}

bool ChannelRack::hasSamplerSample (model::ChannelId channel) const
{
    auto* sampler = instrumentOf<dsp::SamplerProcessor> (session, channel);
    return sampler != nullptr && ! sampler->getZones().empty() && sampler->getZones().front().audio != nullptr;
}

bool ChannelRack::sliceSamplerChannel (model::ChannelId channel)
{
    auto* sampler = instrumentOf<dsp::SamplerProcessor> (session, channel);
    if (sampler == nullptr || sampler->getZones().empty() || sampler->getZones().front().audio == nullptr)
        return false;
    const auto zone = sampler->getZones().front(); // its first sample, from its start to its end

    const auto start = (int)juce::jlimit<juce::int64> (0, zone.getLength() - 1, zone.start);
    const auto end =
        (int)juce::jlimit<juce::int64> (start + 1, zone.getLength(), zone.end > 0 ? zone.end : zone.getLength());
    juce::AudioBuffer<float> audio (zone.audio->getNumChannels(), end - start);
    for (int c = 0; c < audio.getNumChannels(); ++c)
        audio.copyFrom (c, 0, *zone.audio, c, start, end - start);

    const auto* original = session.getProject().findChannel (channel);
    if (! addChannel ("spinus.beat-slicer", (original != nullptr ? original->name : zone.name) + " (sliced)"))
        return false;
    const auto slicerChannel = session.getSelectedChannel();
    auto* slicer = instrumentOf<dsp::BeatSlicerProcessor> (session, slicerChannel);
    if (slicer == nullptr)
        return false;
    slicer->setLoop (std::move (audio), zone.sampleRate, zone.name);
    showInstrumentWindow (slicerChannel);
    return true;
}

bool ChannelRack::isLongAudio (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::SamplerProcessor::getFormats().createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return true; // M4A and others: songs and stems more often than samples
    return (double)reader->lengthInSamples / reader->sampleRate > longAudioSeconds;
}

void ChannelRack::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragExit (files);
    // Short samples become Sampler channels for patterns; long audio (vocals, stems) audio tracks,
    // unless Shift is held while dropping.
    const bool keepInRack = juce::ModifierKeys::currentModifiers.isShiftDown();
    juce::StringArray longFiles;
    for (auto& f : files)
        if (onLongAudio && ! keepInRack && project::isImportableAudio (juce::File (f)) && isLongAudio (juce::File (f)))
            longFiles.add (f);
        else
            addSamplerChannel (juce::File (f));
    if (! longFiles.isEmpty())
        onLongAudio (longFiles);
}

bool ChannelRack::isInterestedInDragSource (const SourceDetails& details)
{
    if (Browser::sampleFromDrag (details.description))
        return true;
    plugins::DeviceList::Entry entry;
    return plugins::DeviceList::fromVar (details.description, entry) && entry.instrument;
}

void ChannelRack::itemDragEnter (const SourceDetails&)
{
    dropHighlight = true;
    repaint();
}

void ChannelRack::itemDragExit (const SourceDetails&)
{
    dropHighlight = false;
    repaint();
}

void ChannelRack::itemDropped (const SourceDetails& details)
{
    // A sample from the Browser, as if dropped from the file manager.
    if (const auto sample = Browser::sampleFromDrag (details.description))
    {
        filesDropped ({ sample->getFullPathName() }, 0, 0);
        return;
    }

    itemDragExit (details);
    plugins::DeviceList::Entry entry;
    if (plugins::DeviceList::fromVar (details.description, entry) && entry.instrument)
        addChannelWith (entry.device, entry.name);
}

bool ChannelRack::showInstrumentWindow (model::ChannelId channel)
{
    auto* c = session.getProject().findChannel (channel);
    return c != nullptr && c->instrument && session.showDeviceWindow (c->instrument->id);
}

bool ChannelRack::slicesToPattern (model::ChannelId channel)
{
    return spinus::slicesToPattern (session, channel).wasOk();
}

void ChannelRack::timerCallback()
{
    int step = -1;
    if (session.isPlaying() && session.getMode() == Session::Mode::pattern)
    {
        auto& edit = session.getEdit();
        const auto ticks = engine::timeToTicks (edit, edit.getTransport().getPosition());
        step = (int)std::floor (ticks / (double)getStepTicks()) % juce::jmax (1, getNumSteps());
    }

    for (auto& row : rows)
        row->followAutomation();

    // Freeze progress is shown on the frozen channels' names, and stopped instruments in red.
    const bool rendering = session.getFreezer().isRendering();
    auto stopped = session.getAdapter().getStoppedDevices();
    auto missing = session.getAdapter().getMissingDevices();
    const bool stoppedChanged = stopped != stoppedDevices || missing != missingDevices;
    stoppedDevices = std::move (stopped);
    missingDevices = std::move (missing);
    if (step != playingStep || rendering || wasRendering || stoppedChanged)
    {
        playingStep = step;
        for (auto& row : rows)
            row->repaint();
    }
    wasRendering = rendering;
}

} // namespace spinus::ui

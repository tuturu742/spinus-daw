#include "Mixer.h"
#include "ColourMenu.h"
#include "SliderDrag.h"

#include "DeviceMenu.h"
#include "Theme.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "model/ModelJson.h"
#include "model/Validation.h"

namespace spinus::ui
{

namespace te = tracktion;

namespace
{
constexpr int headerHeight = 38, slotHeight = 22, scrollBarHeight = 12, shownSlots = 10;

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

juce::var idValue (auto id) { return (int)id.value; }

std::vector<const model::Insert*> allInserts (const model::Project& p)
{
    std::vector<const model::Insert*> result { &p.mixer.master };
    for (auto& i : p.mixer.inserts)
        result.push_back (&i);
    return result;
}

/** What a strip or menu calls an insert: "Master", or "3 Bass" (its position and name). */
juce::String displayName (const model::Project& p, model::InsertId id)
{
    if (id == model::masterInsertId)
        return "Master";
    for (size_t i = 0; i < p.mixer.inserts.size(); ++i)
        if (p.mixer.inserts[i].id == id)
            return juce::String ((int)i + 1) + " " + p.mixer.inserts[i].name;
    return "?";
}

/** Compressors, limiters, gates and the Ducker: effects whose job is to turn the sound down, so the Mixer
    shows how far they do. */
bool isDynamics (const model::Device& d)
{
    if (d.kind == model::Device::Kind::stock)
        return d.stockType.startsWith ("bundled.lsp.compressor") || d.stockType == "bundled.lsp.limiter" ||
               d.stockType == "bundled.lsp.gate" || d.stockType == "bundled.lsp.multiband" ||
               d.stockType == "spinus.ducker" || d.stockType == "spinus.compressor" ||
               d.stockType == "spinus.limiter" || d.stockType == "spinus.gate";
    const auto name = d.plugin.name.toLowerCase();
    for (auto word : { "compress", "limit", "gate", "duck", "dynamic", "ott", "clipper", "leveler", "expander" })
        if (name.contains (word))
            return true;
    return false;
}

juce::String effectName (const model::Device& d, bool missing)
{
    const auto name = model::getDeviceDisplayName (d);
    return missing ? "Missing: " + name : name;
}

void configureKnob (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
}

//==============================================================================
/** A stereo peak meter fed by a Tracktion LevelMeasurer, which may come and go with the audio device. */
class LevelMeter final : public juce::Component
{
public:
    ~LevelMeter() override { setSource (nullptr); }

    void setSource (te::LevelMeasurer* measurer)
    {
        if (source.get() == measurer)
            return;
        if (auto* old = source.get())
            old->removeClient (client);
        source = measurer;
        client.reset();
        if (measurer != nullptr)
            measurer->addClient (client);
    }

    void tick()
    {
        bool changed = false;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto db = source != nullptr ? client.getAndClearAudioLevel (ch).dB : -100.0f;
            const auto next = juce::jmax (db, levels[ch] - 1.5f); // fall about 45 dB per second
            changed = changed || std::abs (next - levels[ch]) > 0.05f;
            levels[ch] = next;
        }
        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        auto area = getLocalBounds().toFloat();
        g.setColour (theme.get ("background"));
        g.fillRect (area);

        const auto barWidth = (area.getWidth() - 1.0f) / 2.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            // -60 dB at the bottom, +6 dB at the top; red above 0 dB.
            const auto proportion = juce::jlimit (0.0f, 1.0f, (levels[ch] + 60.0f) / 66.0f);
            auto bar = juce::Rectangle<float> (area.getX() + (float)ch * (barWidth + 1.0f), area.getY(), barWidth,
                                               area.getHeight());
            auto lit = bar.removeFromBottom (bar.getHeight() * proportion);
            g.setColour (levels[ch] > 0.0f ? theme.get ("danger") : juce::Colour (0xff6ccf6a));
            g.fillRect (lit);
        }
    }

private:
    juce::WeakReference<te::LevelMeasurer> source;
    te::LevelMeasurer::Client client;
    float levels[2] = { -100.0f, -100.0f };
};
} // namespace

//==============================================================================
class Mixer::Strip final : public juce::Component
{
public:
    Strip (Mixer& m, model::InsertId insertId) : mixer (m), id (insertId)
    {
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (-60.0, 6.0, 0.1);
        fader.setSkewFactorFromMidPoint (-12.0);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setHelpText ("Insert volume (double-click for 0 dB)");
        bindDrag (fader, [this] (double v, bool joined) { mixer.setInsertVolume (id, v, joined); });

        configureKnob (pan);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setHelpText ("Insert pan (double-click to centre)");
        bindDrag (pan, [this] (double v, bool joined) { mixer.setInsertPan (id, v, joined); });

        mute.setButtonText ("M");
        mute.setHelpText ("Mute this insert");
        mute.onClick = [this] { mixer.setInsertMuted (id, ! muted); };
        solo.setButtonText ("S");
        solo.setHelpText ("Solo this insert");
        solo.onClick = [this] { mixer.setInsertSolo (id, ! soloed); };

        for (juce::Component* c : { (juce::Component*)&meter, (juce::Component*)&fader, (juce::Component*)&pan,
                                    (juce::Component*)&mute, (juce::Component*)&solo })
        {
            c->addMouseListener (this, false); // a click anywhere selects the strip
            addAndMakeVisible (c);
        }
    }

    model::InsertId getId() const noexcept { return id; }
    LevelMeter& getMeter() noexcept { return meter; }

    /** The insert's effects at work: how far its compressors turn the sound down, and its CPU share. */
    void updateActivity()
    {
        const auto* insert = mixer.session.getProject().findInsert (id);
        if (insert == nullptr)
            return;
        float reduction = 0.0f, load = 0.0f;
        bool dynamics = false;
        for (const auto& slot : insert->effects)
            if (auto activity = mixer.session.getAdapter().getDeviceActivity (slot.device.id))
            {
                load += activity->load;
                if (isDynamics (slot.device))
                {
                    dynamics = true;
                    reduction = juce::jmin (reduction, activity->gainReductionDb);
                }
            }
        const auto percent = juce::roundToInt (load * 100.0f);
        if (dynamics != hasDynamics || std::abs (reduction - gainReduction) > 0.25f || percent != cpuPercent)
        {
            const bool relayout = dynamics != hasDynamics;
            hasDynamics = dynamics;
            gainReduction = reduction;
            cpuPercent = percent;
            if (relayout)
                resized();
            repaint();
        }
    }

    float getGainReduction() const noexcept { return gainReduction; }
    bool showsGainReduction() const noexcept { return hasDynamics; }

    /** The fader and pan knob move with their automation while it plays. */
    void followAutomation()
    {
        using Kind = model::ParameterTarget::Kind;
        const auto* insert = mixer.session.getProject().findInsert (id);
        if (insert == nullptr)
            return;
        auto& adapter = mixer.session.getAdapter();
        ui::followAutomation (
            fader, adapter.findParameter ({ .kind = Kind::insertVolume, .insert = id }), [] (float position)
            { return (double)te::volumeFaderPositionToDB (position); }, insert->volumeDb, faderAutomated);
        ui::followAutomation (
            pan, adapter.findParameter ({ .kind = Kind::insertPan, .insert = id }),
            [] (float value) { return (double)value; }, insert->pan, panAutomated);
    }

    void update (const model::Insert& insert, const juce::String& number)
    {
        name = insert.name;
        label = number;
        colour = juce::Colour (insert.colour);
        muted = insert.muted;
        soloed = insert.solo;
        frozen = insert.frozen;

        if (! fader.isMouseButtonDown())
            fader.setValue (insert.volumeDb, juce::dontSendNotification);
        if (! pan.isMouseButtonDown())
            pan.setValue (insert.pan, juce::dontSendNotification);

        // Where its sound comes from and goes, besides the usual: sidechains in, an output other than the master.
        const auto& project = mixer.session.getProject();
        auto shortName = [&project] (model::InsertId other)
        {
            const auto* o = project.findInsert (other);
            return o == nullptr || o->name.isEmpty() ? displayName (project, other) : o->name;
        };
        juce::StringArray from, said;
        for (auto source : insert.sidechainSources)
            from.add (shortName (source));
        routing.clear();
        if (! from.isEmpty())
        {
            routing = juce::String::fromUTF8 ("\u2190 ") + from.joinIntoString (", ");
            said.add ("sidechain from " + from.joinIntoString (", "));
        }
        if (id != model::masterInsertId && insert.output.isValid() && insert.output != model::masterInsertId)
        {
            const auto to = shortName (insert.output);
            routing = routing.isEmpty() ? juce::String::fromUTF8 ("\u2192 ") + to : routing;
            said.add ("goes to " + to + " instead of the Master");
        }
        juce::StringArray sentTo;
        for (const auto& send : insert.sends)
            sentTo.add (shortName (send.target));
        if (! sentTo.isEmpty())
        {
            routing = routing.isEmpty() ? "+ " + sentTo.joinIntoString (", ") : routing;
            said.add ("sends to " + sentTo.joinIntoString (", "));
        }
        routingHelp = said.joinIntoString ("; ");

        mute.setToggleState (muted, juce::dontSendNotification);
        solo.setToggleState (soloed, juce::dontSendNotification);
        solo.setVisible (id != model::masterInsertId);
        setHelpText ((id == model::masterInsertId ? juce::String ("Master") : label + " " + name) +
                     (frozen ? " (frozen: right-click and Unfreeze to edit its effects)" : "") +
                     (routingHelp.isEmpty() ? juce::String() : " (" + routingHelp + ")") +
                     ": click to select; drag the name to reorder; double-click it to rename; right-click for more");
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const bool isSelected = mixer.getSelectedInsert() == id;

        // The strip faintly in the insert's colour, its number and name on the colour itself.
        const auto body = isSelected ? theme.get ("panelHeader").brighter (0.25f) : theme.get ("panelHeader");
        g.setColour (ColourMenu::tint (body, colour, 0.14f));
        g.fillRect (getLocalBounds().reduced (1, 0));
        const auto headerFill = ColourMenu::tint (body, colour);
        g.setColour (headerFill);
        g.fillRect (getLocalBounds().reduced (1, 0).withHeight (headerHeight));
        const auto onHeader = ColourMenu::textOn (headerFill);

        if (isSelected)
        {
            g.setColour (theme.get ("accent"));
            g.drawRect (getLocalBounds().reduced (1, 0), 1);
        }

        g.setColour (onHeader.withAlpha (0.7f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (label, 4, 4, getWidth() - 8, 10, juce::Justification::centredLeft);

        if (frozen)
        {
            // Frozen: a snowflake by the number; while rendering, faint with the progress under the name.
            using State = Freezer::State;
            const auto status = mixer.session.getFreezer().getStatus (id);
            const bool rendering = status.state == State::rendering;
            const auto icon = juce::Rectangle<float> ((float)getWidth() - 15.0f, 3.0f, 11.0f, 11.0f);
            drawFrozenIcon (g, icon,
                            status.state == State::failed || status.state == State::cannotFreeze
                                ? theme.get ("danger")
                                : theme.get ("frozen").withAlpha (rendering ? 0.45f : 1.0f));
            if (rendering)
            {
                g.setColour (theme.get ("frozen"));
                g.fillRect (4, headerHeight - 1, juce::roundToInt ((float)(getWidth() - 8) * status.progress), 2);
            }
        }
        else if (cpuPercent >= 1)
        {
            g.setColour (cpuPercent >= 25 ? theme.get ("danger") : onHeader.withAlpha (0.7f));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (juce::String (cpuPercent) + "%", 4, 4, getWidth() - 8, 10, juce::Justification::centredRight);
        }

        if (hasDynamics)
        {
            // Gain reduction: a bar growing down from the top, 0 to 24 dB.
            const auto area = reductionArea.toFloat();
            g.setColour (theme.get ("background"));
            g.fillRect (area);
            const auto depth = juce::jlimit (0.0f, 1.0f, -gainReduction / 24.0f);
            g.setColour (juce::Colour (0xffe0894a));
            g.fillRect (area.withHeight (area.getHeight() * depth));
        }

        g.setColour (onHeader.withAlpha (muted ? 0.5f : 1.0f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (id == model::masterInsertId ? juce::String ("Master") : name, 4, 13, getWidth() - 8, 12,
                    juce::Justification::centredLeft, true);
        if (routing.isNotEmpty())
        {
            g.setColour (onHeader.withAlpha (0.85f));
            g.setFont (juce::FontOptions (10.5f));
            g.drawText (routing, 4, 25, getWidth() - 8, 11, juce::Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 0);
        area.removeFromTop (headerHeight + 2);

        auto buttons = area.removeFromBottom (22);
        if (solo.isVisible())
        {
            mute.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 0));
            solo.setBounds (buttons.reduced (1, 0));
        }
        else
        {
            mute.setBounds (buttons.reduced (1, 0));
        }

        area.removeFromBottom (2);
        pan.setBounds (area.removeFromBottom (34).withSizeKeepingCentre (34, 34));
        area.removeFromBottom (4);
        meter.setBounds (area.removeFromLeft (10).reduced (0, 6));
        reductionArea =
            hasDynamics ? area.removeFromLeft (6).withTrimmedLeft (2).reduced (0, 6) : juce::Rectangle<int>();
        fader.setBounds (area);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        mixer.selectInsert (id);

        if (e.eventComponent == this && e.mods.isPopupMenu())
            showMenu();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Dragging the name reorders (the master stays first).
        if (e.eventComponent != this || e.mouseDownPosition.y > headerHeight || id == model::masterInsertId)
            return;
        setTopLeftPosition (getX() + e.getDistanceFromDragStartX() - dragOffset, getY());
        dragOffset = e.getDistanceFromDragStartX();
        toFront (false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != this || dragOffset == 0)
            return;

        const auto shift = (int)std::round ((float)dragOffset / (float)stripWidth);
        dragOffset = 0;
        const auto& inserts = mixer.session.getProject().mixer.inserts;
        for (size_t i = 0; i < inserts.size(); ++i)
            if (inserts[i].id == id)
            {
                const auto target = juce::jlimit (0, (int)inserts.size() - 1, (int)i + shift);
                if (target == (int)i || ! mixer.moveInsert (id, target))
                    mixer.resized(); // snap back
                return;
            }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && e.y <= headerHeight && id != model::masterInsertId)
            startRename();
    }

private:
    void startRename()
    {
        editor = std::make_unique<juce::TextEditor>();
        editor->setText (name, false);
        editor->selectAll();
        editor->setBounds (2, 12, getWidth() - 4, 15);
        editor->onReturnKey = [this] { finishRename (true); };
        editor->onEscapeKey = [this] { finishRename (false); };
        editor->onFocusLost = [this] { finishRename (true); };
        addAndMakeVisible (*editor);
        editor->grabKeyboardFocus();
    }

    void finishRename (bool apply)
    {
        if (editor == nullptr)
            return;
        const auto text = editor->getText().trim();
        // Remove the editor after this callback returns: it is the one calling us.
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer<Strip> (this)]
            {
                if (safe != nullptr)
                    safe->editor.reset();
            });
        editor->setVisible (false);
        if (apply && text.isNotEmpty() && text != name)
            mixer.renameInsert (id, text);
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (id == model::masterInsertId ? juce::String ("Master") : label + " " + name);
        if (id != model::masterInsertId)
        {
            menu.addItem ("Rename", [this] { startRename(); });
            ColourMenu::addTo (menu, colour.getARGB(),
                               [safe = juce::Component::SafePointer<Mixer> (&mixer), id = id] (juce::uint32 c)
                               {
                                   if (safe != nullptr)
                                       safe->setInsertColour (id, c);
                               });
            menu.addItem ("Remove insert", [this] { mixer.removeInsert (id); });
            menu.addSeparator();

            // Freeze: why not, when it cannot be (something inside it also plays elsewhere).
            const auto status = mixer.session.getFreezer().getStatus (id);
            if (frozen)
                menu.addItem ("Unfreeze", [this] { mixer.setInsertFrozen (id, false); });
            else
            {
                const auto why = model::whyCannotFreeze (mixer.session.getProject(), id);
                menu.addItem ("Freeze (play as audio to save CPU)", why.isEmpty(), false,
                              [this] { mixer.setInsertFrozen (id, true); });
                if (why.isNotEmpty())
                    menu.addItem (why, false, false, nullptr);
            }
            if (status.message.isNotEmpty())
                menu.addItem ((status.state == Freezer::State::failed ? "Freeze failed: " : "Playing live: ") +
                                  status.message,
                              false, false, nullptr);
            menu.addSeparator();
        }
        using Kind = model::ParameterTarget::Kind;
        for (auto [kind, text] : { std::pair { Kind::insertVolume, "Create automation clip: volume" },
                                   std::pair { Kind::insertPan, "Create automation clip: pan" } })
            menu.addItem (text,
                          [this, kind]
                          {
                              model::ParameterTarget target;
                              target.kind = kind;
                              target.insert = id;
                              createAutomationClip (mixer, mixer.session, target);
                          });
        // Sidechain: whose sound this insert's ducker or sidechain compressor follows.
        const auto& project = mixer.session.getProject();
        if (const auto* insert = project.findInsert (id))
        {
            const auto current = insert->sidechainSources.empty() ? model::InsertId() : insert->sidechainSources[0];
            juce::PopupMenu sources;
            sources.addItem ("None", true, ! current.isValid(), [this] { mixer.setSidechain (id, {}); });
            for (auto* other : allInserts (project))
                if (other->id != id && other->id != model::masterInsertId)
                    sources.addItem (displayName (project, other->id),
                                     ! model::wouldCreateRoutingCycle (project, other->id, id) || other->id == current,
                                     other->id == current,
                                     [this, source = other->id] { mixer.setSidechain (id, source); });
            menu.addSeparator();
            menu.addSubMenu ("Sidechain from", sources);

            // Notes for its effects (Spinus Tune's target notes).
            if (id != model::masterInsertId)
            {
                juce::PopupMenu notes;
                notes.addItem ("None", true, ! insert->notesFrom, [this] { mixer.setNotesFrom (id, {}); });
                for (auto& channel : project.channels)
                    notes.addItem (channel.name, true, channel.id == insert->notesFrom,
                                   [this, channel = channel.id] { mixer.setNotesFrom (id, channel); });
                menu.addSubMenu ("Notes from channel", notes);
            }
        }

        menu.addSeparator();
        menu.addItem ("Remove unused inserts", ! mixer.findUnusedInserts().empty(), false,
                      [this] { mixer.removeUnusedInserts(); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    Mixer& mixer;
    model::InsertId id;
    juce::String name, label, routing, routingHelp;
    juce::Rectangle<int> reductionArea;
    float gainReduction = 0.0f;
    bool hasDynamics = false;
    int cpuPercent = 0;
    juce::Colour colour;
    bool muted = false, soloed = false, frozen = false;
    int dragOffset = 0;

    bool faderAutomated = false, panAutomated = false;
    LevelMeter meter;
    juce::Slider fader, pan;
    juce::TextButton mute, solo;
    std::unique_ptr<juce::TextEditor> editor;
};

//==============================================================================
class Mixer::AddStrip final : public juce::Component
{
public:
    explicit AddStrip (Mixer& m) : mixer (m) { setHelpText ("Add a mixer insert"); }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.setColour (theme.get ("border"));
        g.drawRect (getLocalBounds().reduced (3, 2), 1);
        g.setColour (isMouseOver() ? theme.get ("text") : theme.get ("textDim"));
        g.setFont (juce::FontOptions (28.0f));
        g.drawText ("+", getLocalBounds(), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()))
            mixer.addInsert();
    }

private:
    Mixer& mixer;
};

//==============================================================================
/** The selected insert's effect slots, output and sends. */
class Mixer::Details final : public juce::Component
{
public:
    explicit Details (Mixer& m) : mixer (m)
    {
        output.setHelpText ("Where this insert's sound goes");
        output.onChange = [this]
        {
            const auto target = model::InsertId { (uint32_t)output.getSelectedId() };
            if (target.isValid() && mixer.setOutput (mixer.getSelectedInsert(), target))
                return;
            update(); // refused (e.g. a feedback loop): show the real output again
        };
        addAndMakeVisible (output);

        addSend.setButtonText ("+ Send");
        addSend.setHelpText ("Send some of this insert's sound to another insert as well (e.g. a reverb)");
        addSend.onClick = [this] { showAddSendMenu(); };
        addAndMakeVisible (addSend);

        status.setJustificationType (juce::Justification::topLeft);
        status.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (status);
        setHelpText ("Effect slots: click an empty slot to add an effect; double-click an effect to open its "
                     "window; click the light to bypass; drag to reorder; right-click for more");
    }

    void update()
    {
        const auto& project = mixer.session.getProject();
        const auto* insert = project.findInsert (mixer.getSelectedInsert());
        if (insert == nullptr)
            return;

        title = displayName (project, insert->id);
        effects = insert->effects;
        activity.clear();
        missing = mixer.session.getAdapter().getMissingDevices();
        stopped = mixer.session.getAdapter().getStoppedDevices();

        output.clear (juce::dontSendNotification);
        output.setVisible (insert->id != model::masterInsertId);
        for (auto* other : allInserts (project))
            if (other->id != insert->id)
                output.addItem ("Out: " + displayName (project, other->id), (int)other->id.value);
        output.setSelectedId ((int)insert->output.value, juce::dontSendNotification);

        // Keep the rows of sends that still exist (one may be mid-drag); rebuild the rest.
        std::vector<std::unique_ptr<SendRow>> kept;
        for (auto& send : insert->sends)
        {
            auto it = std::find_if (sendRows.begin(), sendRows.end(),
                                    [&] (auto& r) { return r != nullptr && r->target == send.target; });
            if (it != sendRows.end())
                kept.push_back (std::move (*it));
            else
                kept.push_back (std::make_unique<SendRow> (*this, send.target));
            kept.back()->update (displayName (project, send.target), send.levelDb);
            addAndMakeVisible (*kept.back());
        }
        sendRows = std::move (kept);

        status.setText (mixer.getLastError(), juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, themeOf (*this).get ("danger"));
        resized();
        repaint();
    }

    /** Each effect at work: how far a compressor turns the sound down, and its CPU share. */
    void updateActivity()
    {
        std::vector<juce::String> now;
        now.reserve (effects.size());
        for (const auto& slot : effects)
        {
            juce::String text;
            if (auto reading = mixer.session.getAdapter().getDeviceActivity (slot.device.id))
            {
                if (isDynamics (slot.device) && reading->gainReductionDb <= -0.5f)
                    text << juce::String (reading->gainReductionDb, 1) << " dB";
                if (const auto percent = juce::roundToInt (reading->load * 100.0f); percent >= 1)
                    text << (text.isEmpty() ? "" : "  ") << percent << "%";
            }
            now.push_back (text);
        }
        if (now != activity)
        {
            activity = std::move (now);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.setColour (theme.get ("panelHeader"));
        g.fillRect (getLocalBounds());
        g.setColour (theme.get ("border"));
        g.fillRect (0, 0, 1, getHeight());

        g.setColour (theme.get ("text"));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (title, 10, 4, getWidth() - 20, 18, juce::Justification::centredLeft, true);

        g.setFont (juce::FontOptions (12.0f));
        for (int slot = 0; slot < getNumSlots(); ++slot)
        {
            auto box = slotBounds (slot).toFloat().reduced (0.0f, 1.0f);
            const bool filled = slot < (int)effects.size();
            g.setColour (slot == dragTarget ? theme.get ("accentAlt") : theme.get ("panel"));
            g.fillRoundedRectangle (box, 3.0f);

            g.setColour (theme.get ("textDim"));
            g.drawText (juce::String (slot + 1), box.removeFromLeft (20.0f), juce::Justification::centred);

            if (! filled)
            {
                g.drawText ("(empty)", box, juce::Justification::centredLeft);
                continue;
            }

            auto& device = effects[(size_t)slot].device;
            const bool isMissing = std::find (missing.begin(), missing.end(), device.id) != missing.end();
            const bool isStopped = std::find (stopped.begin(), stopped.end(), device.id) != stopped.end();
            g.setColour (isStopped         ? theme.get ("danger")
                         : device.bypassed ? theme.get ("textDim").withAlpha (0.4f)
                                           : juce::Colour (0xff6ccf6a));
            g.fillEllipse (box.removeFromLeft (16.0f).withSizeKeepingCentre (9.0f, 9.0f));
            if (slot < (int)activity.size() && activity[(size_t)slot].isNotEmpty())
            {
                g.setColour (theme.get ("textDim"));
                g.setFont (juce::FontOptions (10.5f));
                g.drawText (activity[(size_t)slot], box.removeFromRight (78.0f).reduced (4.0f, 0.0f),
                            juce::Justification::centredRight);
                g.setFont (juce::FontOptions (12.0f));
            }
            g.setColour (isMissing || isStopped ? theme.get ("danger")
                                                : (device.bypassed ? theme.get ("textDim") : theme.get ("text")));
            g.drawText (isStopped ? "Stopped: " + effectName (device, false) : effectName (device, isMissing),
                        box.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
        }

        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (11.0f));
        if (! sendRows.empty() || addSend.isVisible())
            g.drawText ("Sends", 10, sendsTop() - 16, 80, 14, juce::Justification::centredLeft);
    }

    void resized() override
    {
        int y = slotBounds (getNumSlots() - 1).getBottom() + 8;
        output.setBounds (10, y, getWidth() - 20, 22);
        if (output.isVisible())
            y += 30;

        y += 16; // "Sends" label
        for (auto& row : sendRows)
        {
            row->setBounds (10, y, getWidth() - 20, 28);
            y += 30;
        }
        addSend.setBounds (10, y + 2, 80, 22);
        status.setBounds (8, y + 28, getWidth() - 16, juce::jmax (0, getHeight() - y - 30));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragTarget = -1;
        const auto slot = slotAt (e.getPosition());
        if (slot < 0)
            return;

        if (slot >= (int)effects.size())
        {
            if (! e.mods.isPopupMenu())
                showAddMenu (slot);
            return;
        }

        auto& device = effects[(size_t)slot].device;
        if (e.mods.isPopupMenu())
            showSlotMenu (slot);
        else if (e.x < slotBounds (slot).getX() + 36)
            mixer.setEffectBypassed (device.id, ! device.bypassed);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto slot = slotAt (e.getPosition());
        if (slot >= 0 && slot < (int)effects.size() && e.x >= slotBounds (slot).getX() + 36)
            mixer.showEffectWindow (effects[(size_t)slot].device.id);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto from = slotAt (e.mouseDownPosition.toInt());
        if (from < 0 || from >= (int)effects.size() || e.getDistanceFromDragStart() < 4)
            return;
        const auto to = juce::jmin (slotAt (e.getPosition()), (int)effects.size() - 1);
        if (to != dragTarget)
        {
            dragTarget = to;
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto from = slotAt (e.mouseDownPosition.toInt());
        const auto to = dragTarget;
        dragTarget = -1;
        repaint();
        if (from >= 0 && from < (int)effects.size() && to >= 0 && to != from)
            mixer.moveEffect (effects[(size_t)from].device.id, mixer.getSelectedInsert(), to);
    }

    static juce::Rectangle<int> slotBounds (int slot)
    {
        return { 10, 28 + slot * slotHeight, detailsWidth - 20, slotHeight };
    }

    /** Highlights the slot an effect from the Browser would be dropped into (-1: none). */
    void showDropSlot (int slot)
    {
        if (slot != dragTarget)
        {
            dragTarget = slot;
            repaint();
        }
    }

    /** Slots shown: at least 10 (FL's familiar column), always one empty after the last effect. */
    int getNumSlots() const { return juce::jmax (shownSlots, (int)effects.size() + 1); }

    /** The height everything needs (the panel scrolls when it is taller than the Mixer). */
    int getContentHeight() const { return sendsTop() + (int)sendRows.size() * 30 + 30 + 60; }

    int slotAt (juce::Point<int> p) const
    {
        for (int slot = 0; slot < getNumSlots(); ++slot)
            if (slotBounds (slot).contains (p))
                return slot;
        return -1;
    }

private:
    class SendRow final : public juce::Component
    {
    public:
        SendRow (Details& d, model::InsertId to) : details (d), target (to)
        {
            configureKnob (level);
            level.setRange (-60.0, 6.0, 0.1);
            level.setSkewFactorFromMidPoint (-12.0);
            level.setDoubleClickReturnValue (true, 0.0);
            level.setHelpText ("Send level (double-click for 0 dB)");
            bindDrag (level, [this] (double v, bool joined)
                      { details.mixer.setSend (details.mixer.getSelectedInsert(), target, v, joined); });
            addAndMakeVisible (level);

            remove.setButtonText ("x");
            remove.setHelpText ("Remove this send");
            remove.onClick = [this] { details.mixer.removeSend (details.mixer.getSelectedInsert(), target); };
            addAndMakeVisible (remove);
        }

        void update (const juce::String& name, float db)
        {
            text = name;
            if (! level.isMouseButtonDown())
                level.setValue (db, juce::dontSendNotification);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (themeOf (*this).get ("text"));
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (text, getHeight() + 6, 0, getWidth() - getHeight() - 34, getHeight(),
                        juce::Justification::centredLeft, true);
        }

        void resized() override
        {
            level.setBounds (0, 0, getHeight(), getHeight());
            remove.setBounds (getWidth() - 22, 3, 22, getHeight() - 6);
        }

        Details& details;
        model::InsertId target;

    private:
        juce::String text;
        juce::Slider level;
        juce::TextButton remove;
    };

    int sendsTop() const { return slotBounds (getNumSlots() - 1).getBottom() + 8 + (output.isVisible() ? 30 : 0) + 16; }

    void showAddMenu (int slot)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Add effect");
        addDeviceItems (
            menu, mixer.session.getDeviceList(), false,
            [safe = juce::Component::SafePointer<Details> (this), slot] (const plugins::DeviceList::Entry& e)
            {
                if (safe != nullptr)
                    safe->mixer.addEffect (safe->mixer.getSelectedInsert(), e.device, slot);
            });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (slotBounds (slot))));
    }

    void showSlotMenu (int slot)
    {
        auto device = effects[(size_t)slot].device;
        const auto& project = mixer.session.getProject();

        juce::PopupMenu moveTo, copyTo;
        for (auto* other : allInserts (project))
        {
            if (other->id != mixer.getSelectedInsert())
                moveTo.addItem (displayName (project, other->id), true, false,
                                [this, id = device.id, to = other->id, at = (int)other->effects.size()]
                                { mixer.moveEffect (id, to, at); });
            copyTo.addItem (displayName (project, other->id), true, false,
                            [this, id = device.id, to = other->id] { mixer.copyEffect (id, to); });
        }

        juce::PopupMenu menu;
        menu.addSectionHeader (effectName (device, false));
        const bool deviceFrozen = mixer.session.isDeviceFrozen (device.id);
        menu.addItem (deviceFrozen ? "Open window (unfreeze to edit)" : "Open window", ! deviceFrozen, false,
                      [this, id = device.id] { mixer.showEffectWindow (id); });
        menu.addItem (device.bypassed ? "Enable" : "Bypass",
                      [this, id = device.id, b = device.bypassed] { mixer.setEffectBypassed (id, ! b); });
        menu.addItem ("Move up", slot > 0, false,
                      [this, id = device.id, slot] { mixer.moveEffect (id, mixer.getSelectedInsert(), slot - 1); });
        menu.addItem ("Move down", slot + 1 < (int)effects.size(), false,
                      [this, id = device.id, slot] { mixer.moveEffect (id, mixer.getSelectedInsert(), slot + 1); });
        menu.addSubMenu ("Move to insert", moveTo);
        menu.addSubMenu ("Copy to insert (with its settings)", copyTo);
        menu.addSeparator();
        menu.addSubMenu ("Create automation clip", makeAutomateParameterMenu (mixer, mixer.session, device.id));
        if (device.kind == model::Device::Kind::plugin && mixer.session.getSandboxList() != nullptr)
        {
            const bool separate = mixer.session.runsSeparately (device.id);
            menu.addItem ("Run in a separate process (safe from crashes)", true, separate,
                          [this, id = device.id, separate] { mixer.session.setRunSeparately (id, ! separate); });
        }
        menu.addSeparator();
        menu.addItem ("Remove", [this, id = device.id] { mixer.removeEffect (id); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (slotBounds (slot))));
    }

    void showAddSendMenu()
    {
        const auto& project = mixer.session.getProject();
        const auto* insert = project.findInsert (mixer.getSelectedInsert());
        if (insert == nullptr)
            return;

        juce::PopupMenu menu;
        menu.addSectionHeader ("Send to");
        for (auto* other : allInserts (project))
        {
            const bool exists = std::any_of (insert->sends.begin(), insert->sends.end(),
                                             [&] (auto& s) { return s.target == other->id; });
            if (other->id != insert->id && ! exists)
                menu.addItem (displayName (project, other->id),
                              [this, to = other->id] { mixer.setSend (mixer.getSelectedInsert(), to, -6.0); });
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addSend));
    }

    Mixer& mixer;
    juce::String title;
    std::vector<model::EffectSlot> effects;
    std::vector<juce::String> activity; // per effect: "-6.0 dB  3%", or empty
    std::vector<model::DeviceId> missing, stopped;
    int dragTarget = -1;

    juce::ComboBox output;
    std::vector<std::unique_ptr<SendRow>> sendRows;
    juce::TextButton addSend;
    juce::Label status;
};

//==============================================================================
Mixer::Mixer (Session& s) : Panel ("mixer", "Mixer"), session (s)
{
    setHelpText ("Mixer (F9): inserts with their effects, volume and routing");

    addStrip = std::make_unique<AddStrip> (*this);
    details = std::make_unique<Details> (*this);
    stripArea.addAndMakeVisible (*addStrip);
    addAndMakeVisible (stripArea);
    detailsView.setViewedComponent (details.get(), false);
    detailsView.setScrollBarsShown (true, false);
    detailsView.setScrollBarThickness (8);
    addAndMakeVisible (detailsView);

    scrollBar.setAutoHide (true); // shown only when the strips do not fit
    scrollBar.addListener (this);
    addAndMakeVisible (scrollBar);

    session.addChangeListener (this);
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    handleAsyncUpdate();
    startTimerHz (30);
}

Mixer::~Mixer()
{
    cancelPendingUpdate();
    session.getHistory().removeListener (historyListener);
    session.removeChangeListener (this);
}

//==============================================================================
void Mixer::handleAsyncUpdate()
{
    if (session.getProject().findInsert (selected) == nullptr)
        selected = model::masterInsertId;

    layoutStrips();
    details->update();
    layoutDetails();
    repaint();
}

int Mixer::getNumShownSlots() const { return details->getNumSlots(); }

void Mixer::layoutDetails()
{
    details->setSize (detailsWidth, juce::jmax (detailsView.getHeight(), details->getContentHeight()));
}

int Mixer::getNumStrips() const { return (int)session.getProject().mixer.inserts.size() + 2; }

juce::String Mixer::getStripHelpText (model::InsertId id) const
{
    const auto strip = strips.find (id);
    return strip == strips.end() ? juce::String() : strip->second->getHelpText();
}

int Mixer::stripX (int index) const { return index == 0 ? 0 : masterWidth + (index - 1) * stripWidth; }

void Mixer::resized()
{
    auto area = getLocalBounds();
    detailsView.setBounds (area.removeFromRight (detailsWidth));
    layoutDetails();
    scrollBar.setBounds (area.removeFromBottom (scrollBarHeight));
    stripArea.setBounds (area);
    layoutStrips();
}

void Mixer::layoutStrips()
{
    const auto& project = session.getProject();
    const auto total = stripX (getNumStrips());
    const auto visibleWidth = juce::jmax (1, stripArea.getWidth());
    scrollBar.setRangeLimits (0.0, (double)juce::jmax (total, visibleWidth), juce::dontSendNotification);
    scrollBar.setCurrentRange (scrollBar.getCurrentRangeStart(), (double)visibleWidth, juce::dontSendNotification);
    const auto offset = (int)scrollBar.getCurrentRangeStart();

    // Only strips that are at least partly visible exist.
    std::map<model::InsertId, std::unique_ptr<Strip>> visible;
    auto inserts = allInserts (project);
    for (int index = 0; index < (int)inserts.size(); ++index)
    {
        const auto x = stripX (index) - offset;
        const auto width = index == 0 ? masterWidth : stripWidth;
        if (x + width <= 0 || x >= visibleWidth)
            continue;

        auto& insert = *inserts[(size_t)index];
        auto existing = strips.find (insert.id);
        auto strip =
            existing != strips.end() ? std::move (existing->second) : std::make_unique<Strip> (*this, insert.id);
        strip->update (insert, index == 0 ? juce::String ("M") : juce::String (index));
        strip->setBounds (x, 0, width, stripArea.getHeight());
        stripArea.addAndMakeVisible (*strip);
        visible[insert.id] = std::move (strip);
    }
    strips = std::move (visible); // the rest are deleted

    addStrip->setBounds (stripX (getNumStrips() - 1) - offset, 0, stripWidth, stripArea.getHeight());
}

void Mixer::scrollToStrip (int index) { scrollBar.setCurrentRangeStart ((double)stripX (index)); }

void Mixer::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! stripArea.getBounds().contains (e.getEventRelativeTo (this).getPosition()))
        return;
    const auto delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    scrollBar.moveScrollbarInSteps ((int)std::round (-delta * 4.0f));
}

void Mixer::paint (juce::Graphics& g) { g.fillAll (themeOf (*this).get ("panel")); }

void Mixer::timerCallback()
{
    const bool rendering = session.getFreezer().isRendering();
    for (auto& [id, strip] : strips)
    {
        strip->getMeter().setSource (session.getAdapter().getInsertLevels (id));
        strip->getMeter().tick();
        strip->followAutomation();
        strip->updateActivity();
        if (rendering || wasRendering)
            strip->repaint(); // Freeze progress
    }
    wasRendering = rendering;
    details->updateActivity();

    // A plugin in a separate process that stopped (or was reloaded) shows in its slot.
    if (auto stopped = session.getAdapter().getStoppedDevices(); stopped != lastStopped)
    {
        lastStopped = std::move (stopped);
        details->update();
        details->repaint();
    }
}

void Mixer::selectInsert (model::InsertId id)
{
    if (id == selected || session.getProject().findInsert (id) == nullptr)
        return;
    selected = id;
    lastError.clear();
    for (auto& [_, strip] : strips)
        strip->repaint();
    details->update();
}

//==============================================================================
bool Mixer::perform (const juce::String& command, const juce::var& parameters, bool joined)
{
    juce::String error;
    auto c = commands::CommandRegistry::get().create (command, parameters, error);
    if (c == nullptr)
    {
        lastError = std::move (error);
        return false;
    }

    auto& history = session.getHistory();
    const auto result = joined ? history.performJoined (std::move (c)) : history.perform (std::move (c));
    lastError = result.failed() ? result.getErrorMessage() : juce::String();
    if (result.failed())
        details->update(); // show why
    return result.wasOk();
}

bool Mixer::addInsert()
{
    juce::String error;
    auto c = commands::CommandRegistry::get().create ("insert.add", object ({}), error);
    auto* command = c.get();
    if (c == nullptr || session.getHistory().perform (std::move (c)).failed())
        return false;

    // Select the new insert and scroll to it.
    selected = model::InsertId { (uint32_t)(int)command->getResult()["insert"] };
    handleAsyncUpdate();
    scrollToStrip (getNumStrips() - 1);
    return true;
}

bool Mixer::removeInsert (model::InsertId id)
{
    return perform ("insert.remove", object ({ { "insert", idValue (id) } }));
}

std::vector<model::InsertId> Mixer::findUnusedInserts() const
{
    const auto& p = session.getProject();
    std::vector<model::InsertId> result;
    for (auto& insert : p.mixer.inserts)
    {
        if (! insert.effects.empty())
            continue;
        const bool fedByChannel =
            std::any_of (p.channels.begin(), p.channels.end(), [&] (auto& c) { return c.targetInsert == insert.id; });
        const auto all = allInserts (p);
        const bool fedByInsert = std::any_of (
            all.begin(), all.end(),
            [&] (auto* other)
            {
                return other->id != insert.id &&
                       (other->output == insert.id || std::any_of (other->sends.begin(), other->sends.end(),
                                                                   [&] (auto& s) { return s.target == insert.id; }));
            });
        if (! fedByChannel && ! fedByInsert)
            result.push_back (insert.id);
    }
    return result;
}

bool Mixer::removeUnusedInserts()
{
    const auto unused = findUnusedInserts();
    if (unused.empty())
        return false;

    auto transaction = std::make_unique<commands::Transaction> ("Remove unused inserts");
    for (auto id : unused)
    {
        juce::String error;
        auto c =
            commands::CommandRegistry::get().create ("insert.remove", object ({ { "insert", idValue (id) } }), error);
        if (c == nullptr)
            return false;
        transaction->add (std::move (c));
    }
    return session.getHistory().perform (std::move (transaction)).wasOk();
}

bool Mixer::moveInsert (model::InsertId id, int index)
{
    return perform ("insert.move", object ({ { "insert", idValue (id) }, { "index", index } }));
}

bool Mixer::renameInsert (model::InsertId id, const juce::String& name)
{
    return perform ("insert.set", object ({ { "insert", idValue (id) }, { "name", name } }));
}

bool Mixer::setInsertColour (model::InsertId id, juce::uint32 argb)
{
    return perform ("insert.set", object ({ { "insert", idValue (id) }, { "colour", ColourMenu::toText (argb) } }));
}

bool Mixer::setInsertVolume (model::InsertId id, double db, bool joined)
{
    return perform ("insert.set", object ({ { "insert", idValue (id) }, { "volumeDb", db } }), joined);
}

bool Mixer::setInsertPan (model::InsertId id, double pan, bool joined)
{
    return perform ("insert.set", object ({ { "insert", idValue (id) }, { "pan", pan } }), joined);
}

bool Mixer::setInsertMuted (model::InsertId id, bool muted)
{
    return perform ("insert.set", object ({ { "insert", idValue (id) }, { "muted", muted } }));
}

bool Mixer::setNotesFrom (model::InsertId id, model::ChannelId channel)
{
    auto params = object ({ { "insert", idValue (id) } });
    if (channel)
        params.getDynamicObject()->setProperty ("channel", (int)channel.value);
    return perform ("insert.setNotesFrom", params);
}

bool Mixer::setInsertFrozen (model::InsertId id, bool frozen)
{
    return perform ("insert.freeze", object ({ { "insert", idValue (id) }, { "frozen", frozen } }));
}

bool Mixer::setInsertSolo (model::InsertId id, bool solo)
{
    return perform ("insert.set", object ({ { "insert", idValue (id) }, { "solo", solo } }));
}

bool Mixer::addEffect (model::InsertId id, const juce::String& stockType, std::optional<int> slot)
{
    model::Device device;
    device.stockType = stockType;
    return addEffect (id, device, slot);
}

bool Mixer::addEffect (model::InsertId id, const model::Device& device, std::optional<int> slot)
{
    auto params = object ({ { "insert", idValue (id) }, { "device", model::deviceToJson (device) } });
    if (slot)
    {
        const auto* insert = session.getProject().findInsert (id);
        const auto count = insert != nullptr ? (int)insert->effects.size() : 0;
        params.getDynamicObject()->setProperty ("index", juce::jmin (*slot, count));
    }
    return perform ("insert.addEffect", params);
}

//==============================================================================
bool Mixer::isInterestedInDragSource (const SourceDetails& source)
{
    plugins::DeviceList::Entry entry;
    return plugins::DeviceList::fromVar (source.description, entry) && ! entry.instrument;
}

void Mixer::itemDragMove (const SourceDetails& source)
{
    if (detailsView.getBounds().contains (source.localPosition))
    {
        const auto slot = details->slotAt (details->getLocalPoint (this, source.localPosition));
        const auto* insert = session.getProject().findInsert (selected);
        const auto count = insert != nullptr ? (int)insert->effects.size() : 0;
        details->showDropSlot (slot < 0 ? -1 : juce::jmin (slot, count));
        return;
    }

    details->showDropSlot (-1);
    for (auto& [id, strip] : strips)
        if (getLocalArea (strip.get(), strip->getLocalBounds()).contains (source.localPosition) && id != selected)
            selectInsert (id);
}

void Mixer::itemDragExit (const SourceDetails&) { details->showDropSlot (-1); }

void Mixer::itemDropped (const SourceDetails& source)
{
    plugins::DeviceList::Entry entry;
    if (! plugins::DeviceList::fromVar (source.description, entry) || entry.instrument)
        return;

    std::optional<int> slot;
    if (detailsView.getBounds().contains (source.localPosition))
        if (auto s = details->slotAt (details->getLocalPoint (this, source.localPosition)); s >= 0)
            slot = s;

    details->showDropSlot (-1);
    addEffect (selected, entry.device, slot);
}

bool Mixer::removeEffect (model::DeviceId id)
{
    return perform ("insert.removeEffect", object ({ { "device", idValue (id) } }));
}

bool Mixer::moveEffect (model::DeviceId id, model::InsertId to, int slot)
{
    return perform ("insert.moveEffect",
                    object ({ { "device", idValue (id) }, { "insert", idValue (to) }, { "index", slot } }));
}

bool Mixer::copyEffect (model::DeviceId id, model::InsertId to)
{
    const auto* device = session.getProject().findDevice (id);
    if (device == nullptr)
        return false;
    // The same effect, with its settings as the original has them now (also those turned in its window).
    model::Device copy = *device;
    for (auto& [key, value] : session.getAdapter().readParameters (id))
        copy.parameters[key] = value;
    return perform ("insert.addEffect",
                    object ({ { "insert", idValue (to) }, { "device", model::deviceToJson (copy) } }));
}

bool Mixer::setEffectBypassed (model::DeviceId id, bool bypassed)
{
    return perform ("insert.setEffect", object ({ { "device", idValue (id) }, { "bypassed", bypassed } }));
}

bool Mixer::showEffectWindow (model::DeviceId id)
{
    if (session.showDeviceWindow (id))
        return true;
    if (session.isDeviceFrozen (id))
    {
        lastError = "This effect is frozen: unfreeze its insert (right-click the insert) to edit it.";
        details->update();
        return false;
    }
    lastError = "This effect's window cannot be opened (its plugin is missing or has no window).";
    details->update();
    return false;
}

bool Mixer::setOutput (model::InsertId id, model::InsertId target)
{
    return perform ("insert.setOutput", object ({ { "insert", idValue (id) }, { "target", idValue (target) } }));
}

bool Mixer::setSidechain (model::InsertId id, model::InsertId source)
{
    auto parameters = object ({ { "insert", idValue (id) } });
    if (source.isValid())
        parameters.getDynamicObject()->setProperty ("source", idValue (source));
    return perform ("insert.setSidechain", parameters);
}

bool Mixer::setSend (model::InsertId id, model::InsertId target, double levelDb, bool joined)
{
    return perform ("insert.setSend",
                    object ({ { "insert", idValue (id) }, { "target", idValue (target) }, { "levelDb", levelDb } }),
                    joined);
}

bool Mixer::removeSend (model::InsertId id, model::InsertId target)
{
    return perform ("insert.removeSend", object ({ { "insert", idValue (id) }, { "target", idValue (target) } }));
}

} // namespace spinus::ui

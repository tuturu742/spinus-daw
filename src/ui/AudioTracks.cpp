#include "AudioTracks.h"

#include "Browser.h"
#include "DeviceMenu.h"
#include "SliderDrag.h"
#include "Theme.h"
#include "commands/CommandRegistry.h"
#include "session/RecordingTime.h"
#include "project/AudioImport.h"

namespace spinus::ui
{

namespace
{
constexpr int nameWidth = 150, knobWidth = 30, insertWidth = 110, armWidth = 26, inputWidth = 120, meterWidth = 6;
constexpr int recordWidth = armWidth + 4 + inputWidth + 4 + meterWidth + 6; // a track's recording controls
constexpr int stereoInputIds = 1000;                                        // "Input 3 + 4" in the input list

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

juce::String formatSeconds (double seconds)
{
    const auto total = juce::roundToInt (seconds);
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}
} // namespace

//==============================================================================
class AudioTracks::Row final : public juce::Component
{
public:
    Row (AudioTracks& t, const model::Channel& channel) : tracks (t), id (channel.id)
    {
        for (auto* knob : { &volume, &pan })
        {
            knob->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            addAndMakeVisible (knob);
        }

        volume.setRange (-60.0, 6.0, 0.1);
        volume.setSkewFactorFromMidPoint (-12.0);
        volume.setDoubleClickReturnValue (true, 0.0);
        volume.setHelpText ("Track volume (double-click for 0 dB)");
        bindDrag (volume, [this] (double v, bool joined) { tracks.setVolume (id, v, joined); });

        pan.setRange (-1.0, 1.0, 0.01);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setHelpText ("Track pan (double-click to centre)");
        bindDrag (pan, [this] (double v, bool joined) { tracks.setPan (id, v, joined); });

        // Recording: armed tracks record their own inputs together (multi-track recording).
        arm.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f")));
        arm.setClickingTogglesState (false);
        arm.setHelpText ("Arm for recording: Record audio records every armed track at once, each from its own "
                         "input");
        arm.onClick = [this] { tracks.setArmed (id, ! arm.getToggleState()); };
        addAndMakeVisible (arm);
        input.setHelpText ("The sound card input this track records from (a pair for stereo)");
        input.onChange = [this]
        {
            const auto chosen = input.getSelectedId();
            if (chosen == 1)
                tracks.setRecordInput (id, 0, false);
            else if (chosen > stereoInputIds)
                tracks.setRecordInput (id, chosen - stereoInputIds, true);
            else if (chosen > 1)
                tracks.setRecordInput (id, chosen - 1, false);
        };
        addAndMakeVisible (input);

        update (channel);
    }

    model::ChannelId getId() const { return id; }

    void update (const model::Channel& channel)
    {
        if (! volume.isMouseButtonDown())
            volume.setValue (channel.volumeDb, juce::dontSendNotification);
        if (! pan.isMouseButtonDown())
            pan.setValue (channel.pan, juce::dontSendNotification);
        name = channel.name.isNotEmpty() ? channel.name : "Audio";
        muted = channel.muted;
        colour = juce::Colour (channel.colour);
        const auto* insert = tracks.session.getProject().findInsert (channel.targetInsert);
        insertName = insert != nullptr ? insert->name : juce::String ("Master");
        summary = tracks.describeClips (id);
        armed = channel.recordArmed;
        arm.setToggleState (armed, juce::dontSendNotification);
        arm.setColour (juce::TextButton::textColourOnId, juce::Colour (0xffe05252));
        arm.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5a2a2a));
        fillInputs (channel);
        setHelpText (name + ": click the light to mute; double-click the name to open its audio in Spinus Tune; "
                            "right-click it for options");
        repaint();
    }

    void followAutomation()
    {
        using Kind = model::ParameterTarget::Kind;
        const auto* channel = tracks.session.getProject().findChannel (id);
        if (channel == nullptr)
            return;
        auto& adapter = tracks.session.getAdapter();
        ui::followAutomation (
            volume, adapter.findParameter ({ .kind = Kind::channelVolume, .channel = id }), [] (float position)
            { return (double)tracktion::volumeFaderPositionToDB (position); }, channel->volumeDb, volumeAutomated);
        ui::followAutomation (
            pan, adapter.findParameter ({ .kind = Kind::channelPan, .channel = id }),
            [] (float value) { return (double)value; }, channel->pan, panAutomated);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.setColour (theme.get ("border"));
        g.fillRect (getLocalBounds().removeFromBottom (1));

        const auto light = juce::Rectangle<float> (8.0f, (float)rowHeight / 2.0f - 5.0f, 10.0f, 10.0f);
        g.setColour (muted ? theme.get ("textDim").withAlpha (0.3f) : juce::Colour (0xff6ccf6a));
        g.fillEllipse (light);

        // The name, on a strip of the track's colour (a wave sign: this is audio, not an instrument).
        auto nameBox = juce::Rectangle<int> (26, 4, nameWidth - 6, rowHeight - 8);
        g.setColour (theme.get ("panelHeader"));
        g.fillRoundedRectangle (nameBox.toFloat(), 3.0f);
        g.setColour (colour);
        g.fillRect (nameBox.removeFromLeft (4));
        auto text = nameBox.reduced (6, 0);
        drawWave (g, text.removeFromLeft (14).toFloat(), theme.get ("textDim"));
        g.setColour (muted ? theme.get ("textDim") : theme.get ("text"));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (name, text.withTrimmedLeft (4), juce::Justification::centredLeft, true);

        // The input's level while armed (or recording).
        if (armed)
        {
            const auto level = tracks.getTrackLevel (id);
            const auto bar = meterArea.toFloat();
            g.setColour (theme.get ("background"));
            g.fillRect (bar);
            const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
            const auto fill = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            g.setColour (db > -1.0f ? juce::Colour (0xffe05252) : juce::Colour (0xff6ccf6a));
            g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * fill));
        }

        const auto x = 26 + nameWidth + 2 * knobWidth + 10 + recordWidth;
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (theme.get ("textDim"));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 ")) + insertName,
                    juce::Rectangle<int> (x, 0, insertWidth, rowHeight), juce::Justification::centredLeft, true);
        g.drawText (summary, juce::Rectangle<int> (x + insertWidth, 0, getWidth() - x - insertWidth - 8, rowHeight),
                    juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        volume.setBounds (26 + nameWidth, 1, knobWidth, rowHeight - 2);
        pan.setBounds (26 + nameWidth + knobWidth, 1, knobWidth, rowHeight - 2);
        auto x = 26 + nameWidth + 2 * knobWidth + 6;
        arm.setBounds (x, 5, armWidth, rowHeight - 10);
        x += armWidth + 4;
        input.setBounds (x, 5, inputWidth, rowHeight - 10);
        x += inputWidth + 4;
        meterArea = { x, 5, meterWidth, rowHeight - 10 };
    }

    /** Repaints the meter (the panel's timer). */
    void updateMeter()
    {
        if (armed)
            repaint (meterArea);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.x < 26)
        {
            tracks.setMuted (id, ! muted);
            return;
        }
        if (e.x < 26 + nameWidth && e.mods.isPopupMenu())
            showMenu();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.x >= 26 && e.x < 26 + nameWidth)
            openFirstClip();
    }

private:
    static void drawWave (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        juce::Path wave;
        const auto mid = r.getCentreY();
        wave.startNewSubPath (r.getX(), mid);
        for (int i = 1; i <= 12; ++i)
        {
            const auto x = r.getX() + r.getWidth() * (float)i / 12.0f;
            wave.lineTo (x, mid + std::sin ((float)i * 1.7f) * r.getHeight() * 0.3f * (i % 2 == 0 ? 1.0f : 0.5f));
        }
        g.setColour (c);
        g.strokePath (wave, juce::PathStrokeType (1.2f));
    }

    std::optional<model::ClipId> firstClip() const
    {
        std::optional<std::pair<model::Ticks, model::ClipId>> first;
        for (auto& clip : tracks.session.getProject().playlist.clips)
            if (auto* audio = std::get_if<model::AudioClip> (&clip.content); audio != nullptr && audio->channel == id)
                if (! first || clip.start < first->first)
                    first = std::pair { clip.start, clip.id };
        return first ? std::optional (first->second) : std::nullopt;
    }

    void openFirstClip()
    {
        if (auto clip = firstClip(); clip && tracks.onOpenAudioClip)
        {
            tracks.session.setSelectedChannel (id);
            tracks.onOpenAudioClip (*clip);
        }
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (name);
        menu.addItem ("Rename...", [this] { askForName(); });
        menu.addItem ("Edit in Spinus Tune", firstClip().has_value(), false, [this] { openFirstClip(); });
        const auto file = tracks.getTrackFile (id);
        menu.addItem ("Open in Beat Slicer (map its hits to keys)", file.has_value() && tracks.onSlice != nullptr,
                      false,
                      [this, file]
                      {
                          if (file && tracks.onSlice)
                              tracks.onSlice (*file);
                      });
        menu.addItem (file ? juce::String ("Move to Channel Rack (as a Sampler)")
                           : juce::String ("Move to Channel Rack (its clips play different files)"),
                      file.has_value() && tracks.onMoveToRack != nullptr, false,
                      [this, file]
                      {
                          if (file && tracks.onMoveToRack)
                              tracks.onMoveToRack (id, *file);
                      });
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
                              createAutomationClip (tracks, tracks.session, target);
                          });
        menu.addSeparator();
        menu.addItem ("Delete track (and its clips)", [this] { tracks.removeTrack (id); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void askForName()
    {
        auto window = std::make_shared<juce::AlertWindow> ("Rename track", juce::String(),
                                                           juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor ("name", name);
        window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (
            true, juce::ModalCallbackFunction::create (
                      [safe = juce::Component::SafePointer<AudioTracks> (&tracks), id = id, window] (int result)
                      {
                          const auto text = window->getTextEditorContents ("name").trim();
                          if (safe != nullptr && result == 1 && text.isNotEmpty())
                              safe->rename (id, text);
                      }));
        if (auto* editor = window->getTextEditor ("name"))
        {
            editor->grabKeyboardFocus();
            editor->selectAll();
        }
    }

    /** "Default input", each input, each pair: the sound card's names. */
    void fillInputs (const model::Channel& channel)
    {
        const auto names = recording::inputChannelNames (tracks.session.getEdit().engine);
        auto label = [&names] (int i)
        {
            // Bare numbers ("1", as some sound cards name them) read as "Input 1".
            const auto n = names[i].trim();
            return n.isEmpty() || n.containsOnly ("0123456789") ? "Input " + juce::String (i + 1) : n;
        };
        input.clear (juce::dontSendNotification);
        input.addItem ("Default input", 1); // the one in Options › Audio
        for (int i = 0; i < names.size(); ++i)
            input.addItem (label (i), i + 2);
        if (names.size() > 1)
        {
            input.addSectionHeading ("Stereo");
            for (int i = 0; i + 1 < names.size(); i += 2)
                input.addItem (label (i).startsWith ("Input ") && label (i + 1).startsWith ("Input ")
                                   ? "Inputs " + juce::String (i + 1) + " + " + juce::String (i + 2)
                                   : label (i) + " + " + label (i + 1),
                               stereoInputIds + i + 1);
        }
        const auto selected = channel.recordInput <= 0 ? 1
                              : channel.recordStereo   ? stereoInputIds + channel.recordInput
                                                       : channel.recordInput + 1;
        if (input.indexOfItemId (selected) < 0) // not on this sound card
            input.addItem (
                "Input " + juce::String (channel.recordInput) +
                    (channel.recordStereo ? " + " + juce::String (channel.recordInput + 1) : juce::String()) +
                    " (not here)",
                selected);
        input.setSelectedId (selected, juce::dontSendNotification);
    }

    AudioTracks& tracks;
    model::ChannelId id;
    juce::String name, insertName, summary;
    juce::Colour colour;
    bool muted = false, volumeAutomated = false, panAutomated = false, armed = false;
    juce::Slider volume, pan;
    juce::TextButton arm;
    juce::ComboBox input;
    juce::Rectangle<int> meterArea;
};

//==============================================================================
AudioTracks::AudioTracks (Session& s) : Panel ("audioTracks", "Audio tracks"), session (s)
{
    setHelpText ("Audio tracks: vocals, stems and other long audio on the Playlist, apart from the Channel Rack's "
                 "instruments. Drop audio files here to add tracks");

    addButton.setButtonText ("+ Add audio file");
    addButton.setHelpText ("Add an audio file as a new track, at the start of the song on the Playlist");
    addButton.onClick = [this] { chooseFiles(); };

    addTrackButton.setButtonText ("+ Track to record");
    addTrackButton.setHelpText ("Add an empty audio track to record into: choose its input and arm it");
    addTrackButton.onClick = [this] { addRecordingTrack(); };

    recordButton.setHelpText ("Record while the song plays from the current position: every armed track from its "
                              "own input, all at once; with none armed, the input chosen in Options › Audio into a "
                              "new track (or the one chosen beside)");
    recordButton.onClick = [this] { toggleRecording(); };
    recordButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f Record audio")));
    recordInto.setHelpText ("Where a recording goes: a new track, or into a track; over audio already on it, it "
                            "becomes another take (choose the best parts with Alt+click on the Playlist)");
    recordInto.onChange = [this]
    {
        const auto id = recordInto.getSelectedId();
        recordTarget = id > 1 ? std::optional (model::ChannelId { (uint32_t)(id - 1) }) : std::nullopt;
    };
    recordStatus.setJustificationType (juce::Justification::centredLeft);
    recordStatus.setInterceptsMouseClicks (false, false);

    emptyMessage.setText ("No audio tracks yet. Drop vocals, stems or other long audio here or on the Playlist.",
                          juce::dontSendNotification);
    emptyMessage.setJustificationType (juce::Justification::centredLeft);
    emptyMessage.setInterceptsMouseClicks (false, false);

    viewport.setViewedComponent (&rowHolder, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    rowHolder.addAndMakeVisible (addButton);
    rowHolder.addAndMakeVisible (addTrackButton);
    rowHolder.addAndMakeVisible (recordButton);
    rowHolder.addAndMakeVisible (recordInto);
    rowHolder.addAndMakeVisible (recordStatus);
    rowHolder.addChildComponent (emptyMessage);

    session.addChangeListener (this);
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    rebuild();
    startTimerHz (30);
}

AudioTracks::~AudioTracks()
{
    cancelPendingUpdate();
    session.getAudioRecorder().setMetering (false);
    session.getHistory().removeListener (historyListener);
    session.removeChangeListener (this);
}

std::vector<model::ChannelId> AudioTracks::getTracks() const
{
    std::vector<model::ChannelId> result;
    for (auto& channel : session.getProject().channels)
        if (channel.audioTrack)
            result.push_back (channel.id);
    return result;
}

std::optional<juce::File> AudioTracks::getTrackFile (model::ChannelId channel) const
{
    std::optional<juce::String> path;
    for (auto& clip : session.getProject().playlist.clips)
        if (auto* audio = std::get_if<model::AudioClip> (&clip.content); audio != nullptr && audio->channel == channel)
        {
            if (path && *path != audio->file)
                return std::nullopt;
            path = audio->file;
        }
    if (! path)
        return std::nullopt;
    const auto file = session.resolveAudioFile (*path);
    return file.existsAsFile() ? std::optional (file) : std::nullopt;
}

juce::String AudioTracks::describeClips (model::ChannelId channel) const
{
    const auto& project = session.getProject();
    int count = 0;
    model::Ticks first = -1, end = 0;
    for (auto& clip : project.playlist.clips)
        if (auto* audio = std::get_if<model::AudioClip> (&clip.content); audio != nullptr && audio->channel == channel)
        {
            ++count;
            first = first < 0 ? clip.start : juce::jmin (first, clip.start);
            end = juce::jmax (end, clip.start + clip.length);
        }
    if (count == 0)
        return "No clips on the Playlist";

    const auto secondsPerTick = 60.0 / (project.settings.tempoBpm * (double)model::ticksPerQuarterNote);
    return juce::String (count) + (count == 1 ? " clip, " : " clips, ") +
           formatSeconds ((double)first * secondsPerTick) + " to " + formatSeconds ((double)end * secondsPerTick);
}

//==============================================================================
std::optional<model::ChannelId> AudioTracks::getRecordTarget() const { return recordTarget; }

void AudioTracks::setRecordTarget (std::optional<model::ChannelId> target)
{
    recordTarget = target;
    updateRecordInto();
}

void AudioTracks::updateRecordInto()
{
    // "New track", then each audio track; a track that went away is forgotten.
    if (recordTarget)
        if (const auto* channel = session.getProject().findChannel (*recordTarget);
            channel == nullptr || ! channel->audioTrack)
            recordTarget.reset();
    recordInto.clear (juce::dontSendNotification);
    recordInto.addItem ("Record into a new track", 1);
    for (const auto& channel : session.getProject().channels)
        if (channel.audioTrack)
            recordInto.addItem ("Record into " + channel.name, (int)channel.id.value + 1);
    recordInto.setSelectedId (recordTarget ? (int)recordTarget->value + 1 : 1, juce::dontSendNotification);
}

void AudioTracks::rebuild()
{
    updateRecordInto();
    std::vector<const model::Channel*> channels;
    for (auto& channel : session.getProject().channels)
        if (channel.audioTrack)
            channels.push_back (&channel);

    if (rows.size() == channels.size() && std::equal (rows.begin(), rows.end(), channels.begin(),
                                                      [] (auto& row, auto* c) { return row->getId() == c->id; }))
    {
        for (size_t i = 0; i < rows.size(); ++i)
            rows[i]->update (*channels[i]);
        repaint();
        return;
    }

    rows.clear();
    for (auto* channel : channels)
    {
        rows.push_back (std::make_unique<Row> (*this, *channel));
        rowHolder.addAndMakeVisible (*rows.back());
    }
    emptyMessage.setVisible (rows.empty());
    resized();
    repaint();
}

void AudioTracks::resized()
{
    viewport.setBounds (getLocalBounds());
    const auto width = viewport.getMaximumVisibleWidth();
    int y = 0;
    for (auto& row : rows)
    {
        row->setBounds (0, y, width, rowHeight);
        y += rowHeight;
    }
    if (rows.empty())
    {
        emptyMessage.setBounds (12, 8, width - 24, 40);
        y = 48;
    }
    addButton.setBounds (8, y + 8, 130, 24);
    addTrackButton.setBounds (addButton.getRight() + 8, y + 8, 130, 24);
    recordButton.setBounds (addTrackButton.getRight() + 8, y + 8, 130, 24);
    recordInto.setBounds (recordButton.getRight() + 8, y + 8, 190, 24);
    recordStatus.setBounds (recordInto.getRight() + 8, y + 8, juce::jmax (0, width - recordInto.getRight() - 16), 24);
    rowHolder.setSize (width, y + 40);
}

void AudioTracks::paint (juce::Graphics& g) { g.fillAll (themeOf (*this).get ("panel")); }

void AudioTracks::paintOverChildren (juce::Graphics& g)
{
    if (dropHighlight)
    {
        g.setColour (themeOf (*this).get ("accent"));
        g.drawRect (getLocalBounds(), 2);
    }
}

void AudioTracks::timerCallback()
{
    for (auto& row : rows)
    {
        row->followAutomation();
        row->updateMeter();
    }

    // Armed tracks' inputs are metered (while the panel is open), so levels can be set before recording.
    auto& audio = session.getAudioRecorder();
    const auto armed = getArmedTracks().size();
    const bool wantMeters = armed > 0 && isShowing();
    if (wantMeters != audio.isMetering())
        if (auto result = audio.setMetering (wantMeters); result.failed())
            recordStatus.setText (result.getErrorMessage(), juce::dontSendNotification);
    audio.keepMetering();
    recordInto.setEnabled (armed == 0 && ! audio.isRecording());

    // Recording: the button stops it; how long and how loud.
    auto& recorder = session.getAudioRecorder();
    const auto recording = recorder.isRecording();
    recordButton.setButtonText (recording ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xa0 Stop recording"))
                                          : juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f Record audio")));
    recordButton.setToggleState (recording, juce::dontSendNotification);
    if (recording)
    {
        const auto seconds = (int)recorder.getSeconds();
        const auto level = juce::Decibels::gainToDecibels (recorder.getInputLevel(), -60.0f);
        recordStatus.setText ("Recording " + juce::String (seconds / 60) + ":" +
                                  juce::String (seconds % 60).paddedLeft ('0', 2) +
                                  (recorder.getNumTakes() > 1 ? " to " : " from ") + recorder.getInputName() + ", " +
                                  (level <= -59.0f ? juce::String ("silent") : juce::String ((int)level) + " dB") +
                                  (level > -1.0f ? " - too loud!" : ""),
                              juce::dontSendNotification);
    }
}

void AudioTracks::toggleRecording()
{
    auto& recorder = session.getAudioRecorder();
    if (recorder.isRecording())
    {
        recorder.stop();
        return;
    }
    if (auto started = recorder.start(); started.failed())
        recordStatus.setText (started.getErrorMessage(), juce::dontSendNotification);
}

void AudioTracks::setRecordStatus (const juce::String& text)
{
    recordStatus.setText (text, juce::dontSendNotification);
}

//==============================================================================
bool AudioTracks::perform (const juce::String& command, const juce::var& parameters, bool joined)
{
    juce::String error;
    auto c = commands::CommandRegistry::get().create (command, parameters, error);
    if (c == nullptr)
        return false;
    auto& history = session.getHistory();
    return (joined ? history.performJoined (std::move (c)) : history.perform (std::move (c))).wasOk();
}

bool AudioTracks::setVolume (model::ChannelId channel, double db, bool joined)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "volumeDb", db } }), joined);
}

bool AudioTracks::setPan (model::ChannelId channel, double pan, bool joined)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "pan", pan } }), joined);
}

bool AudioTracks::setMuted (model::ChannelId channel, bool muted)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "muted", muted } }));
}

bool AudioTracks::rename (model::ChannelId channel, const juce::String& name)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "name", name } }));
}

bool AudioTracks::setArmed (model::ChannelId channel, bool armed)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value }, { "recordArmed", armed } }));
}

bool AudioTracks::setRecordInput (model::ChannelId channel, int input, bool stereo)
{
    return perform ("channel.set", object ({ { "channel", (int)channel.value },
                                             { "recordInput", juce::jmax (0, input) },
                                             { "recordStereo", stereo && input > 0 } }));
}

std::vector<model::ChannelId> AudioTracks::getArmedTracks() const
{
    std::vector<model::ChannelId> armed;
    for (const auto& channel : session.getProject().channels)
        if (channel.audioTrack && channel.recordArmed)
            armed.push_back (channel.id);
    return armed;
}

float AudioTracks::getTrackLevel (model::ChannelId channel) const
{
    const auto* c = session.getProject().findChannel (channel);
    if (c == nullptr)
        return 0.0f;
    auto& recorder = session.getAudioRecorder();
    const auto first = c->recordInput > 0 ? c->recordInput - 1 : recording::getDefaultInput (session.getEdit().engine);
    return juce::jmax (recorder.getInputLevel (first), c->recordStereo ? recorder.getInputLevel (first + 1) : 0.0f);
}

bool AudioTracks::addRecordingTrack()
{
    int number = (int)getTracks().size() + 1;
    auto taken = [this] (const juce::String& n)
    {
        const auto& channels = session.getProject().channels;
        return std::any_of (channels.begin(), channels.end(), [&n] (const model::Channel& c) { return c.name == n; });
    };
    while (taken ("Track " + juce::String (number)))
        ++number;
    return perform ("channel.add", object ({ { "name", "Track " + juce::String (number) }, { "audioTrack", true } }));
}

bool AudioTracks::removeTrack (model::ChannelId channel)
{
    return perform ("channel.remove", object ({ { "channel", (int)channel.value } }));
}

//==============================================================================
void AudioTracks::chooseFiles()
{
    chooser = std::make_unique<core::FileDialog> (
        "Add audio files as tracks", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*." + juce::StringArray::fromTokens (
                   juce::String (project::ownAudioFormats) + ";" + project::ffmpegAudioFormats, ";", {})
                   .joinIntoString (";*."));
    using FileBrowser = juce::FileBrowserComponent;
    chooser->launchAsync ((int)((unsigned)FileBrowser::openMode | (unsigned)FileBrowser::canSelectFiles |
                                (unsigned)FileBrowser::canSelectMultipleItems),
                          [this] (const core::FileDialog& fc)
                          {
                              juce::StringArray files;
                              for (auto& f : fc.getResults())
                                  files.add (f.getFullPathName());
                              if (! files.isEmpty() && onAddFiles)
                                  onAddFiles (files);
                          });
}

bool AudioTracks::isInterestedInFileDrag (const juce::StringArray& files)
{
    return std::any_of (files.begin(), files.end(),
                        [] (const juce::String& f) { return project::isImportableAudio (juce::File (f)); });
}

void AudioTracks::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragExit (files);
    if (onAddFiles)
        onAddFiles (files);
}

bool AudioTracks::isInterestedInDragSource (const SourceDetails& details)
{
    return Browser::sampleFromDrag (details.description).has_value();
}

void AudioTracks::itemDropped (const SourceDetails& details)
{
    if (const auto sample = Browser::sampleFromDrag (details.description))
        filesDropped ({ sample->getFullPathName() }, 0, 0);
}

void AudioTracks::fileDragEnter (const juce::StringArray&, int, int)
{
    dropHighlight = true;
    repaint();
}

void AudioTracks::fileDragExit (const juce::StringArray&)
{
    dropHighlight = false;
    repaint();
}

} // namespace spinus::ui

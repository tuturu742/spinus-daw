#include "AudioSettings.h"

#include "Theme.h"
#include "engine/AudioSetup.h"

namespace spinus::ui
{

namespace
{
/** "JACK (PipeWire's, the lowest latency)": what a device type is, in plain words. */
juce::String describeType (const juce::String& type)
{
    if (type == "JACK")
        return "JACK (through PipeWire: the lowest latency)";
    if (type == "ALSA")
        return "ALSA";
    return type;
}

/** A device's name as the window shows it: ALSA calls PipeWire's default device "Default ALSA Output
    (currently PipeWire Media Server)", also for recording. */
juce::String describeDevice (const juce::String& name, bool input)
{
    if (name.containsIgnoreCase ("PipeWire Media Server") || name == "default")
        return juce::String ("PipeWire: the system's default ") + (input ? "input" : "output");
    return name;
}

constexpr int firstSourceId = 1000; // "Record from": PipeWire's microphones

/** The first stereo pair switched on (0 for outputs 1 + 2), or -1. */
int firstPair (const juce::BigInteger& channels)
{
    const auto first = channels.findNextSetBit (0);
    return first < 0 ? -1 : first / 2;
}
} // namespace

bool AudioSettings::isClutter (const juce::String& name)
{
    return name.containsIgnoreCase ("Plugin") || name.startsWithIgnoreCase ("Rate Converter") ||
           name == "JACK Audio Connection Kit" || name == "Open Sound System";
}

AudioSettings::AudioSettings (juce::AudioDeviceManager& m, PipeWireInput* p) : manager (m), pipeWire (p)
{
    setHelpText ("Audio settings: what plays Spinus's sound, what it records from, sample rate and buffer size");

    auto label = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    };
    label (typeLabel, "Sound system:");
    label (outputLabel, "Play through:");
    label (speakersLabel, "Speakers on:");
    label (inputLabel, "Record from:");
    label (channelLabel, "Microphone on:");
    label (rateLabel, "Sample rate:");
    label (bufferLabel, "Buffer size:");

    typeBox.setHelpText ("How Spinus talks to the sound card. JACK (PipeWire's) has the lowest latency; ALSA works "
                         "everywhere");
    outputBox.setHelpText ("The sound card (or PipeWire) that plays Spinus's sound");
    speakersBox.setHelpText ("Which outputs of the sound card Spinus plays on: a stereo pair, usually the first");
    inputBox.setHelpText ("The sound card (or PipeWire) Spinus records from: Record audio and Record idea use it");
    channelBox.setHelpText ("The input your microphone or instrument is plugged into; the meter beside shows its "
                            "level");
    rateBox.setHelpText ("Samples per second: 48 kHz is the default and what PipeWire uses");
    bufferBox.setHelpText ("How much sound is prepared at a time: smaller is quicker to respond, larger is safer from "
                           "crackles");
    testButton.setHelpText ("Plays a short beep through the chosen speakers");

    typeBox.onChange = [this]
    {
        if (refreshing)
            return;
        const auto& types = manager.getAvailableDeviceTypes();
        if (juce::isPositiveAndBelow (typeBox.getSelectedItemIndex(), types.size()))
            manager.setCurrentAudioDeviceType (types[typeBox.getSelectedItemIndex()]->getTypeName(), true);
    };
    outputBox.onChange = [this]
    {
        if (! refreshing)
            apply ([name = outputNames[outputBox.getSelectedId() - 1]] (auto& s) { s.outputDeviceName = name; });
    };
    inputBox.onChange = [this]
    {
        if (refreshing)
            return;
        const auto id = inputBox.getSelectedId();
        const auto none = id <= 1;
        // A microphone by name: recorded through PipeWire's device, which PipeWire then connects to it.
        const auto source = id - firstSourceId;
        const bool byName = pipeWire != nullptr && juce::isPositiveAndBelow (source, (int)sources.size());
        const auto chosenSource = byName ? sources[(size_t)source].name : juce::String();
        const auto device = byName ? inputNames[pipeWireDevice] : inputNames[id - 2];
        const auto result = apply (
            [none, device] (auto& s)
            {
                s.inputDeviceName = none ? juce::String() : device;
                s.useDefaultInputChannels = false;
                s.inputChannels.clear();
                if (! none)
                    s.inputChannels.setBit (0);
            });
        if (result.failed() && ! PipeWireInput::isPipeWireDevice (device) && pipeWireDevice >= 0)
            problem.setText (problem.getText() + " Another program (PipeWire) may be using it: choose the "
                                                 "microphone by its name at the top of the list instead.",
                             juce::dontSendNotification);
        if (result.wasOk() && ! none && onDefaultInputChosen)
            onDefaultInputChosen (0);
        if (pipeWire != nullptr && result.wasOk() && ! none && PipeWireInput::isPipeWireDevice (device))
        {
            pipeWire->choose (chosenSource);
            refresh();
        }
    };
    speakersBox.onChange = [this]
    {
        if (! refreshing)
            chooseSpeakers (speakersBox.getSelectedId() - 1);
    };
    channelBox.onChange = [this]
    {
        if (! refreshing)
            chooseInputChannel (channelBox.getSelectedId() - 1);
    };
    rateBox.onChange = [this]
    {
        if (! refreshing)
            apply ([rate = (double)rateBox.getSelectedId()] (auto& s) { s.sampleRate = rate; });
    };
    bufferBox.onChange = [this]
    {
        if (! refreshing)
            apply ([size = bufferBox.getSelectedId()] (auto& s) { s.bufferSize = size; });
    };
    testButton.onClick = [this] { manager.playTestSound(); };

    advice.setText ("A buffer of 256 samples is safe (the default); 128 responds quicker, for playing live. If you "
                    "hear crackles, choose a larger one. Use headphones when you record, so the song doesn't get "
                    "into the microphone.",
                    juce::dontSendNotification);
    advice.setMinimumHorizontalScale (1.0f);
    advice.setJustificationType (juce::Justification::topLeft);
    status.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    status.setJustificationType (juce::Justification::topLeft);
    problem.setJustificationType (juce::Justification::topLeft);
    for (auto* c :
         std::initializer_list<juce::Component*> { &typeBox, &outputBox, &speakersBox, &inputBox, &channelBox, &rateBox,
                                                   &bufferBox, &testButton, &status, &advice, &problem })
        addAndMakeVisible (c);

    inputLevel = manager.getInputLevelGetter();
    manager.addChangeListener (this);
    refresh();
    timerCallback();
    startTimerHz (20);
    setSize (580, 470);
}

AudioSettings::~AudioSettings() { manager.removeChangeListener (this); }

void AudioSettings::show (juce::AudioDeviceManager& manager, juce::Component* centreAround, PipeWireInput* pipeWire,
                          std::function<int()> getDefault, std::function<void (int)> setDefault)
{
    juce::DialogWindow::LaunchOptions options;
    auto* settings = new AudioSettings (manager, pipeWire);
    settings->getDefaultInput = std::move (getDefault);
    settings->onDefaultInputChosen = std::move (setDefault);
    settings->refresh();
    options.content.setOwned (settings);
    options.dialogTitle = "Audio settings";
    options.componentToCentreAround = centreAround;
    options.useNativeTitleBar = true;
    options.resizable = false;
    if (centreAround != nullptr)
        if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&centreAround->getLookAndFeel()))
        {
            options.dialogBackgroundColour = lf->getTheme().get ("panel");
            options.content->setLookAndFeel (lf);
        }
    options.launchAsync();
}

//==============================================================================
juce::Result AudioSettings::apply (const std::function<void (juce::AudioDeviceManager::AudioDeviceSetup&)>& change)
{
    auto setup = manager.getAudioDeviceSetup();
    change (setup);
    const auto error = manager.setAudioDeviceSetup (setup, true);
    problem.setText (error.isEmpty() ? juce::String() : "That did not work: " + error, juce::dontSendNotification);
    refresh();
    return error.isEmpty() ? juce::Result::ok() : juce::Result::fail (error);
}

juce::StringArray AudioSettings::getSpeakerChoices() const
{
    juce::StringArray choices;
    if (auto* device = manager.getCurrentAudioDevice())
    {
        const auto outputs = device->getOutputChannelNames().size();
        for (int pair = 0; pair * 2 < outputs; ++pair)
            choices.add (pair * 2 + 1 < outputs ? "Outputs " + juce::String (pair * 2 + 1) + " + " +
                                                      juce::String (pair * 2 + 2) + (pair == 0 ? " (the usual)" : "")
                                                : "Output " + juce::String (pair * 2 + 1));
    }
    return choices;
}

juce::StringArray AudioSettings::getInputChoices() const
{
    juce::StringArray choices;
    if (auto* device = manager.getCurrentAudioDevice())
    {
        const auto names = device->getInputChannelNames();
        for (int i = 0; i < names.size(); ++i)
        {
            // A name of its own ("Mic", "Line 2"), not just its number again ("channel 3").
            const auto number = juce::String (i + 1);
            const bool plain = names[i].isEmpty() || names[i].containsOnly ("0123456789 ") ||
                               names[i].equalsIgnoreCase ("channel " + number) ||
                               names[i].equalsIgnoreCase ("input " + number);
            choices.add ("Input " + number + (plain ? juce::String() : " (" + names[i] + ")"));
        }
    }
    return choices;
}

bool AudioSettings::chooseSpeakers (int pairIndex)
{
    if (pairIndex < 0)
        return false;
    return apply (
               [pairIndex] (auto& s)
               {
                   s.useDefaultOutputChannels = false;
                   s.outputChannels.clear();
                   s.outputChannels.setRange (pairIndex * 2, 2, true);
               })
        .wasOk();
}

bool AudioSettings::chooseInputChannel (int channel)
{
    const auto ok = apply (
                        [channel] (auto& s)
                        {
                            s.useDefaultInputChannels = false;
                            s.inputChannels.clear();
                            if (channel >= 0)
                                s.inputChannels.setBit (channel);
                        })
                        .wasOk();
    // The input recorded when no track is armed (armed tracks switch their own inputs on again).
    if (ok && channel >= 0 && onDefaultInputChosen)
        onDefaultInputChosen (channel);
    return ok;
}

void AudioSettings::refresh()
{
    const juce::ScopedValueSetter<bool> busy (refreshing, true);
    const auto setup = manager.getAudioDeviceSetup();
    auto* device = manager.getCurrentAudioDevice();

    typeBox.clear (juce::dontSendNotification);
    const auto& types = manager.getAvailableDeviceTypes();
    for (int i = 0; i < types.size(); ++i)
    {
        typeBox.addItem (describeType (types[i]->getTypeName()), i + 1);
        if (types[i]->getTypeName() == manager.getCurrentAudioDeviceType())
            typeBox.setSelectedId (i + 1, juce::dontSendNotification);
    }

    outputBox.clear (juce::dontSendNotification);
    inputBox.clear (juce::dontSendNotification);
    inputBox.addItem ("Nothing (no recording)", 1);
    if (auto* type = manager.getCurrentDeviceTypeObject())
    {
        type->scanForDevices();
        const auto outputs = type->getDeviceNames (false), inputs = type->getDeviceNames (true);
        outputNames = outputs;
        inputNames = inputs;
        for (int i = 0; i < outputs.size(); ++i)
            if (! isClutter (outputs[i]) || outputs[i] == setup.outputDeviceName)
                outputBox.addItem (describeDevice (outputs[i], false), i + 1);
        outputBox.setSelectedId (outputs.indexOf (setup.outputDeviceName) + 1, juce::dontSendNotification);

        // Through PipeWire (ALSA's default device, or its PipeWire one): every microphone by its name.
        pipeWireDevice = -1;
        for (int i = 0; i < inputs.size() && pipeWireDevice < 0; ++i)
            if (inputs[i] == "default" || inputs[i].containsIgnoreCase ("PipeWire Media Server"))
                pipeWireDevice = i;
        for (int i = 0; i < inputs.size() && pipeWireDevice < 0; ++i)
            if (inputs[i].containsIgnoreCase ("PipeWire"))
                pipeWireDevice = i;
        sources =
            pipeWire != nullptr && pipeWireDevice >= 0 ? pipeWire->listSources() : std::vector<PipeWireInput::Source>();
        if (! sources.empty())
        {
            inputBox.addSectionHeading ("Microphones and inputs");
            inputBox.addItem (describeDevice (inputs[pipeWireDevice], true), pipeWireDevice + 2);
            for (size_t i = 0; i < sources.size(); ++i)
                inputBox.addItem (sources[i].description, firstSourceId + (int)i);
            inputBox.addSectionHeading ("Directly through ALSA (only when no other program uses it)");
        }
        for (int i = 0; i < inputs.size(); ++i)
            if ((sources.empty() || i != pipeWireDevice) &&
                (! isClutter (inputs[i]) || inputs[i] == setup.inputDeviceName))
                inputBox.addItem (describeDevice (inputs[i], true), i + 2);

        auto selected = 1;
        if (setup.inputDeviceName.isNotEmpty() && ! setup.inputChannels.isZero())
        {
            selected = inputs.indexOf (setup.inputDeviceName) + 2;
            if (! sources.empty() && setup.inputDeviceName == inputs[pipeWireDevice])
                for (size_t i = 0; i < sources.size(); ++i)
                    if (sources[i].name == pipeWire->getChosen())
                        selected = firstSourceId + (int)i;
        }
        inputBox.setSelectedId (selected, juce::dontSendNotification);
    }

    // Outputs as stereo pairs, inputs one by one: only shown when there is a choice.
    speakersBox.clear (juce::dontSendNotification);
    const auto speakers = getSpeakerChoices();
    for (int i = 0; i < speakers.size(); ++i)
        speakersBox.addItem (speakers[i], i + 1);
    speakersBox.setSelectedId (firstPair (setup.outputChannels) + 1, juce::dontSendNotification);
    speakersBox.setVisible (speakers.size() > 1);
    speakersLabel.setVisible (speakersBox.isVisible());

    channelBox.clear (juce::dontSendNotification);
    const auto inputs = getInputChoices();
    for (int i = 0; i < inputs.size(); ++i)
        channelBox.addItem (inputs[i], i + 1);
    // Several inputs on (tracks armed to record from them): the one recorded when none is armed.
    const auto shown = getDefaultInput ? getDefaultInput() : setup.inputChannels.findNextSetBit (0);
    channelBox.setSelectedId ((setup.inputChannels[shown] ? shown : setup.inputChannels.findNextSetBit (0)) + 1,
                              juce::dontSendNotification);
    channelBox.setTextWhenNothingSelected ("Choose the input");
    channelBox.setVisible (inputBox.getSelectedId() != 1 && ! inputs.isEmpty());
    channelLabel.setVisible (channelBox.isVisible());

    rateBox.clear (juce::dontSendNotification);
    bufferBox.clear (juce::dontSendNotification);
    if (device != nullptr)
    {
        for (auto rate : device->getAvailableSampleRates())
            rateBox.addItem (juce::String (rate / 1000.0, (int)rate % 1000 == 0 ? 0 : 1) + " kHz", (int)rate);
        rateBox.setSelectedId ((int)device->getCurrentSampleRate(), juce::dontSendNotification);
        // The usual sizes (powers of two), and the current one.
        const auto rate = juce::jmax (1.0, device->getCurrentSampleRate());
        for (auto size : device->getAvailableBufferSizes())
            if (juce::isPowerOfTwo (size) || size == device->getCurrentBufferSizeSamples())
                bufferBox.addItem (juce::String (size) + " samples (" + juce::String (1000.0 * size / rate, 1) + " ms)",
                                   size);
        bufferBox.setSelectedId (device->getCurrentBufferSizeSamples(), juce::dontSendNotification);
    }
    resized();
}

void AudioSettings::timerCallback()
{
    status.setText (engine::AudioSetup::getStatus (manager).describe(), juce::dontSendNotification);
    const auto level = inputLevel != nullptr ? (float)inputLevel->getCurrentLevel() : 0.0f;
    if (std::abs (level - shownLevel) > 0.001f)
    {
        shownLevel = level;
        repaint (meterArea);
    }
}

void AudioSettings::paint (juce::Graphics& g)
{
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&getLookAndFeel());
    g.fillAll (lf != nullptr ? lf->getTheme().get ("panel") : juce::Colours::darkgrey);

    // The chosen input's level: is the microphone heard?
    if (channelBox.isVisible() && ! meterArea.isEmpty())
    {
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRoundedRectangle (meterArea.toFloat(), 3.0f);
        const auto db = juce::Decibels::gainToDecibels (shownLevel, -60.0f);
        const auto fill = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (db > -3.0f ? juce::Colour (0xffd65c5c) : juce::Colour (0xff6ccf6a));
        g.fillRoundedRectangle (meterArea.toFloat().withWidth ((float)meterArea.getWidth() * fill), 3.0f);
    }
}

void AudioSettings::resized()
{
    auto area = getLocalBounds().reduced (14);
    advice.setBounds (area.removeFromBottom (54));
    status.setBounds (area.removeFromBottom (44));
    problem.setBounds (area.removeFromBottom (problem.getText().isEmpty() ? 0 : 36));

    auto row = [&area] (juce::Label& label, juce::Component& box, juce::Component* extra = nullptr, int extraWidth = 0)
    {
        if (! box.isVisible())
            return juce::Rectangle<int>();
        auto r = area.removeFromTop (28);
        area.removeFromTop (6);
        label.setBounds (r.removeFromLeft (140));
        r.removeFromLeft (8);
        juce::Rectangle<int> extraArea;
        if (extraWidth > 0)
        {
            extraArea = r.removeFromRight (extraWidth);
            r.removeFromRight (8);
            if (extra != nullptr)
                extra->setBounds (extraArea);
        }
        box.setBounds (r);
        return extraArea;
    };
    row (typeLabel, typeBox);
    area.removeFromTop (6);
    row (outputLabel, outputBox, &testButton, 70);
    row (speakersLabel, speakersBox);
    area.removeFromTop (6);
    row (inputLabel, inputBox);
    meterArea = row (channelLabel, channelBox, nullptr, 110).reduced (0, 9);
    area.removeFromTop (6);
    row (rateLabel, rateBox);
    row (bufferLabel, bufferBox);
}

} // namespace spinus::ui

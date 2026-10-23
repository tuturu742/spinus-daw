#include "PatternGenerator.h"

#include "dsp/drums/DrumMachineProcessor.h"
#include "model/StockDevices.h"

namespace spinus::ui
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

juce::var notesToJson (const std::vector<model::Note>& notes)
{
    juce::Array<juce::var> list;
    for (auto& n : notes)
        list.add (object ({ { "start", (juce::int64)n.start },
                            { "length", (juce::int64)n.length },
                            { "pitch", n.pitch },
                            { "velocity", n.velocity } }));
    return list;
}

/** The Spinus Synth Kit's pads, for a Drum Machine whose plugin is not loaded (frozen, or missing). */
std::vector<std::pair<juce::String, int>> defaultPads()
{
    static const char* names[] = { "Kick",    "Snare",    "Clap", "Closed Hat", "Open Hat", "Low Tom",
                                   "Mid Tom", "High Tom", "Rim",  "Cowbell",    "Crash",    "Shaker",
                                   "Kick 2",  "Snare 2",  "Ride", "Perc" };
    std::vector<std::pair<juce::String, int>> pads;
    pads.reserve (std::size (names));
    for (int i = 0; i < (int)std::size (names); ++i)
        pads.emplace_back (names[i], 60 + i);
    return pads;
}
} // namespace

PatternGenerator::PatternGenerator (Session& s, ai::AiService& ai) : session (s), service (ai)
{
    listener = service.addListener (
        [this] (const juce::String& method, const juce::var& params)
        {
            if (method != "ai.v1.job.update" || job.isEmpty() || params["job"].toString() != job)
                return;
            const auto kind = params["kind"].toString();
            if (kind == "status")
                session.getSuggestions().setWorking (params["text"].toString());
            else if (kind == "done")
            {
                job.clear();
                if (auto done = std::exchange (whenDone, nullptr))
                    done (params["result"]);
            }
            else if (kind == "error")
            {
                job.clear();
                fail (params["message"].toString());
            }
        });
    session.getSuggestions().addChangeListener (this);
}

PatternGenerator::~PatternGenerator()
{
    session.getSuggestions().removeChangeListener (this);
    service.removeListener (listener);
}

std::vector<std::pair<juce::String, int>> PatternGenerator::getPads (Session& session, model::ChannelId id)
{
    const auto* channel = session.getProject().findChannel (id);
    if (channel == nullptr || ! channel->instrument || channel->instrument->stockType != "spinus.drum-machine")
        return {};

    if (auto* external =
            dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (channel->instrument->id)))
        if (auto* drums = dynamic_cast<dsp::DrumMachineProcessor*> (external->getAudioPluginInstance()))
        {
            std::vector<std::pair<juce::String, int>> pads;
            for (auto& pad : drums->getPads())
                if (pad.audio != nullptr)
                    pads.emplace_back (pad.name.isNotEmpty() ? pad.name : "Pad " + juce::String (pad.note), pad.note);
            return pads;
        }
    return defaultPads();
}

juce::var PatternGenerator::describe (const Request& request) const
{
    const auto& project = session.getProject();
    const auto* pattern = project.findPattern (session.getCurrentPattern());

    auto channelJson = [&] (const model::Channel& c, bool withId)
    {
        auto o =
            object ({ { "name", c.name },
                      { "instrument", c.instrument ? model::getDeviceDisplayName (*c.instrument) : juce::String() } });
        if (withId)
            o.getDynamicObject()->setProperty ("channel", (int)c.id.value);
        juce::Array<juce::var> pads;
        for (auto& [name, note] : getPads (session, c.id))
            pads.add (object ({ { "name", name }, { "note", note } }));
        o.getDynamicObject()->setProperty ("pads", pads);
        if (pattern != nullptr)
            if (auto it = pattern->notes.find (c.id); it != pattern->notes.end())
                o.getDynamicObject()->setProperty ("notes", notesToJson (it->second));
        return o;
    };

    juce::Array<juce::var> targets, context;
    for (auto& c : project.channels)
    {
        const bool target =
            std::find (request.channels.begin(), request.channels.end(), c.id) != request.channels.end();
        if (target)
            targets.add (channelJson (c, true));
        else if (pattern != nullptr && pattern->notes.contains (c.id) && ! pattern->notes.at (c.id).empty())
            context.add (channelJson (c, false));
    }

    const auto& s = project.settings;
    const auto sig = project.getTimeSignature (session.getCurrentPattern());
    auto params = object ({ { "request", request.text },
                            { "targets", targets },
                            { "context", context },
                            { "tempo", s.tempoBpm },
                            { "beatsPerBar", sig.numerator },
                            { "barTicks", (juce::int64)sig.barTicks() },
                            { "timeSignature", juce::String (sig.numerator) + "/" + juce::String (sig.denominator) },
                            { "lengthTicks", (juce::int64)lengthOf (request) },
                            { "variations", request.variations },
                            { "adjust", request.adjust },
                            { "swing", request.swing },
                            { "humanize", request.humanize },
                            { "seed", juce::Random::getSystemRandom().nextInt (1000000000) } });
    if (s.key)
        params.getDynamicObject()->setProperty ("key",
                                                object ({ { "tonic", s.key->tonic }, { "scale", s.key->scale } }));
    return params;
}

juce::Result PatternGenerator::generate (const Request& request)
{
    if (! service.isEnabled())
        return juce::Result::fail ("The pattern generator uses AI: turn it on in Options › AI.");
    if (isBusy())
        return juce::Result::fail ("The pattern generator is already writing.");
    if (request.channels.empty())
        return juce::Result::fail ("Choose at least one channel to write notes for.");
    if (request.text.trim().isEmpty() && request.adjust)
        return juce::Result::fail ("Say how to change the notes, e.g. \"more syncopated\".");
    if (! session.getCurrentPattern().isValid())
        return juce::Result::fail ("There is no pattern: add one first.");

    const auto pattern = session.getCurrentPattern();
    session.getSuggestions().setWorking ("Writing a pattern...");
    waiting = true;
    whenDone = [this, request, pattern] (const juce::var& result) { finished (result, request, pattern); };
    service.call ("ai.v1.pattern.generate", describe (request),
                  [this] (const juce::var& result, const juce::String& error)
                  {
                      waiting = false;
                      if (error.isNotEmpty())
                          return fail (error);
                      job = result["job"].toString();
                  });
    return juce::Result::ok();
}

std::vector<Suggestions::Suggestion>
PatternGenerator::toSuggestions (const juce::var& written, model::PatternId pattern, const juce::String& fallbackTitle)
{
    std::vector<Suggestions::Suggestion> variations;
    if (auto* list = written.getArray())
        for (auto& v : *list)
        {
            Suggestions::Suggestion suggestion;
            suggestion.title = v["title"].toString().isNotEmpty() ? v["title"].toString() : fallbackTitle;
            suggestion.pattern = pattern;
            if (auto* parts = v["parts"].getDynamicObject())
                for (auto& [channel, notes] : parts->getProperties())
                {
                    std::vector<model::Note> notesWritten;
                    if (auto* array = notes.getArray())
                        for (auto& n : *array)
                        {
                            model::Note note;
                            note.start = (model::Ticks)(juce::int64)n["start"];
                            note.length = (model::Ticks)(juce::int64)n["length"];
                            note.pitch = (int)n["pitch"];
                            note.velocity = (float)(double)n["velocity"];
                            notesWritten.push_back (note);
                        }
                    suggestion.notes[model::ChannelId { (uint32_t)channel.toString().getIntValue() }] =
                        std::move (notesWritten);
                }
            if (! suggestion.notes.empty())
                variations.push_back (std::move (suggestion));
        }
    return variations;
}

model::Ticks PatternGenerator::lengthOf (const Request& request) const
{
    const auto& project = session.getProject();
    const auto* pattern = project.findPattern (session.getCurrentPattern());
    const auto& sig = project.settings.timeSignature;
    const auto bar = sig.denominator > 0
                         ? (model::Ticks)sig.numerator * model::ticksPerQuarterNote * 4 / sig.denominator
                         : model::ticksPerQuarterNote * 4;
    if (request.bars > 0)
        return request.bars * bar;
    return pattern != nullptr ? pattern->length : bar;
}

void PatternGenerator::finished (const juce::var& result, const Request& request, model::PatternId pattern)
{
    auto variations = toSuggestions (result["variations"], pattern, request.text);
    if (variations.empty())
        return fail ("The AI did not write any usable notes. Try again.");
    // Longer than the pattern: accepting makes the pattern that long too.
    if (const auto* p = session.getProject().findPattern (pattern); p != nullptr && request.bars > 0)
        for (auto& v : variations)
            if (lengthOf (request) > p->length)
                v.patternLength = lengthOf (request);

    session.getSuggestions().show (std::move (variations), { "Pattern generator", [this, request]
                                                             {
                                                                 if (auto again = generate (request); again.failed())
                                                                     fail (again.getErrorMessage());
                                                             } });
}

void PatternGenerator::fail (const juce::String& message)
{
    whenDone = nullptr;
    session.getSuggestions().setWorking ("Pattern generator: " + message);
}

void PatternGenerator::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Discard while writing: stop asking.
    if (job.isNotEmpty() && session.getSuggestions().getWorkingMessage().isEmpty() &&
        ! session.getSuggestions().isShowing())
    {
        service.call ("ai.v1.job.cancel", object ({ { "job", job } }), [] (const juce::var&, const juce::String&) {});
        job.clear();
        whenDone = nullptr;
    }
}

//==============================================================================
namespace
{
/** What to write, for which channels: under the Piano Roll's Generate button. */
class GenerateDialog final : public juce::Component
{
public:
    GenerateDialog (PatternGenerator& g, Session& s) : generator (g), session (s)
    {
        prompt.setTextToShowWhenEmpty ("e.g. a funky bass line for these drums, 2 bars", juce::Colours::grey);
        prompt.setMultiLine (true, true);
        prompt.setReturnKeyStartsNewLine (false);
        prompt.onReturnKey = [this] { submit (false); };
        addAndMakeVisible (prompt);

        heading.setText ("What should it play?", juce::dontSendNotification);
        forLabel.setText ("Write notes for:", juce::dontSendNotification);
        for (auto* l : { &heading, &forLabel })
            addAndMakeVisible (l);

        const auto* pattern = s.getProject().findPattern (s.getCurrentPattern());
        for (auto& c : s.getProject().channels)
        {
            auto toggle = std::make_unique<juce::ToggleButton> (c.name);
            toggle->setToggleState (c.id == s.getSelectedChannel(), juce::dontSendNotification);
            toggle->onClick = [this] { updateButtons(); };
            channelList.addAndMakeVisible (*toggle);
            channels.push_back (
                { c.id, std::move (toggle),
                  pattern != nullptr && pattern->notes.contains (c.id) && ! pattern->notes.at (c.id).empty() });
        }
        viewport.setViewedComponent (&channelList, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        const auto* current = s.getProject().findPattern (s.getCurrentPattern());
        const auto bar = (double)s.getProject().getTimeSignature (s.getCurrentPattern()).barTicks();
        patternBars = current != nullptr ? juce::jmax (1, juce::roundToInt ((double)current->length / bar)) : 1;
        bars.setEditableText (true);
        bars.addItemList ({ "1", "2", "4", "8", "16" }, 1);
        bars.setText (juce::String (patternBars), juce::dontSendNotification);
        bars.setTooltip ("How many bars to write (type any number up to 64); a longer part makes the pattern longer");
        barsLabel.setText ("Bars", juce::dontSendNotification);
        addAndMakeVisible (bars);
        addAndMakeVisible (barsLabel);

        variations.addItemList ({ "1 variation", "4 variations" }, 1);
        variations.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (variations);

        for (auto [slider, label, text] :
             { std::tuple { &swing, &swingLabel, "Swing" }, std::tuple { &humanize, &humanizeLabel, "Humanize" } })
        {
            slider->setRange (0.0, 1.0, 0.01);
            slider->setSliderStyle (juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            label->setText (text, juce::dontSendNotification);
            addAndMakeVisible (slider);
            addAndMakeVisible (label);
        }

        write.setButtonText ("Write");
        write.setHelpText ("Write new notes for the chosen channels (a suggestion: Accept or Discard)");
        write.onClick = [this] { submit (false); };
        change.setButtonText ("Change their notes");
        change.setHelpText ("Change the chosen channels' notes as you describe (more syncopated, add a fill...)");
        change.onClick = [this] { submit (true); };
        addAndMakeVisible (write);
        addAndMakeVisible (change);

        error.setColour (juce::Label::textColourId, juce::Colours::orange);
        addAndMakeVisible (error);
        updateButtons();
        setSize (480, 330);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        heading.setBounds (area.removeFromTop (20));
        prompt.setBounds (area.removeFromTop (54));
        area.removeFromTop (6);
        forLabel.setBounds (area.removeFromTop (20));
        viewport.setBounds (area.removeFromTop (92));
        const auto rows = ((int)channels.size() + 1) / 2;
        channelList.setSize (viewport.getWidth() - 10, juce::jmax (1, rows) * 22);
        for (size_t i = 0; i < channels.size(); ++i)
            channels[i].toggle->setBounds ((int)(i % 2) * channelList.getWidth() / 2, (int)(i / 2) * 22,
                                           channelList.getWidth() / 2, 22);
        area.removeFromTop (6);
        auto row = area.removeFromTop (24);
        barsLabel.setBounds (row.removeFromLeft (34));
        bars.setBounds (row.removeFromLeft (56));
        row.removeFromLeft (8);
        variations.setBounds (row.removeFromLeft (110));
        row.removeFromLeft (10);
        swingLabel.setBounds (row.removeFromLeft (46));
        swing.setBounds (row.removeFromLeft (row.getWidth() / 2 - 30));
        humanizeLabel.setBounds (row.removeFromLeft (64));
        humanize.setBounds (row);
        error.setBounds (area.removeFromBottom (20));
        auto buttons = area.removeFromBottom (28);
        write.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        change.setBounds (buttons.removeFromRight (150));
    }

private:
    struct Choice
    {
        model::ChannelId id;
        std::unique_ptr<juce::ToggleButton> toggle;
        bool hasNotes = false;
    };

    void updateButtons()
    {
        const auto chosen =
            std::count_if (channels.begin(), channels.end(), [] (auto& c) { return c.toggle->getToggleState(); });
        const auto withNotes = std::count_if (channels.begin(), channels.end(),
                                              [] (auto& c) { return c.toggle->getToggleState() && c.hasNotes; });
        write.setEnabled (chosen > 0);
        change.setEnabled (withNotes > 0);
    }

    void submit (bool adjust)
    {
        PatternGenerator::Request request;
        request.text = prompt.getText().trim();
        for (auto& c : channels)
            if (c.toggle->getToggleState())
                request.channels.push_back (c.id);
        request.variations = variations.getSelectedId() == 2 ? 4 : 1;
        request.swing = (float)swing.getValue();
        request.humanize = (float)humanize.getValue();
        request.adjust = adjust;
        const auto chosenBars = bars.getText().trim().getIntValue();
        if (chosenBars < 1 || chosenBars > 64)
        {
            error.setText ("Bars: a number from 1 to 64.", juce::dontSendNotification);
            return;
        }
        request.bars = chosenBars == patternBars ? 0 : chosenBars;

        if (auto result = generator.generate (request); result.failed())
        {
            error.setText (result.getErrorMessage(), juce::dontSendNotification);
            return;
        }
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    PatternGenerator& generator;
    Session& session;
    juce::Label heading, forLabel, swingLabel, humanizeLabel, barsLabel, error;
    juce::ComboBox bars;
    int patternBars = 1;
    juce::TextEditor prompt;
    juce::Viewport viewport;
    juce::Component channelList;
    std::vector<Choice> channels;
    juce::ComboBox variations;
    juce::Slider swing, humanize;
    juce::TextButton write, change;
};
} // namespace

void PatternGenerator::showDialog (juce::Component& button)
{
    auto dialog = std::make_unique<GenerateDialog> (*this, session);
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (dialog), button.getScreenBounds(), nullptr);
    juce::ignoreUnused (box);
}

} // namespace spinus::ui

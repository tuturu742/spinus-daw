#include "SongWriterPanel.h"

#include "ai/AnalysisJobs.h"
#include "ai/Proposal.h"
#include "session/SoundIndex.h"

#include <array>

namespace spinus::ui
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : items)
        o->setProperty (key, value);
    return juce::var (o);
}

/** The sound chosen on the Sounds page for a song part, for a channel the song makes; none for a drum loop
    (it plays as audio: the song's drums then get Spinus's Drum Machine). */
std::optional<song::Sound> soundOf (const parts::Choice& c)
{
    if (c.kind == parts::Choice::Kind::loop)
        return std::nullopt;
    song::Sound sound;
    sound.instrument = c.deviceJson();
    if (c.kind == parts::Choice::Kind::preset && c.source.stockType == "spinus.3x-osc" &&
        c.preset.startsWith ("program:"))
        sound.preset = c.preset.fromLastOccurrenceOf (":", false, false);
    if (c.kind == parts::Choice::Kind::kit)
    {
        sound.drums = song::drumNotesFor (model::Channel {}); // General MIDI, with the kit's own where it says
        for (const auto& [drum, note] : c.drumNotes)
            sound.drums[drum] = note;
    }
    return sound;
}

const parts::Plan::Part* chosenFor (const parts::Plan& plan, const juce::String& part)
{
    for (const auto& p : plan.parts)
        if (p.target.part == song::summaryPart (part))
            return &p;
    return nullptr;
}

} // namespace

const parts::Plan::Part* SongWriterPanel::pagePartFor (const song::Layer& layer) const
{
    // The Sounds page's part of the same id: its choice decides the sound.
    for (const auto& p : context.sounds.parts)
        if (p.target.id == layer.id)
            return &p;
    return nullptr;
}

const std::map<juce::String, parts::Choice>& SongWriterPanel::getLayerChoices()
{
    if (layerChoicesKnown)
        return layerChoices;
    layerChoicesKnown = true;
    layerChoices.clear();
    const auto layers = song::layersOf (song);
    if (layers.empty() || context.indexFolder == juce::File())
        return layerChoices;
    SoundIndex index (context.indexFolder);
    index.load();
    if (index.instruments.empty())
        return layerChoices;
    // Varied: each part another instrument when one fits as well, and another song (seed) other ones.
    std::set<juce::String> used;
    const auto seed = (juce::uint32)(int)song["seeds"]["song"];
    for (const auto& layer : layers)
    {
        // What the Sounds page chose stays; the drums without a choice there get Spinus's Drum Machine.
        if (layer.drums || pagePartFor (layer) != nullptr)
            continue;
        // The sound the AI picked for it, from the candidates it was given.
        if (const auto picked = song["sounds"][juce::Identifier (layer.id)].toString(); picked.isNotEmpty())
            if (const auto part = candidateChoices.find (layer.id); part != candidateChoices.end())
                if (const auto c = part->second.find (picked); c != part->second.end())
                {
                    used.insert (c->second.instrument);
                    layerChoices[layer.id] = c->second;
                    continue;
                }
        parts::Target target;
        target.part = layer.part == "fx" ? juce::String ("fx") : song::summaryPart (layer.part);
        target.roles = { layer.soundRole };
        target.low = layer.low;
        target.median = layer.middle;
        target.high = layer.high;
        target.chords = layer.role == "pad" || (layer.role == "chords" && layer.instance == 0);
        target.voices = target.chords ? 3.0 : 1.0;
        // A described song names instruments to look for ("piano", or its genre's palette): the closest sound
        // whose name says it.
        const auto lookFor = context.summary["described"]["sounds"][juce::Identifier (layer.role)].toString();
        target.names = parts::targetFor ({ layer.id, layer.name, layer.role }, layer.instance, context.summary).names;
        const auto found =
            parts::vary (parts::choose (target, {}, index, 40), used, seed + (juce::uint32)layer.id.hashCode(), 40);
        const parts::Choice* best = nullptr;
        for (const auto& c : found)
            if (c.kind == parts::Choice::Kind::preset && c.role == layer.soundRole && best == nullptr)
                best = &c;
        if (lookFor.isNotEmpty())
            for (const auto& c : found)
                if (c.kind == parts::Choice::Kind::preset && (c.name + " " + c.category).containsIgnoreCase (lookFor))
                {
                    best = &c;
                    break;
                }
        if (best != nullptr)
        {
            used.insert (best->instrument);
            layerChoices[layer.id] = *best;
        }
    }
    return layerChoices;
}

std::optional<parts::Plan::Part> SongWriterPanel::chosenForLayer (const song::Layer& layer)
{
    if (const auto* chosen = pagePartFor (layer))
        return *chosen;
    const auto& choices = getLayerChoices();
    if (const auto found = choices.find (layer.id); found != choices.end())
    {
        parts::Plan::Part part;
        part.target.part = layer.part;
        part.target.id = layer.id;
        part.target.name = layer.name;
        part.target.role = layer.role;
        part.target.roles = { layer.soundRole };
        part.choice = found->second;
        return part;
    }
    return std::nullopt;
}

SongWriterPanel::SongWriterPanel (Session& s, std::function<Context()> c) : session (s), getContext (std::move (c))
{
    intro.setText ("The last step: a new song with the parts of step 3, in the style of step 2, written by Spinus's "
                   "rules on this computer or, if you choose, by your AI connection. Apply puts it in the project to "
                   "hear.",
                   juce::dontSendNotification);
    intro.setMinimumHorizontalScale (1.0f);
    intro.setJustificationType (juce::Justification::topLeft);
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (1.0f);
    messageBox = [this] (const juce::MessageBoxOptions& options, std::function<void (int)> done)
    {
        const auto n = options.getNumButtons();
        juce::AlertWindow::showAsync (options.withAssociatedComponent (this),
                                      [n, done = std::move (done)] (int result)
                                      {
                                          if (done)
                                              done ((result + n - 1) % juce::jmax (1, n));
                                      });
    };
    refineButton.setTooltip ("Ask your AI connection (Options > AI) to vary the sections, the chord loops and the "
                             "melody's shape; Spinus checks what it says against the style and writes every note "
                             "itself. Off: rules only, nothing is sent anywhere");
    writeButton.setTooltip ("Write a song from the style summary");
    writeButton.onClick = [this] { write(); };
    againButton.setTooltip ("Write another song in the same style (a new random seed)");
    againButton.onClick = [this] { anotherVersion(); };
    redoButton.setTooltip ("Write one part (drums, bass, chords, lead) or one section again, keeping the rest");
    redoButton.onClick = [this] { showRedoMenu(); };
    stopButton.onClick = [this] { stop(); };
    applyButton.setTooltip ("Put the song into the project: patterns, channels, the Playlist, tempo and key. One "
                            "undo step");
    applyButton.onClick = [this] { apply(); };
    mixButton.setTooltip ("Open the automixer to balance the parts, with the first reference song as its reference");
    mixButton.onClick = [this] { mix(); };
    writerBox.addItem ("Spinus (offline, nothing is sent)", 1);
    writerBox.addItem ("Your AI connection", 2);
    writerBox.setSelectedId (1, juce::dontSendNotification);
    writerBox.setTooltip ("Spinus's own rules write on this computer. Your AI connection (Options > AI) composes the "
                          "whole song with the music harness's tools, checked by Spinus; it sends the style, your "
                          "parts and their sounds' names to your AI provider and uses tokens");
    writerBox.onChange = [this]
    {
        updateButtons();
        refreshEstimate();
    };
    effortBox.addItem ("Reasoning: the model's own", 1);
    effortBox.addItem ("Reasoning: low", 2);
    effortBox.addItem ("Reasoning: medium", 3);
    effortBox.addItem ("Reasoning: high", 4);
    effortBox.setSelectedId (1, juce::dontSendNotification);
    effortBox.setTooltip ("How hard the model thinks before it writes: higher levels can use several times more "
                          "tokens");
    effortBox.onChange = [this] { refreshEstimate(); };
    roundsBox.addItem ("Then fix what Spinus hears: no", 1);
    roundsBox.addItem ("Then fix what Spinus hears: once", 2);
    roundsBox.addItem ("Then fix what Spinus hears: twice", 3);
    roundsBox.addItem ("Then fix what Spinus hears: 3 times", 4);
    roundsBox.setSelectedId (2, juce::dontSendNotification);
    roundsBox.setTooltip ("After the AI finishes, Spinus plays each part with its sound; what it hears (a part that "
                          "made no sound, the balance) goes back to the AI to fix, this many times");
    roundsBox.onChange = [this] { refreshEstimate(); };
    stopNowButton.setTooltip ("Stop the AI after its current step and keep its best checked version of the song");
    stopNowButton.onClick = [this] { stopNow(); };
    estimateLabel.setJustificationType (juce::Justification::topLeft);
    estimateLabel.setMinimumHorizontalScale (1.0f);
    text.setMultiLine (true);
    text.setReadOnly (true);
    text.setScrollbarsShown (true);
    text.setCaretVisible (false);
    for (auto* component :
         std::initializer_list<juce::Component*> { &intro, &refineButton, &writeButton, &againButton, &redoButton,
                                                   &applyButton, &mixButton, &status, &text, &writerLabel, &writerBox })
        addAndMakeVisible (component);
    addChildComponent (stopButton);
    for (auto* component :
         std::initializer_list<juce::Component*> { &effortBox, &roundsBox, &estimateLabel, &stopNowButton })
        addChildComponent (component);
    updateButtons();
}

SongWriterPanel::~SongWriterPanel()
{
    *alive = false;
    if (context.service != nullptr && job.isNotEmpty())
        context.service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

bool SongWriterPanel::aiReady() const { return context.service != nullptr && context.service->isEnabled(); }

void SongWriterPanel::run (const juce::var& params, const juce::String& words,
                           std::function<void (const juce::var&)> then, const juce::String& method)
{
    busy = true;
    status.setText (words, juce::dontSendNotification);
    updateButtons();
    ai::runJob (
        *context.service, method, params, alive, [this] (const juce::String& started) { job = started; },
        [this, then = std::move (then)] (const juce::var& result, const juce::String& error)
        {
            job.clear();
            busy = false;
            if (error.isNotEmpty())
            {
                composing = false;
                status.setText ((error == "Stopped." && song.isObject()
                                     ? juce::String ("Cancelled: the song shown before stays.")
                                     : "The song could not be written: " + error),
                                juce::dontSendNotification);
            }
            else
                then (result);
            updateButtons();
        },
        [this, words] (double, double, const juce::String& step)
        {
            // The AI writer's steps in plain words, with the running total of tokens.
            if (step.isNotEmpty())
                status.setText (words.upToFirstOccurrenceOf ("...", false, false) + ": " + step,
                                juce::dontSendNotification);
        });
}

bool SongWriterPanel::write()
{
    if (busy)
        return false;
    context = getContext();
    if (! context.summary.isObject())
    {
        status.setText ("There is no style summary yet: add songs on the first page and let them be analyzed.",
                        juce::dontSendNotification);
        return false;
    }
    if (! aiReady())
    {
        status.setText ("Writing a song needs Spinus's AI service, which runs on this computer (no AI connection "
                        "is needed): turn AI on in Options > AI. Everything else in Spinus works without it.",
                        juce::dontSendNotification);
        return false;
    }
    if (context.partsFromPage && context.parts.empty())
    {
        status.setText ("The song has no parts yet: add them in step 3, Parts (Add part..., or Suggest parts), "
                        "then Write.",
                        juce::dontSendNotification);
        return false;
    }
    const auto seed = fixedSeed >= 0 ? fixedSeed++ : juce::Random::getSystemRandom().nextInt (0x7fffffff);
    if (isAiWriter())
    {
        composing = true;
        stopRequested = false;
        roundsLeft = getRounds();
        composeRequest = composeParams (seed);
        composeRound (composeRequest, "Writing with " + connectionName + "...");
        return true;
    }
    const bool refine = refineButton.getToggleState();
    // The Sounds page's parts, as the user made the list.
    juce::Array<juce::var> list;
    for (const auto& p : context.parts)
        list.add (object ({ { "id", p.id }, { "name", p.name }, { "role", p.role } }));
    auto params = object ({ { "summary", context.summary }, { "seed", seed }, { "refine", refine } });
    if (! list.isEmpty())
        params.getDynamicObject()->setProperty ("parts", list);
    run (params, refine ? "Writing a song (the AI may vary its plan)..." : "Writing a song...",
         [this] (const juce::var& result)
         {
             song = result;
             layerChoicesKnown = false;
             whole = true;
             redoneParts.clear();
             redoneSections.clear();
             show();
             status.setText ("Written. Apply puts it in the project to hear (Ctrl+Z takes it away); or write another "
                             "version or redo a part first.",
                             juce::dontSendNotification);
             balance();
         });
    return true;
}

namespace
{
/** How loud a recording is: its RMS over all of it, in dB (-100: silent or unreadable). */
double rmsDb (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return -100.0;
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    double sum = 0.0;
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            sum += (double)audio.getSample (c, i) * audio.getSample (c, i);
    const auto mean = sum / ((double)audio.getNumChannels() * audio.getNumSamples());
    return mean > 1.0e-10 ? 10.0 * std::log10 (mean) : -100.0;
}
} // namespace

void SongWriterPanel::balance (
    std::function<void (const std::map<juce::String, double>&, const std::map<juce::String, juce::String>&)> then)
{
    faders.clear();
    const auto nothing = [&then]
    {
        if (then)
            then ({}, {});
    };
    if (! song::isFull (song) || context.helper.helper == juce::File())
        return nothing();
    // Each layer's busiest bars, played with its sound in the helper (as Listen plays a part).
    struct Measure
    {
        SoundSource source;
        juce::Array<juce::var> items;
    };
    std::map<juce::String, Measure> byInstrument;
    const auto folder = context.workFolder.getChildFile ("levels");
    folder.deleteRecursively();
    folder.createDirectory();
    for (const auto& layer : song::layersOf (song))
    {
        const auto chosen = layer.part == "fx" ? std::nullopt : chosenForLayer (layer);
        if (! chosen)
            continue;
        const auto& c = chosen->choice;
        using Kind = parts::Choice::Kind;
        if ((c.kind != Kind::preset && c.kind != Kind::kit) || (layer.drums && c.drumNotes.empty()))
            continue;
        const auto notes = song::layerExcerpt (song, layer, c.drumNotes);
        if (notes.empty())
            continue;
        juce::Array<juce::var> played;
        for (const auto& n : notes)
            played.add (object ({ { "note", n.note },
                                  { "start", n.start },
                                  { "length", n.length },
                                  { "velocity", (double)n.velocity } }));
        auto item = object ({ { "id", layer.id },
                              { "preset", c.preset },
                              { "notes", played },
                              { "tail", 0.5 },
                              { "output", folder.getChildFile (layer.id + ".wav").getFullPathName() } });
        if (chosen->tuned)
        {
            auto* values = new juce::DynamicObject();
            for (const auto& [id, value] : chosen->tuned->settings)
                values->setProperty (id, value);
            item.getDynamicObject()->setProperty ("settings", juce::var (values));
        }
        auto& m = byInstrument[c.source.key];
        m.source = c.source;
        m.items.add (item);
    }
    if (byInstrument.empty())
        return nothing();
    busy = true;
    status.setText ("Written. Setting each part's level: playing it with its sound...", juce::dontSendNotification);
    updateButtons();
    auto measured = std::make_shared<std::map<juce::String, double>>();
    auto problems = std::make_shared<std::map<juce::String, juce::String>>(); // layer id -> why not measured
    task = std::make_unique<BackgroundTask> (
        "Spinus song levels",
        [helper = context.helper, byInstrument, measured, problems] (const std::function<bool()>& shouldStop)
        {
            for (const auto& [key, m] : byInstrument)
            {
                if (shouldStop())
                    break;
                juce::String problem;
                const auto done = parts::renderInHelper (helper, m.source, m.items, shouldStop, problem);
                for (const auto& item : m.items)
                {
                    const auto id = item["id"].toString();
                    const auto* result = std::find_if (done.begin(), done.end(),
                                                       [&] (const juce::var& d) { return d["id"].toString() == id; });
                    if (result == done.end() || result->hasProperty ("problem"))
                        (*problems)[id] = result != done.end()   ? (*result)["problem"].toString()
                                          : problem.isNotEmpty() ? problem
                                                                 : juce::String ("it did not play");
                    else
                        (*measured)[id] = rmsDb (juce::File ((*result)["output"].toString()));
                }
            }
            return juce::Result::ok();
        },
        [this, a = alive, measured, problems, then] (const juce::Result&)
        {
            if (! *a)
                return;
            juce::MessageManager::callAsync (
                [this, a, measured, problems, then]
                {
                    if (! *a)
                        return;
                    task = nullptr;
                    busy = false;
                    const auto layers = song::layersOf (song);
                    faders = song::startingFaders (layers, *measured);
                    juce::StringArray set, unmeasured;
                    for (const auto& layer : layers)
                        if (const auto f = faders.find (layer.id); f != faders.end())
                            set.add (layer.name + " " + juce::String (f->second, 1) + " dB");
                        else if (const auto p = problems->find (layer.id); p != problems->end())
                            unmeasured.add (layer.name + " (" + p->second + ")");
                        else if (measured->contains (layer.id))
                            unmeasured.add (layer.name + " (it played nothing: its sounds may load late)");
                    status.setText (
                        "Written. Apply puts it in the project to hear (Ctrl+Z takes it away). " +
                            (set.isEmpty() ? juce::String()
                                           : "Each part starts at its role's level, from how loud its sound "
                                             "played: " +
                                                 set.joinIntoString (", ") + ".") +
                            (unmeasured.isEmpty()
                                 ? juce::String()
                                 : " Not measured, left as they are: " + unmeasured.joinIntoString (", ") + "."),
                        juce::dontSendNotification);
                    updateButtons();
                    if (then)
                        then (*measured, *problems);
                });
        });
}

bool SongWriterPanel::redo (const juce::String& part, const juce::String& section)
{
    if (busy || ! song.isObject() || part.isEmpty() == section.isEmpty())
        return false;
    context = getContext();
    if (! aiReady())
    {
        status.setText ("Writing needs Spinus's AI service: turn AI on in Options > AI.", juce::dontSendNotification);
        return false;
    }
    const auto seed = fixedSeed >= 0 ? fixedSeed++ : juce::Random::getSystemRandom().nextInt (0x7fffffff);
    auto redoWhat = part.isNotEmpty() ? object ({ { "part", part } }) : object ({ { "section", section } });
    juce::String partName = song::displayName (part);
    for (const auto& layer : song::layersOf (song))
        if (layer.id == part)
            partName = layer.name;
    const auto what = part.isNotEmpty() ? "the " + partName.toLowerCase() : "the " + section;
    run (object ({ { "summary", context.summary }, { "seed", seed }, { "song", song }, { "redo", redoWhat } }),
         "Writing " + what + " again...",
         [this, part, section, what] (const juce::var& result)
         {
             song = result;
             // The song in the project gets only what was written again; a song not applied yet stays whole.
             if (! whole && applied.isIn (session.getProject()))
             {
                 if (part.isNotEmpty())
                     redoneParts.addIfNotAlreadyThere (part);
                 else
                     redoneSections.addIfNotAlreadyThere (section);
             }
             else
                 whole = true;
             show();
             status.setText ("Wrote " + what + " again; the rest is as it was. Apply to use it.",
                             juce::dontSendNotification);
         });
    return true;
}

song::Options SongWriterPanel::optionsFor (const model::Project& project, const juce::String& replace)
{
    song::Options options;
    options.channels =
        song::isFull (song) ? song::findLayerChannels (project, song::layersOf (song)) : song::findChannels (project);
    options.replace = replace;
    options.name = replace.isNotEmpty() ? replace : song::nextSongName (project);
    for (const auto& layer : song::layersOf (song))
        if (const auto chosen = chosenForLayer (layer))
            if (auto sound = soundOf (chosen->choice))
            {
                // Its fader: toward the songs when Effects and mix set one, else the starting balance.
                if (std::abs (chosen->volumeDb) >= 0.05)
                    sound->volumeDb = chosen->volumeDb;
                else if (const auto f = faders.find (layer.id); f != faders.end())
                    sound->volumeDb = f->second;
                options.layerSounds[layer.id] = *sound;
            }
    for (const auto& part : song::partNames())
        if (const auto* chosen = chosenFor (context.sounds, part))
            if (auto sound = soundOf (chosen->choice))
            {
                if (! options.channels.contains (part))
                    options.sounds[part] = *sound;
                else if (part == "drums" && ! sound->drums.empty())
                    options.drums = sound->drums; // the kit the Sounds page added
            }
    return options;
}

juce::var SongWriterPanel::makeProposal (juce::String* error) { return makeProposal (error, {}); }

juce::var SongWriterPanel::makeProposal (juce::String* error, const juce::String& replace)
{
    auto fail = [error] (const juce::String& why)
    {
        if (error != nullptr)
            *error = why;
        return juce::var();
    };
    if (! song.isObject())
        return fail ("Write a song first.");
    const auto& project = session.getProject();
    if (whole || ! applied.isIn (project))
        return song::makeProposal (song, project, optionsFor (project, replace), error);
    if (redoneParts.isEmpty() && redoneSections.isEmpty())
        return fail ("The song is in the project as it is: write another version or redo a part first.");
    juce::Array<juce::var> steps;
    juce::StringArray summaries;
    for (const auto& part : redoneParts)
    {
        const auto made = song::makeRedoProposal (song, applied, part, {}, error);
        if (! made.isObject())
            return {};
        steps.addArray (*made["steps"].getArray());
        summaries.add (made["summary"].toString());
    }
    for (const auto& section : redoneSections)
    {
        const auto made = song::makeRedoProposal (song, applied, {}, section, error);
        if (! made.isObject())
            return {};
        steps.addArray (*made["steps"].getArray());
        summaries.add (made["summary"].toString());
    }
    return object ({ { "summary", summaries.joinIntoString (", ") }, { "steps", steps } });
}

bool SongWriterPanel::apply()
{
    if (busy)
        return false;
    context = getContext();
    // Writing again replaces the song written before, once the user says so; or both stay.
    const auto& project = session.getProject();
    const auto previous = song::lastSongName (project);
    if (song.isObject() && (whole || ! applied.isIn (project)) && previous.isNotEmpty())
    {
        messageBox (juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::QuestionIcon)
                        .withTitle ("Replace " + previous + "?")
                        .withMessage ("Replace " + previous + "? It will be overwritten (Ctrl+Z brings it back).")
                        .withButton ("Replace")
                        .withButton ("Keep both")
                        .withButton ("Cancel"),
                    [safe = juce::Component::SafePointer (this), previous] (int choice)
                    {
                        if (safe != nullptr && choice == 0)
                            safe->applyNow (previous);
                        else if (safe != nullptr && choice == 1)
                            safe->applyNow ({});
                    });
        return true;
    }
    return applyNow ({});
}

bool SongWriterPanel::applyNow (const juce::String& replace)
{
    if (busy)
        return false;
    juce::String error;
    const auto json = makeProposal (&error, replace);
    if (! json.isObject())
    {
        status.setText (error, juce::dontSendNotification);
        return false;
    }
    auto& project = session.getProject();
    const bool wholeSong = whole || ! applied.isIn (project);
    const auto options = optionsFor (project, replace);
    auto proposal = ai::Proposal::prepare (json, session.getHistory());
    proposal.undoName = "Track creator: " + proposal.summary;
    if (const auto result = proposal.apply (session.getHistory()); result.failed())
    {
        status.setText ("The song could not be applied: " + result.getErrorMessage(), juce::dontSendNotification);
        return false;
    }
    juce::String loaded;
    if (wholeSong)
    {
        applied = song::findApplied (song, session.getProject(), options.name);
        // The sounds chosen on the Sounds page (or, for a full arrangement's other layers, from the sound
        // index), into the channels the song made for them (as the Sounds page loads them: a plugin's own presets,
        // samples onto pads or keys, tuned settings on top).
        parts::Plan chosen;
        std::vector<model::ChannelId> channels;
        if (applied.full)
        {
            for (const auto& layer : song::layersOf (song))
                if (options.layerSounds.contains (layer.id) && applied.layerChannels.contains (layer.id))
                    if (const auto p = chosenForLayer (layer))
                    {
                        chosen.parts.push_back (*p);
                        channels.push_back (applied.layerChannels.at (layer.id));
                    }
        }
        else
            for (const auto& [part, sound] : options.sounds)
                if (const auto* p = chosenFor (context.sounds, part); p != nullptr && applied.channels.contains (part))
                {
                    chosen.parts.push_back (*p);
                    channels.push_back (applied.channels.at (part));
                }
        if (! chosen.parts.empty())
            if (const auto r = parts::loadSounds (session, chosen, channels); r.failed())
                loaded = " Not every chosen sound loaded: " + r.getErrorMessage();
    }
    whole = false;
    redoneParts.clear();
    redoneSections.clear();
    show();
    status.setText ("Applied " + proposal.summary +
                        " as one undo step (Edit > Undo takes it away). Mix it... "
                        "balances the parts toward the songs." +
                        loaded,
                    juce::dontSendNotification);
    updateButtons();
    return true;
}

void SongWriterPanel::stop()
{
    if (task != nullptr)
    {
        task = nullptr; // stops the measuring and waits for it
        busy = false;
    }
    if (context.service != nullptr && job.isNotEmpty())
        context.service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
    session.getSamplePreview().stop();
    status.setText (composing ? "Cancelled: the song shown before stays." : "Stopped.", juce::dontSendNotification);
    composing = false;
    updateButtons();
}

//==============================================================================
// The AI writer

void SongWriterPanel::setAiWriter (bool on)
{
    writerBox.setSelectedId (on ? 2 : 1, juce::dontSendNotification);
    updateButtons();
}

bool SongWriterPanel::isAiWriter() const { return writerBox.getSelectedId() == 2 && aiAvailable; }

void SongWriterPanel::setEffort (const juce::String& effort)
{
    const juce::StringArray levels { "", "low", "medium", "high" };
    effortBox.setSelectedId (juce::jmax (0, levels.indexOf (effort)) + 1, juce::dontSendNotification);
}

juce::String SongWriterPanel::getEffort() const
{
    const juce::StringArray levels { "", "low", "medium", "high" };
    return levels[juce::jlimit (0, 3, effortBox.getSelectedId() - 1)];
}

void SongWriterPanel::setRounds (int rounds)
{
    roundsBox.setSelectedId (juce::jlimit (0, 3, rounds) + 1, juce::dontSendNotification);
}

juce::var SongWriterPanel::candidatesFor (juce::var& current)
{
    candidateChoices.clear();
    auto* candidates = new juce::DynamicObject();
    auto* now = new juce::DynamicObject();
    current = juce::var (now);
    SoundIndex index (context.indexFolder);
    if (context.indexFolder != juce::File())
        index.load();
    std::map<juce::String, int> instances;
    for (const auto& p : context.parts)
    {
        const auto instance = instances[p.role]++;
        if (const auto* chosen = [&]() -> const parts::Plan::Part*
            {
                for (const auto& page : context.sounds.parts)
                    if (page.target.id == p.id)
                        return &page;
                return nullptr;
            }())
        {
            now->setProperty (p.id, chosen->choice.title()); // the user's choice: the AI only hears of it
            continue;
        }
        if (index.instruments.empty())
            continue;
        const auto target = parts::targetFor (p, instance, context.summary);
        juce::Array<juce::var> list;
        for (const auto& c : parts::choose (target, {}, index, 8))
        {
            if (c.kind != parts::Choice::Kind::preset)
                continue;
            const auto id = c.instrument + "|" + c.preset;
            candidateChoices[p.id][id] = c;
            list.add (object ({ { "id", id }, { "name", c.title() } }));
        }
        if (! list.isEmpty())
            candidates->setProperty (p.id, list);
    }
    return juce::var (candidates);
}

juce::var SongWriterPanel::composeParams (int seed)
{
    juce::Array<juce::var> list;
    for (const auto& p : context.parts)
        list.add (object ({ { "id", p.id }, { "name", p.name }, { "role", p.role } }));
    juce::var current;
    const auto candidates = candidatesFor (current);
    auto params = object ({ { "summary", context.summary },
                            { "seed", seed },
                            { "candidates", candidates },
                            { "current", current },
                            { "effort", getEffort() },
                            { "rounds", getRounds() },
                            { "way", "harness" } });
    if (! list.isEmpty())
        params.getDynamicObject()->setProperty ("parts", list);
    return params;
}

void SongWriterPanel::composeRound (const juce::var& params, const juce::String& words)
{
    run (
        params, words,
        [this] (const juce::var& result)
        {
            song = result;
            layerChoicesKnown = false;
            whole = true;
            redoneParts.clear();
            redoneSections.clear();
            show();
            if (! composing)
                return;
            if (! song["composed"].isObject())
            {
                // The AI was not used after all (no connection, it failed): the rules' song, and why.
                composing = false;
                status.setText (song["note"].toString(), juce::dontSendNotification);
                balance();
                return;
            }
            balance ([this] (const auto& levels, const auto& problems) { afterMeasured (levels, problems); });
        },
        "ai.v1.song.compose");
}

void SongWriterPanel::afterMeasured (const std::map<juce::String, double>& levels,
                                     const std::map<juce::String, juce::String>& problems)
{
    if (! composing)
        return;
    if (roundsLeft <= 0 || stopRequested || ! song["composed"].isObject() || levels.empty())
        return finishComposing();
    --roundsLeft;
    auto params = composeRequest.clone();
    auto* heard = new juce::DynamicObject();
    for (const auto& [id, db] : levels)
        heard->setProperty (id, db);
    auto* why = new juce::DynamicObject();
    for (const auto& [id, problem] : problems)
        why->setProperty (id, problem);
    params.getDynamicObject()->setProperty ("song", song);
    params.getDynamicObject()->setProperty ("levels", juce::var (heard));
    params.getDynamicObject()->setProperty ("problems", juce::var (why));
    composeRound (params, "Revising with " + connectionName + " (what Spinus heard)...");
}

void SongWriterPanel::finishComposing()
{
    composing = false;
    status.setText (song["note"].toString() + " Apply puts it in the project to hear (Ctrl+Z takes it away).",
                    juce::dontSendNotification);
    updateButtons();
    refreshEstimate(); // from this run's real numbers now
}

bool SongWriterPanel::stopNow()
{
    if (! composing)
        return false;
    stopRequested = true;
    if (context.service != nullptr && job.isNotEmpty())
        context.service->call ("ai.v1.song.composeStop", object ({ { "job", job } }), {});
    status.setText ("Stopping after the AI's current step; its best checked version is kept...",
                    juce::dontSendNotification);
    return true;
}

namespace
{
juce::String thousands (double tokens)
{
    if (tokens < 1000.0)
        return juce::String (juce::roundToInt (tokens));
    return juce::String (tokens < 10000.0 ? juce::roundToInt (tokens / 100.0) / 10.0
                                          : juce::roundToInt (tokens / 1000.0)) +
           " thousand";
}
} // namespace

void SongWriterPanel::refreshEstimate()
{
    if (busy)
        return;
    context = getContext();
    if (context.service == nullptr || ! context.service->isEnabled() || ! context.summary.isObject())
    {
        aiAvailable = false;
        writerBox.changeItemText (2, "Your AI connection (turn AI on in Options > AI)");
        writerBox.setItemEnabled (2, false);
        updateButtons();
        return;
    }
    auto params = composeParams (fixedSeed >= 0 ? fixedSeed : 1);
    context.service->call (
        "ai.v1.song.composeEstimate", params,
        [this, a = alive] (const juce::var& result, const juce::String& error)
        {
            if (! *a)
                return;
            aiAvailable = error.isEmpty() && (bool)result["available"];
            connectionName = result["connection"].toString();
            writerBox.changeItemText (2, aiAvailable ? "Your AI connection (" + connectionName + ")"
                                                     : "Your AI connection (none set up: Options > AI)");
            writerBox.setItemEnabled (2, aiAvailable);
            if (aiAvailable)
            {
                const auto runs = (int)result["fromRuns"];
                estimateLabel.setText (
                    "Sends the style, your parts and their sounds' names to " + connectionName + " (" +
                        result["model"].toString() + "). About " + thousands ((double)result["low"]) + " to " +
                        thousands ((double)result["high"]) + " tokens for the song" +
                        (runs > 0
                             ? " (from your last " + juce::String (runs) + " run" + (runs > 1 ? "s" : "") + " like it)"
                             : " (a rough range until you have run it: the reasoning is not known before)") +
                        "; the first request's " + thousands ((double)result["input"]) +
                        ((bool)result["inputCounted"] ? " input tokens counted by your provider."
                                                      : " input tokens, about.") +
                        ((bool)result["cached"]
                             ? " Most of the input is read from your provider's cache, at a lower price."
                             : "") +
                        " Higher reasoning can use several times more.",
                    juce::dontSendNotification);
            }
            else
                estimateLabel.setText (result["reason"].toString(), juce::dontSendNotification);
            updateButtons();
        });
}

bool SongWriterPanel::mix()
{
    if (! applied.isIn (session.getProject()))
    {
        status.setText ("Apply the song first: the automixer balances what is in the project.",
                        juce::dontSendNotification);
        return false;
    }
    context = getContext();
    if (onMix)
        onMix (context.songs.isEmpty() ? juce::File() : context.songs.getFirst());
    status.setText ("The automixer measures every part and suggests changes toward the reference song; tick what "
                    "you want and apply them (one undo step).",
                    juce::dontSendNotification);
    return true;
}

void SongWriterPanel::showRedoMenu()
{
    if (! song.isObject())
        return;
    juce::PopupMenu menu;
    menu.addSectionHeader ("A part, in every section");
    const auto layers = song::layersOf (song);
    if (layers.empty())
        for (const auto& part : song::partNames())
            menu.addItem ("New " + song::displayName (part).toLowerCase(), [this, part] { redo (part); });
    for (const auto& layer : layers)
        if (layer.part != "fx")
            menu.addItem ("New " + layer.name.toLowerCase(), [this, id = layer.id] { redo (id); });
    menu.addSectionHeader ("A section, every part (and its chords)");
    if (const auto* sections = song["sections"].getArray())
        for (const auto& s : *sections)
            menu.addItem ("The " + s["name"].toString().toLowerCase() + " again",
                          [this, id = s["id"].toString()] { redo ({}, id); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&redoButton));
}

void SongWriterPanel::show()
{
    juce::StringArray lines;
    if (const auto& described = context.summary["described"]; described.isObject())
        lines.add ("From your description. " + described["understood"].toString() + ".");
    lines.add (song::describe (song));
    const auto& checks = song["checks"];
    if (checks.isObject())
    {
        const auto& chords = checks["chords"];
        // Reference songs, or only a description's style (its genre's numbers): said as what it is.
        const bool songs = (int)context.summary["songs"] > 0;
        const juce::String theirs = songs ? "the songs'" : "the style's";
        lines.add ({});
        lines.add (songs ? "Compared with the songs:" : "Compared with the style of your description:");
        lines.add ("- tempo " + checks["tempo"]["song"].toString() + " BPM (" + (songs ? "theirs " : "the style's ") +
                   checks["tempo"]["range"][0].toString() + " to " + checks["tempo"]["range"][1].toString() +
                   ", typically " + checks["tempo"]["style"].toString() + ")");
        lines.add ("- chords: " + juce::String (juce::roundToInt (100.0 * (double)chords["inVocabulary"])) +
                   " % from " + theirs + " own chords" +
                   (songs ? "; " + juce::String ((int)chords["loopsOfOneReference"]) +
                                " chord loop(s) as one of the songs plays it, the others new"
                          : juce::String()));
        juce::StringArray groove, reference;
        if (auto* o = checks["groove"].getDynamicObject())
            for (const auto& p : o->getProperties())
                groove.add (p.name.toString() + " " + juce::String (juce::roundToInt (100.0 * (double)p.value)) + " %");
        if (auto* o = checks["referenceGroove"].getDynamicObject())
            for (const auto& p : o->getProperties())
                reference.add (p.name.toString() + " " + juce::String (juce::roundToInt (100.0 * (double)p.value)) +
                               " %");
        lines.add ("- drums alike " + theirs + " groove: " + groove.joinIntoString (", ") +
                   (reference.isEmpty()
                        ? juce::String()
                        : " (each song alike its own average: " + reference.joinIntoString (", ") + ")"));
        lines.add ("- notes in the key: " + juce::String (juce::roundToInt (100.0 * (double)checks["notesInKey"])) +
                   " %; fitting the chords: " +
                   juce::String (juce::roundToInt (100.0 * (double)checks["notesFitChords"])) + " %");
    }
    if (const auto& composed = song["composed"]; composed.isObject())
    {
        // What the AI used, as the provider reported it, per round and in all.
        std::map<int, std::array<juce::int64, 5>> rounds; // requests, input, cached, output, reasoning
        if (const auto* requests = composed["usage"]["requests"].getArray())
            for (const auto& r : *requests)
            {
                auto& t = rounds[(int)r["round"]];
                t[0] += 1;
                t[1] += (juce::int64)r["input"];
                t[2] += (juce::int64)r["cached"];
                t[3] += (juce::int64)r["output"];
                t[4] += (juce::int64)r["reasoning"];
            }
        lines.add ({});
        lines.add ("Tokens used (as " + connectionName + " reported them):");
        std::array<juce::int64, 5> all {};
        for (const auto& [round, t] : rounds)
        {
            for (size_t i = 0; i < all.size(); ++i)
                all[i] += t[i];
            lines.add ("- " + (round == 0 ? juce::String ("writing") : "revision " + juce::String (round)) + ": " +
                       juce::String (t[0]) + " requests, " + juce::String (t[1]) + " in (" + juce::String (t[2]) +
                       " from the cache), " + juce::String (t[3]) + " out (" + juce::String (t[4]) + " reasoning)");
        }
        lines.add ("- in all: " + juce::String (all[1]) + " in, " + juce::String (all[3]) + " out");
        if (const auto* findings = composed["findings"].getArray(); findings != nullptr && ! findings->isEmpty())
        {
            lines.add ("Spinus's checks still find: ");
            for (int i = 0; i < juce::jmin (6, findings->size()); ++i)
                lines.add ("- " + (*findings)[i].toString());
        }
    }
    lines.add ({});
    const auto& project = session.getProject();
    if ((whole || ! applied.isIn (project)) && song::isFull (song))
    {
        // Each part's sound: chosen in step 3, else from the sound index, else Spinus's own; said only for the
        // parts it applies to.
        juce::StringArray fromIndex, own, families;
        const auto& choices = getLayerChoices();
        for (const auto& layer : song::layersOf (song))
        {
            families.addIfNotAlreadyThere (layer.family);
            if (pagePartFor (layer) != nullptr)
                continue;
            if (const auto found = choices.find (layer.id); found != choices.end())
                fromIndex.add (layer.name + ": " + found->second.title());
            else
                own.add (layer.name);
        }
        const auto previous = song::lastSongName (project);
        lines.add (
            (previous.isNotEmpty() ? "Apply asks whether to replace " + previous + " (or keep both), and adds it"
                                   : "Apply adds it as " + song::nextSongName (project)) +
            ": a channel per part, coloured and grouped by family (" + families.joinIntoString (", ") +
            ") with a bus each in the mixer, a pattern per part and section, a Playlist track per part under "
            "its family's, markers, the tempo and key. The sounds chosen in step 3, Parts, are used." +
            (fromIndex.isEmpty() ? juce::String()
                                 : " From your sound index: " + fromIndex.joinIntoString ("; ") + ".") +
            (own.isEmpty() ? juce::String() : " Spinus's own instruments for: " + own.joinIntoString (", ") + "."));
    }
    else if (whole || ! applied.isIn (project))
        lines.add ("Apply adds it as " + song::nextSongName (project) +
                   ": a pattern per section, a Playlist track, markers, the tempo and key; parts without a channel "
                   "of the Parts step get Spinus's own instruments or the sounds chosen there.");
    else if (! redoneParts.isEmpty() || ! redoneSections.isEmpty())
        lines.add ("Apply writes what was written again into " + applied.name + "; the rest stays.");
    else
        lines.add (applied.name + " is in the project.");
    text.setText (lines.joinIntoString ("\n"), false);
}

void SongWriterPanel::updateButtons()
{
    const bool has = song.isObject();
    writeButton.setEnabled (! busy);
    againButton.setEnabled (! busy && has);
    redoButton.setEnabled (! busy && has);
    applyButton.setEnabled (! busy && has);
    mixButton.setEnabled (! busy && applied.isIn (session.getProject()));
    refineButton.setEnabled (! busy);
    const bool ai = writerBox.getSelectedId() == 2;
    refineButton.setVisible (! ai);
    writerBox.setEnabled (! busy);
    effortBox.setVisible (ai);
    roundsBox.setVisible (ai);
    estimateLabel.setVisible (ai);
    effortBox.setEnabled (! busy);
    roundsBox.setEnabled (! busy);
    stopNowButton.setVisible (composing && job.isNotEmpty());
    stopButton.setButtonText (composing ? "Cancel" : "Stop");
    stopButton.setVisible (busy || session.getSamplePreview().isPlaying());
    resized();
}

void SongWriterPanel::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (36));
    layoutAiRow (area.removeFromTop (26));
    if (refineButton.isVisible())
        refineButton.setBounds (area.removeFromTop (26).removeFromLeft (420));
    if (estimateLabel.isVisible())
        estimateLabel.setBounds (area.removeFromTop (34));
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (28);
        for (auto* b : { &writeButton, &againButton, &redoButton })
        {
            b->setBounds (r.removeFromLeft (b == &againButton ? 130 : b == &redoButton ? 110 : 80).reduced (0, 1));
            r.removeFromLeft (6);
        }
        if (stopButton.isVisible())
            stopButton.setBounds (r.removeFromLeft (64).reduced (0, 1));
        if (stopNowButton.isVisible())
        {
            r.removeFromLeft (6);
            stopNowButton.setBounds (r.removeFromLeft (170).reduced (0, 1));
        }
        mixButton.setBounds (r.removeFromRight (80).reduced (0, 1));
        r.removeFromRight (6);
        applyButton.setBounds (r.removeFromRight (80).reduced (0, 1));
    }
    area.removeFromTop (4);
    status.setBounds (area.removeFromBottom (54));
    text.setBounds (area);
}

void SongWriterPanel::layoutAiRow (juce::Rectangle<int> row)
{
    writerLabel.setBounds (row.removeFromLeft (130));
    // The writer, then (for the AI) its reasoning and fix rounds, sharing what is left of the row.
    const auto rest = row.getWidth() - 12;
    writerBox.setBounds (row.removeFromLeft (juce::jmin (300, rest * 2 / 5)).reduced (0, 1));
    row.removeFromLeft (6);
    effortBox.setBounds (row.removeFromLeft (juce::jmin (200, rest * 27 / 100)).reduced (0, 1));
    row.removeFromLeft (6);
    roundsBox.setBounds (row.removeFromLeft (juce::jmin (240, row.getWidth())).reduced (0, 1));
}

void SongWriterPanel::paint (juce::Graphics&) {}

void SongWriterPanel::visibilityChanged()
{
    if (isVisible())
        refreshEstimate();
}

} // namespace spinus::ui

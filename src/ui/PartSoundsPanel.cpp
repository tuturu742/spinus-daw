#include "PartSoundsPanel.h"

#include "ai/AnalysisJobs.h"
#include "ai/Proposal.h"
#include "dsp/sampler/SampleStorage.h"
#include "session/PresetPreviews.h"

namespace spinus::ui
{

namespace
{
constexpr const char* separationModel = "htdemucs"; // the Track creator's stems

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : items)
        o->setProperty (key, value);
    return juce::var (o);
}

juce::var notesVar (const std::vector<OfflineInstrument::Note>& notes)
{
    juce::Array<juce::var> list;
    for (const auto& n : notes)
        list.add (object (
            { { "note", n.note }, { "start", n.start }, { "length", n.length }, { "velocity", (double)n.velocity } }));
    return list;
}

/** A drum's Drum Machine pad (the notes the Track creator's drum excerpts and phrases use). */
int padOf (const juce::String& drum) { return drum == "kick" ? 60 : drum == "snare" ? 61 : 63; }
} // namespace

//==============================================================================
/** One part: its name, the chosen sound (a choice of the alternatives), Listen, The songs', and the reasons. */
class PartSoundsPanel::RowView final : public juce::Component
{
public:
    RowView (PartSoundsPanel& p, int i) : panel (p), index (i)
    {
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        why.setFont (juce::FontOptions (13.0f));
        why.setJustificationType (juce::Justification::topLeft);
        why.setMinimumHorizontalScale (1.0f);
        why.setColour (juce::Label::textColourId, findColour (juce::Label::textColourId).withAlpha (0.75f));
        choice.onChange = [this] { panel.select (index, choice.getSelectedId() - 1); };
        choice.setTooltip ("The closest sounds for this part, the closest first: pick another to swap it");
        listenButton.setTooltip ("Hear the chosen sound play a short phrase");
        listenButton.onClick = [this] { panel.listen (index); };
        songsButton.setTooltip ("Hear the part in the reference song it was compared with");
        songsButton.onClick = [this] { panel.listenToSongs (index); };
        for (auto* c : std::initializer_list<juce::Component*> { &title, &choice, &listenButton, &songsButton, &why })
            addAndMakeVisible (c);
    }

    void update (const Row& row)
    {
        title.setText (parts::displayName (row.target.part), juce::dontSendNotification);
        choice.clear (juce::dontSendNotification);
        for (size_t i = 0; i < row.choices.size(); ++i)
            choice.addItem (juce::String ((int)i + 1) + ". " + row.choices[i].title(), (int)i + 1);
        if (! row.choices.empty())
            choice.setSelectedId (row.selected + 1, juce::dontSendNotification);
        choice.setTextWhenNothingSelected ("No sound in the index fits");
        why.setText (panel.describe (index).fromFirstOccurrenceOf ("\n", false, false), juce::dontSendNotification);
        listenButton.setEnabled (! row.choices.empty());
        songsButton.setEnabled (row.stem != juce::File());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (2);
        auto top = r.removeFromTop (26);
        title.setBounds (top.removeFromLeft (170));
        songsButton.setBounds (top.removeFromRight (90).reduced (0, 1));
        top.removeFromRight (4);
        listenButton.setBounds (top.removeFromRight (70).reduced (0, 1));
        top.removeFromRight (6);
        choice.setBounds (top.reduced (0, 1));
        why.setBounds (r);
    }

    juce::ComboBox choice;

private:
    PartSoundsPanel& panel;
    int index;
    juce::Label title, why;
    juce::TextButton listenButton { "Listen" }, songsButton { "The songs'" };
};

/** What the measuring thread is given and finds. */
struct PartSoundsPanel::Measuring
{
    Context context;
    std::vector<Row> rows;
    juce::String problem;
};

//==============================================================================
PartSoundsPanel::PartSoundsPanel (Session& s, std::function<Context()> c) : session (s), getContext (std::move (c))
{
    intro.setText ("For each part of the songs, the closest sounds you have (from the sound index), with why. "
                   "Experimental: a starting point to change by ear.",
                   juce::dontSendNotification);
    intro.setMinimumHorizontalScale (1.0f);
    intro.setJustificationType (juce::Justification::topLeft);
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (1.0f);
    closerButton.setTooltip ("Compare the closest presets with the songs' stems by listening, with closest preset's "
                             "CLAP model (AI on, the analysis engine and its model files)");
    tuneButton.setTooltip ("Tune each chosen synth toward the song's part with synth match, in a separate program "
                           "(a minute or more per part; Stop keeps what was found)");
    effectsButton.setTooltip ("Set an EQ, a compressor and the level of each part toward the songs, and the "
                              "master's loudness (measured by the AI service; no AI connection needed)");
    closerButton.setToggleState (true, juce::dontSendNotification);
    effectsButton.setToggleState (true, juce::dontSendNotification);
    chooseButton.setTooltip ("Measure the songs' parts and find the closest sounds of the sound index");
    chooseButton.onClick = [this] { chooseSounds(); };
    stopButton.onClick = [this] { stop(); };
    effectsAgainButton.setTooltip ("Set the effects and the mix again for the sounds chosen now");
    effectsAgainButton.onClick = [this] { setEffectsAgain(); };
    addButton.setTooltip ("Make a channel with its sound and effects for each part, and a pattern with a short "
                          "phrase on each: one undo step");
    addButton.onClick = [this] { addToProject(); };
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    for (auto* c :
         std::initializer_list<juce::Component*> { &intro, &closerButton, &tuneButton, &effectsButton, &chooseButton,
                                                   &effectsAgainButton, &addButton, &viewport, &status })
        addAndMakeVisible (c);
    addChildComponent (stopButton);
    updateButtons();
}

PartSoundsPanel::~PartSoundsPanel()
{
    *alive = false;
    *stopping = true;
    task = nullptr; // waits for it (the helper is killed by the stop)
    if (context.service != nullptr && job.isNotEmpty())
        context.service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

juce::File PartSoundsPanel::work (const juce::String& name) const
{
    const auto folder =
        context.workFolder != juce::File()
            ? context.workFolder
            : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-part-sounds");
    folder.createDirectory();
    return folder.getChildFile (name);
}

juce::File PartSoundsPanel::stemWav (const Row& row) const
{
    return work ("stems/" + row.songDigest.substring (0, 12) + "-" + row.target.part + ".wav");
}

bool PartSoundsPanel::aiReady() const { return context.service != nullptr && context.service->isEnabled(); }

void PartSoundsPanel::runInBackground (const juce::String& words,
                                       std::function<juce::String (const std::function<bool()>&)> what,
                                       std::function<void (const juce::String&)> then)
{
    status.setText (words, juce::dontSendNotification);
    auto problem = std::make_shared<juce::String>();
    auto token = stopping;
    task = std::make_unique<BackgroundTask> (
        "Spinus sounds per part",
        [what = std::move (what), problem, token] (const std::function<bool()>& shouldStop)
        {
            *problem = what ([&] { return shouldStop() || token->load(); });
            return juce::Result::ok();
        },
        [this, a = alive, problem, then = std::move (then)] (const juce::Result&)
        {
            if (! *a)
                return;
            // The task is done; it is let go once this call has returned.
            juce::MessageManager::callAsync (
                [this, a, problem, then]
                {
                    if (! *a)
                        return;
                    task = nullptr;
                    then (*problem);
                });
        });
}

//==============================================================================
bool PartSoundsPanel::chooseSounds()
{
    if (busy)
        return false;
    context = getContext ? getContext() : Context {};
    if (! context.summary.isObject())
    {
        status.setText ("First analyze some songs above: the sounds are chosen for the parts of their summary.",
                        juce::dontSendNotification);
        return false;
    }
    busy = true;
    *stopping = false;
    notes.clear();
    updateButtons();
    auto state = std::make_shared<Measuring>();
    state->context = context;
    runInBackground (
        "Measuring the songs' parts and choosing sounds...",
        [state] (const std::function<bool()>& shouldStop)
        {
            SoundIndex index (state->context.indexFolder);
            index.load();
            if (index.instruments.empty() && index.samples.empty())
                return juce::String ("The sound index is empty: click Update sound index first.");
            for (const auto& target : parts::targetsFromSummary (state->context.summary))
            {
                Row row;
                row.target = target;
                parts::Reference reference;
                size_t most = 0;
                for (const auto& song : state->context.songs)
                {
                    if (shouldStop())
                        return juce::String ("Stopped.");
                    const auto entry = state->context.library.read (song.digest);
                    const auto stems = state->context.library.findStems (song.digest, separationModel);
                    const auto stem = stems.find (target.part);
                    if (! entry.isObject() || stem == stems.end())
                        continue;
                    std::vector<HiddenInstrument::Note> played;
                    if (target.part == "drums")
                    {
                        const auto hits = parts::stemHitsFromVar (entry["drums"][separationModel]);
                        if (hits.empty())
                            continue;
                        const auto audio = parts::readMono (stem->second, OfflineInstrument::sampleRate);
                        for (auto& [drum, m] : parts::measureHits (audio.getReadPointer (0), audio.getNumSamples(),
                                                                   OfflineInstrument::sampleRate, hits))
                        {
                            reference.hits[drum].add (m);
                            row.measured += m.count();
                        }
                        for (const auto& h : hits)
                            if (const auto drum = parts::drumOf (h.kind); drum.isNotEmpty())
                                played.push_back ({ padOf (drum), h.time, 0.1, 0.8f });
                    }
                    else
                    {
                        const auto stemNotes = parts::stemNotesFromVar (
                            entry["notes"][juce::Identifier (juce::String (separationModel) + "/" + target.part)]);
                        if (stemNotes.empty())
                            continue;
                        const auto audio = parts::readMono (stem->second, OfflineInstrument::sampleRate);
                        reference.notes.add (parts::measureNotes (audio.getReadPointer (0), audio.getNumSamples(),
                                                                  OfflineInstrument::sampleRate, stemNotes));
                        for (const auto& n : stemNotes)
                            played.push_back ({ n.pitch, n.start, n.end - n.start, 0.8f });
                    }
                    // The song with the most of the part is the one to hear and to tune toward.
                    if (played.size() > most)
                    {
                        most = played.size();
                        row.songDigest = song.digest;
                        row.songName = song.name;
                        row.stem = stem->second;
                        row.excerpt = chooseExcerpt (played, 4.0);
                    }
                }
                if (target.part != "drums")
                    row.measured = reference.notes.count();
                row.choices = parts::choose (target, reference, index, 5);
                state->rows.push_back (std::move (row));
            }
            return juce::String();
        },
        [this, state] (const juce::String& problem)
        {
            if (problem.isNotEmpty())
                return fail (problem);
            measured (state);
        });
    return true;
}

void PartSoundsPanel::measured (const std::shared_ptr<Measuring>& state)
{
    rows = std::move (state->rows);
    updateRows();
    if (std::all_of (rows.begin(), rows.end(), [] (const Row& r) { return r.measured == 0; }))
        notes.add ("The songs' stems were not found in the analysis library (Saved analyses may have deleted "
                   "them): the sounds are chosen by role and register only.");
    if (! aiReady() && (closerButton.getToggleState() || effectsButton.getToggleState()))
        notes.add ("AI is off, so the sounds are chosen by their measurements alone, without closer listening, "
                   "effects or the mix: turn on AI in Options > AI for those.");
    listenCloser (0);
}

void PartSoundsPanel::listenCloser (size_t part)
{
    if (! closerButton.getToggleState() || ! aiReady() || part >= rows.size())
        return tuneNext (0);
    auto& row = rows[part];
    // The first five presets of one instrument (the closest's), compared by closest preset's CLAP model.
    std::vector<size_t> presets;
    const auto* first = row.chosen();
    for (size_t i = 0; i < row.choices.size(); ++i)
        if (row.choices[i].kind == parts::Choice::Kind::preset && first != nullptr &&
            first->kind == parts::Choice::Kind::preset && row.choices[i].instrument == first->instrument)
            presets.push_back (i);
    if (presets.size() < 2 || row.stem == juce::File())
        return listenCloser (part + 1);

    // Is the CLAP model there?
    ai::checkAnalysisEngine (
        *context.service, alive,
        [this, part, presets] (const ai::EngineCheck& engine)
        {
            bool clap = false;
            if (auto* models = engine.models["models"].getArray())
                for (auto& m : *models)
                    clap = clap || (m["id"].toString() == "clap_music" && (bool)m["downloaded"]);
            if (engine.error.isNotEmpty() || ! engine.installed || ! clap)
            {
                notes.addIfNotAlreadyThere ("Listening closer needs the analysis engine and the CLAP model (the "
                                            "Analyzer's closest preset downloads it); the measurements alone chose.");
                return tuneNext (0);
            }
            auto& row = rows[part];
            const auto& source = row.choices[presets.front()].source;
            const auto library = row.choices[presets.front()].instrument;
            juce::Array<juce::var> items, previews;
            std::vector<PresetPreviews::PhraseNote> phrase = PresetPreviews::getPhrase();
            std::vector<OfflineInstrument::Note> phraseNotes;
            phraseNotes.reserve (phrase.size());
            for (const auto& n : phrase)
                phraseNotes.push_back ({ n.note, n.start, n.length, 0.8f });
            for (auto i : presets)
            {
                const auto& c = row.choices[i];
                const auto file = work ("previews/" + row.target.part + "-" + juce::String ((int)i) + ".wav");
                items.add (object ({ { "id", c.preset },
                                     { "preset", c.preset },
                                     { "notes", notesVar (phraseNotes) },
                                     { "tail", 0.5 },
                                     { "output", file.getFullPathName() } }));
                previews.add (
                    object ({ { "id", c.preset }, { "version", c.version }, { "file", file.getFullPathName() } }));
            }
            const auto wav = stemWav (row);
            if (! wav.existsAsFile())
                items.add (object (
                    { { "id", "stem" },
                      { "cut",
                        object ({ { "file", row.stem.getFullPathName() }, { "from", 0.0 }, { "length", -1.0 } }) },
                      { "output", wav.getFullPathName() } }));
            auto options = context.helper;
            runInBackground (
                "Playing the " + parts::displayName (row.target.part).toLowerCase() +
                    "'s closest presets to listen closer...",
                [options, source, items] (const std::function<bool()>& shouldStop)
                {
                    juce::String problem;
                    parts::renderInHelper (options, source, items, shouldStop, problem);
                    return problem;
                },
                [this, part, presets, previews, library, wav] (const juce::String& problem)
                {
                    if (*stopping)
                        return fail ("Stopped.");
                    if (problem.isNotEmpty() || ! wav.existsAsFile())
                    {
                        rows[part].note = "Not compared by listening: " +
                                          (problem.isNotEmpty() ? problem : "the stem could not be read.");
                        return listenCloser (part + 1);
                    }
                    status.setText ("Listening to the " + parts::displayName (rows[part].target.part).toLowerCase() +
                                        "'s presets...",
                                    juce::dontSendNotification);
                    ai::runJob (
                        *context.service, "ai.v1.presets.add",
                        object ({ { "library", library }, { "previews", previews } }), alive,
                        [this] (const juce::String& started) { job = started; },
                        [this, part, presets, library, wav] (const juce::var&, const juce::String& error)
                        {
                            job.clear();
                            if (error.isNotEmpty() || *stopping)
                            {
                                rows[part].note = "Not compared by listening: " + error;
                                return *stopping ? fail ("Stopped.") : listenCloser (part + 1);
                            }
                            juce::Array<juce::var> only;
                            for (auto i : presets)
                                only.add (rows[part].choices[i].preset);
                            ai::runJob (
                                *context.service, "ai.v1.presets.match",
                                object ({ { "library", library },
                                          { "file", wav.getFullPathName() },
                                          { "top", 50 },
                                          { "only", only } }),
                                alive, [this] (const juce::String& started) { job = started; },
                                [this, part, presets] (const juce::var& found, const juce::String& matchError)
                                {
                                    job.clear();
                                    auto& r = rows[part];
                                    if (matchError.isNotEmpty())
                                        r.note = "Not compared by listening: " + matchError;
                                    else if (const auto* list = found["presets"].getArray())
                                    {
                                        // Ranked again: the measurements' order and the listening's count alike.
                                        std::map<juce::String, int> heard;
                                        for (int k = 0; k < list->size(); ++k)
                                            heard[(*list)[k]["id"].toString()] = k;
                                        std::vector<std::pair<double, size_t>> order;
                                        for (size_t k = 0; k < presets.size(); ++k)
                                        {
                                            auto& c = r.choices[presets[k]];
                                            const auto place =
                                                heard.contains (c.preset) ? heard[c.preset] : (int)presets.size();
                                            for (int m = 0; m < list->size(); ++m)
                                                if ((*list)[m]["id"].toString() == c.preset)
                                                    c.clap = (double)(*list)[m]["score"];
                                            order.emplace_back ((double)k + (double)place + 0.01 * (double)k,
                                                                presets[k]);
                                        }
                                        std::sort (order.begin(), order.end());
                                        std::vector<parts::Choice> reordered = r.choices;
                                        for (size_t k = 0; k < presets.size(); ++k)
                                            reordered[presets[k]] = r.choices[order[k].second];
                                        for (auto i : presets)
                                            if (reordered[i].clap > -2.0)
                                                reordered[i].why.add (
                                                    "listening says " +
                                                    juce::String (juce::roundToInt (reordered[i].clap * 100.0)) +
                                                    " % alike (CLAP)");
                                        r.choices = std::move (reordered);
                                        r.selected = 0;
                                        r.note = "Compared by listening too.";
                                    }
                                    updateRows();
                                    listenCloser (part + 1);
                                });
                        },
                        [this, part] (double done, double total, const juce::String&)
                        {
                            status.setText (
                                "Listening to the " + parts::displayName (rows[part].target.part).toLowerCase() +
                                    "'s presets: " +
                                    juce::String (juce::roundToInt (100.0 * done / juce::jmax (1.0, total))) + " %...",
                                juce::dontSendNotification);
                        });
                });
        });
}

void PartSoundsPanel::tuneNext (size_t part)
{
    if (! tuneButton.getToggleState() || part >= rows.size())
        return setEffectsFor();
    auto& row = rows[part];
    const auto* c = row.chosen();
    if (c == nullptr || c->kind != parts::Choice::Kind::preset || row.excerpt.notes.empty() || row.stem == juce::File())
        return tuneNext (part + 1);
    const auto source = c->source;
    const auto preset = c->preset;
    const auto stem = row.stem;
    const auto excerpt = row.excerpt;
    const auto options = context.helper;
    const auto tries = tuneTries;
    const auto name = parts::displayName (row.target.part).toLowerCase();
    auto found = std::make_shared<std::optional<SynthMatchResult>>();
    runInBackground (
        "Tuning " + source.name + " to the " + name + "...",
        [=, this, a = alive] (const std::function<bool()>& shouldStop)
        {
            juce::String problem;
            *found = parts::tuneInHelper (
                options, source, preset, stem, excerpt.start, excerpt.length, excerpt.notes, tries, shouldStop,
                [this, a, name, tries, synth = source.name] (int done, double)
                {
                    juce::MessageManager::callAsync (
                        [this, a, name, tries, synth, done]
                        {
                            if (*a)
                                status.setText ("Tuning " + synth + " to the " + name + ": " + juce::String (done) +
                                                    " of " + juce::String (tries) + " tries...",
                                                juce::dontSendNotification);
                        });
                },
                problem);
            return problem;
        },
        [this, part, found] (const juce::String& problem)
        {
            auto& r = rows[part];
            if (found->has_value())
            {
                r.tuned = **found;
                const auto closer =
                    r.tuned->startDistance > 0.0 ? 1.0 - r.tuned->distance / r.tuned->startDistance : 0.0;
                r.note = "Tuned toward the song: " + juce::String (juce::roundToInt (100.0 * closer)) +
                         " % closer than the preset as it is (" + juce::String (r.tuned->tries) + " tries).";
            }
            else
                r.note = "Not tuned: " + problem;
            updateRows();
            if (*stopping)
                return fail ("Stopped; what was found is kept.");
            tuneNext (part + 1);
        });
}

juce::var PartSoundsPanel::renderItem (const Row& row, const parts::Choice& c,
                                       const std::vector<OfflineInstrument::Note>& played, bool tuned,
                                       const juce::File& output) const
{
    using Kind = parts::Choice::Kind;
    if (c.kind == Kind::loop)
        return object ({ { "id", row.target.part },
                         { "cut", object ({ { "file", c.file },
                                            { "from", 0.0 },
                                            { "length", juce::jmin (8.0, juce::jmax (1.0, c.duration)) } }) },
                         { "output", output.getFullPathName() } });
    double end = 0.0;
    for (const auto& n : played)
        end = juce::jmax (end, n.start + n.length);
    if (c.kind == Kind::drumSamples || c.kind == Kind::sample)
    {
        // Samples put together at the notes' times (a sample on keys at its own pitch).
        juce::Array<juce::var> hits;
        for (const auto& n : played)
        {
            juce::String file = c.file;
            if (c.kind == Kind::drumSamples)
                for (const auto& [drum, path] : c.drums)
                    if (padOf (drum) == n.note)
                        file = path;
            if (file.isNotEmpty())
                hits.add (object ({ { "file", file }, { "at", n.start }, { "gain", (double)n.velocity } }));
        }
        return object ({ { "id", row.target.part },
                         { "mix", hits },
                         { "length", end + 1.0 },
                         { "output", output.getFullPathName() } });
    }
    auto notesPlayed = played;
    if (c.kind == Kind::kit) // the excerpt's drums are the Drum Machine's pads: the kit's own notes here
        for (auto& n : notesPlayed)
            for (const auto& [drum, note] : c.drumNotes)
                if (padOf (drum) == n.note)
                    n.note = note;
    auto item = object ({ { "id", row.target.part },
                          { "preset", c.preset },
                          { "notes", notesVar (notesPlayed) },
                          { "tail", 0.5 },
                          { "output", output.getFullPathName() } });
    if (tuned && row.tuned)
    {
        auto* values = new juce::DynamicObject();
        for (const auto& [id, value] : row.tuned->settings)
            values->setProperty (id, value);
        item.getDynamicObject()->setProperty ("settings", juce::var (values));
    }
    return item;
}

void PartSoundsPanel::setEffectsFor()
{
    if (! effectsButton.getToggleState() || ! aiReady() || rows.empty())
        return finish ({});
    // Each chosen sound plays its part's notes, and each part's stem is decoded, in the helper.
    struct Render
    {
        SoundSource source;
        juce::Array<juce::var> items;
    };
    std::vector<Render> renders;
    juce::Array<juce::var> toMeasure;
    juce::StringArray names;
    for (auto& row : rows)
    {
        const auto* c = row.chosen();
        if (c == nullptr || row.excerpt.notes.empty())
            continue;
        const auto output = work ("mix/" + row.target.part + ".wav");
        Render r;
        r.source =
            c->kind == parts::Choice::Kind::preset || c->kind == parts::Choice::Kind::kit ? c->source : SoundSource {};
        r.items.add (renderItem (row, *c, row.excerpt.notes, true, output));
        const auto wav = stemWav (row);
        if (! wav.existsAsFile())
            r.items.add (object (
                { { "id", "stem" },
                  { "cut", object ({ { "file", row.stem.getFullPathName() }, { "from", 0.0 }, { "length", -1.0 } }) },
                  { "output", wav.getFullPathName() } }));
        renders.push_back (std::move (r));
        toMeasure.add (object ({ { "name", row.target.part }, { "file", output.getFullPathName() } }));
        toMeasure.add (object ({ { "name", row.target.part + " reference" }, { "file", wav.getFullPathName() } }));
        names.add (row.target.part);
    }
    if (renders.empty())
        return finish ({});
    const auto options = context.helper;
    runInBackground (
        "Playing the chosen sounds to set their effects and levels...",
        [options, renders] (const std::function<bool()>& shouldStop)
        {
            juce::StringArray problems;
            for (const auto& r : renders)
            {
                juce::String problem;
                const auto done = parts::renderInHelper (options, r.source, r.items, shouldStop, problem);
                for (const auto& d : done)
                    if (d.hasProperty ("problem"))
                        problems.add (d["problem"].toString());
                if (problem.isNotEmpty())
                    problems.add (problem);
                if (shouldStop())
                    return juce::String ("Stopped.");
            }
            return problems.joinIntoString (" ");
        },
        [this, toMeasure, names] (const juce::String& problem)
        {
            if (*stopping)
                return fail ("Stopped.");
            juce::Array<juce::var> present;
            for (const auto& m : toMeasure)
                if (juce::File (m["file"].toString()).existsAsFile())
                    present.add (m);
            if (present.isEmpty())
                return finish ("No effects: the sounds could not be played (" + problem + ").");
            status.setText ("Measuring the chosen sounds and the songs' parts...", juce::dontSendNotification);
            ai::runJob (
                *context.service, "ai.v1.mix.analyze", object ({ { "inserts", present } }), alive,
                [this] (const juce::String& started) { job = started; },
                [this, names] (const juce::var& mix, const juce::String& error)
                {
                    job.clear();
                    if (error.isNotEmpty())
                        return finish ("No effects: " + error);
                    juce::Array<juce::var> list;
                    for (const auto& n : names)
                        list.add (n);
                    context.service->call ("ai.v1.mix.toward",
                                           object ({ { "mix", mix },
                                                     { "parts", list },
                                                     { "referenceLoudness", context.summary["loudness"]["lufs"] } }),
                                           [this, a = alive] (const juce::var& result, const juce::String& towardError)
                                           {
                                               if (! *a)
                                                   return;
                                               if (towardError.isNotEmpty())
                                                   return finish ("No effects: " + towardError);
                                               for (auto& row : rows)
                                               {
                                                   row.effects.clear();
                                                   row.volumeDb = 0.0;
                                               }
                                               master.clear();
                                               if (const auto* items = result["items"].getArray())
                                                   for (const auto& item : *items)
                                                   {
                                                       const auto insert = item["insert"].toString();
                                                       if (insert == "Master")
                                                           master.add (item);
                                                       for (auto& row : rows)
                                                           if (row.target.part == insert)
                                                           {
                                                               if (item["kind"].toString() == "volume")
                                                                   row.volumeDb = (double)item["db"];
                                                               else
                                                                   row.effects.add (item);
                                                           }
                                                   }
                                               finish ({});
                                           });
                });
        });
}

void PartSoundsPanel::finish (const juce::String& message)
{
    busy = false;
    juce::StringArray said;
    if (message.isNotEmpty())
        said.add (message);
    said.addArray (notes);
    const auto ready = std::count_if (rows.begin(), rows.end(), [] (const Row& r) { return ! r.choices.empty(); });
    status.setText ("Sounds chosen for " + juce::String ((int)ready) + " of " + juce::String ((int)rows.size()) +
                        " parts. Listen, swap, then Add these sounds to the project." +
                        (said.isEmpty() ? juce::String() : " " + said.joinIntoString (" ")),
                    juce::dontSendNotification);
    updateRows();
    updateButtons();
}

void PartSoundsPanel::fail (const juce::String& message)
{
    busy = false;
    status.setText (message, juce::dontSendNotification);
    updateRows();
    updateButtons();
}

void PartSoundsPanel::stop()
{
    *stopping = true;
    if (context.service != nullptr && job.isNotEmpty())
        context.service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

bool PartSoundsPanel::select (int row, int choice)
{
    if (! juce::isPositiveAndBelow (row, (int)rows.size()) ||
        ! juce::isPositiveAndBelow (choice, (int)rows[(size_t)row].choices.size()))
        return false;
    auto& r = rows[(size_t)row];
    if (r.selected == choice)
        return true;
    r.selected = choice;
    // The tuning and the effects were for the other sound.
    const bool had = r.tuned.has_value() || ! r.effects.isEmpty() || std::abs (r.volumeDb) > 0.0;
    r.tuned.reset();
    r.effects.clear();
    r.volumeDb = 0.0;
    r.note = had ? "Swapped: Set effects again to set its effects and level." : juce::String();
    updateRows();
    return true;
}

bool PartSoundsPanel::setEffectsAgain()
{
    if (busy || rows.empty())
        return false;
    if (! aiReady())
    {
        status.setText ("Effects and mix need AI on (Options > AI): the AI service measures the sounds.",
                        juce::dontSendNotification);
        return false;
    }
    busy = true;
    *stopping = false;
    notes.clear();
    updateButtons();
    const auto wanted = effectsButton.getToggleState();
    effectsButton.setToggleState (true, juce::dontSendNotification);
    setEffectsFor();
    effectsButton.setToggleState (wanted, juce::dontSendNotification);
    return true;
}

bool PartSoundsPanel::listen (int index)
{
    if (busy || ! juce::isPositiveAndBelow (index, (int)rows.size()) || rows[(size_t)index].chosen() == nullptr)
        return false;
    const auto& row = rows[(size_t)index];
    const auto& c = *row.chosen();
    const auto output = work ("listen/" + row.target.part + "-" + juce::String (row.selected) + ".wav");
    output.deleteFile();
    const auto phrase = parts::auditionPhrase (row.target, c, context.summary["tempo"]["median"], context.summary);
    const auto item = renderItem (row, c, phrase, true, output);
    const auto source =
        c.kind == parts::Choice::Kind::preset || c.kind == parts::Choice::Kind::kit ? c.source : SoundSource {};
    const auto options = context.helper;
    busy = true;
    *stopping = false;
    updateButtons();
    runInBackground (
        "Playing " + c.title() + "...",
        [options, source, item] (const std::function<bool()>& shouldStop)
        {
            juce::String problem;
            const auto done = parts::renderInHelper (options, source, { item }, shouldStop, problem);
            if (! done.isEmpty() && done[0].hasProperty ("problem"))
                problem = done[0]["problem"].toString();
            return problem;
        },
        [this, output, title = c.title()] (const juce::String& problem)
        {
            busy = false;
            updateButtons();
            if (! output.existsAsFile())
                return fail (title + " could not be played: " + problem);
            lastPlayed = output;
            session.getSamplePreview().play (output, dsp::getAudioFormats());
            status.setText ("Playing " + title + ".", juce::dontSendNotification);
        });
    return true;
}

bool PartSoundsPanel::listenToSongs (int index)
{
    if (busy || ! juce::isPositiveAndBelow (index, (int)rows.size()) || rows[(size_t)index].stem == juce::File())
        return false;
    const auto& row = rows[(size_t)index];
    const auto output = work ("listen/" + row.target.part + "-song.wav");
    const auto item = object ({ { "id", "song" },
                                { "cut", object ({ { "file", row.stem.getFullPathName() },
                                                   { "from", row.excerpt.start },
                                                   { "length", juce::jmax (2.0, row.excerpt.length + 0.5) } }) },
                                { "output", output.getFullPathName() } });
    const auto options = context.helper;
    busy = true;
    *stopping = false;
    updateButtons();
    runInBackground (
        "Cutting the part from " + row.songName + "...",
        [options, item] (const std::function<bool()>& shouldStop)
        {
            juce::String problem;
            parts::renderInHelper (options, {}, { item }, shouldStop, problem);
            return problem;
        },
        [this, output, name = row.songName, part = parts::displayName (row.target.part)] (const juce::String& problem)
        {
            busy = false;
            updateButtons();
            if (! output.existsAsFile())
                return fail ("The song's part could not be played: " + problem);
            lastPlayed = output;
            session.getSamplePreview().play (output, dsp::getAudioFormats());
            status.setText ("Playing the " + part.toLowerCase() + " of " + name + " (its stem).",
                            juce::dontSendNotification);
        });
    return true;
}

parts::Plan PartSoundsPanel::makePlan() const
{
    parts::Plan plan;
    plan.summary = context.summary;
    plan.tempo = session.getProject().settings.tempoBpm;
    for (const auto& row : rows)
    {
        if (row.chosen() == nullptr)
            continue;
        parts::Plan::Part part;
        part.target = row.target;
        part.choice = *row.chosen();
        part.tuned = row.tuned;
        part.effects = row.effects;
        part.volumeDb = row.volumeDb;
        plan.parts.push_back (std::move (part));
    }
    plan.master = master;
    return plan;
}

bool PartSoundsPanel::addToProject()
{
    if (busy)
        return false;
    const auto plan = makePlan();
    if (plan.parts.empty())
    {
        status.setText ("There are no sounds to add: click Choose sounds first.", juce::dontSendNotification);
        return false;
    }
    auto proposal = ai::Proposal::prepare (parts::makeProposal (plan), session.getHistory());
    proposal.undoName = "Track creator: " + proposal.summary;
    if (const auto result = proposal.apply (session.getHistory()); result.failed())
    {
        status.setText ("The sounds could not be added: " + result.getErrorMessage(), juce::dontSendNotification);
        return false;
    }
    // The channels the steps made, in the parts' order (a loop plays on a Playlist track instead).
    std::vector<model::ChannelId> made;
    for (const auto& step : proposal.steps)
        if (step.command == "channel.set")
            made.push_back (model::ChannelId { (uint32_t)(int)step.parameters["channel"] });
    added.clear();
    size_t next = 0;
    for (const auto& part : plan.parts)
        added.push_back (part.choice.kind == parts::Choice::Kind::loop || next >= made.size() ? model::ChannelId {}
                                                                                              : made[next++]);
    const auto loaded = parts::loadSounds (session, plan, added);
    status.setText ("Added " + juce::String ((int)plan.parts.size()) +
                        " parts, as one undo step (Ctrl+Z takes them away); the pattern \"Track creator: sounds\" "
                        "plays a short phrase on each." +
                        (loaded.failed() ? " Not every sound loaded: " + loaded.getErrorMessage() : juce::String()),
                    juce::dontSendNotification);
    return true;
}

//==============================================================================
juce::String PartSoundsPanel::describe (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int)rows.size()))
        return {};
    const auto& row = rows[(size_t)index];
    juce::String text = parts::displayName (row.target.part) + ": ";
    const auto* c = row.chosen();
    text += c == nullptr ? juce::String ("no sound in the index fits") : c->title();
    juce::StringArray lines;
    if (c != nullptr)
        lines.add (c->reasons());
    if (row.measured > 0)
        lines.add ("Compared with " + juce::String (row.measured) + (row.target.part == "drums" ? " hits" : " notes") +
                   " of the songs' " + row.target.part + " stems.");
    if (! row.effects.isEmpty() || std::abs (row.volumeDb) > 0.0)
    {
        juce::StringArray fx;
        for (const auto& e : row.effects)
            fx.add (e["title"].toString());
        if (std::abs (row.volumeDb) > 0.0)
            fx.add ("fader " + juce::String (row.volumeDb, 1) + " dB");
        lines.add ("Effects: " + fx.joinIntoString (", ") + ".");
    }
    if (row.note.isNotEmpty())
        lines.add (row.note);
    return text + "\n" + lines.joinIntoString (" ");
}

void PartSoundsPanel::updateRows()
{
    while (views.size() < rows.size())
    {
        views.push_back (std::make_unique<RowView> (*this, (int)views.size()));
        list.addAndMakeVisible (*views.back());
    }
    while (views.size() > rows.size())
        views.pop_back();
    for (size_t i = 0; i < rows.size(); ++i)
        views[i]->update (rows[i]);
    resized();
}

void PartSoundsPanel::updateButtons()
{
    chooseButton.setEnabled (! busy);
    addButton.setEnabled (! busy && ! rows.empty());
    effectsAgainButton.setEnabled (! busy && ! rows.empty());
    stopButton.setVisible (busy);
    for (auto* b : { &closerButton, &tuneButton, &effectsButton })
        b->setEnabled (! busy);
    resized();
}

void PartSoundsPanel::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (36));
    {
        auto r = area.removeFromTop (26);
        closerButton.setBounds (r.removeFromLeft (160));
        tuneButton.setBounds (r.removeFromLeft (180));
        effectsButton.setBounds (r.removeFromLeft (180));
    }
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (28);
        chooseButton.setBounds (r.removeFromLeft (130).reduced (0, 1));
        r.removeFromLeft (6);
        if (stopButton.isVisible())
        {
            stopButton.setBounds (r.removeFromLeft (70).reduced (0, 1));
            r.removeFromLeft (6);
        }
        effectsAgainButton.setBounds (r.removeFromLeft (130).reduced (0, 1));
        addButton.setBounds (r.removeFromRight (230).reduced (0, 1));
    }
    area.removeFromTop (4);
    status.setBounds (area.removeFromBottom (54));
    viewport.setBounds (area);
    const auto width = juce::jmax (100, area.getWidth() - viewport.getScrollBarThickness());
    constexpr int rowHeight = 84;
    list.setSize (width, rowHeight * (int)views.size());
    for (size_t i = 0; i < views.size(); ++i)
        views[i]->setBounds (0, (int)i * rowHeight, width, rowHeight - 4);
}

void PartSoundsPanel::paint (juce::Graphics&) {}

} // namespace spinus::ui

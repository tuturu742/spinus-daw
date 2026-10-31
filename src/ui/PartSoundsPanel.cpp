#include "PartSoundsPanel.h"
#include "SaveSoundWindow.h"

#include "ai/AnalysisJobs.h"
#include "ai/Proposal.h"
#include "dsp/sampler/SampleStorage.h"
#include "session/PresetPreviews.h"
#include "session/UserRoles.h"
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
    static constexpr int otherId = 10000, saveId = 10001;

    RowView (PartSoundsPanel& p, int i) : panel (p), index (i)
    {
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        why.setFont (juce::FontOptions (13.0f));
        why.setJustificationType (juce::Justification::topLeft);
        why.setMinimumHorizontalScale (1.0f);
        why.setColour (juce::Label::textColourId, findColour (juce::Label::textColourId).withAlpha (0.75f));
        choice.onChange = [this]
        {
            if (choice.getSelectedId() == saveId) // the instrument in its own window, to set up and save
            {
                juce::MessageManager::callAsync (
                    [safe = juce::Component::SafePointer<PartSoundsPanel> (&panel), row = index]
                    {
                        if (safe == nullptr)
                            return;
                        safe->updateRows(); // shows the sound chosen until the new one is saved
                        safe->saveSound (row);
                    });
                return;
            }
            if (choice.getSelectedId() == otherId) // any instrument and preset, chosen from a menu
            {
                juce::MessageManager::callAsync (
                    [safe = juce::Component::SafePointer<PartSoundsPanel> (&panel), row = index]
                    {
                        if (safe == nullptr)
                            return;
                        safe->updateRows(); // shows the sound chosen until another is picked
                        safe->showOtherMenu (row);
                    });
                return;
            }
            panel.select (index, choice.getSelectedId() - 1);
        };
        for (int r = 0; r < parts::songRoles().size(); ++r)
            role.addItem (parts::roleName (parts::songRoles()[r]), r + 1);
        role.setTooltip ("What this part plays in the song");
        role.onChange = [this] { panel.setRole (index, parts::songRoles()[role.getSelectedId() - 1]); };
        tuneToggle.setTooltip ("Tune this synth toward the part in your reference songs with synth match, in a "
                               "separate program (a minute or more; Stop keeps what was found)");
        tuneToggle.onClick = [this] { panel.setTuneFor (index, tuneToggle.getToggleState()); };
        removeButton.setTooltip ("Remove this part: the song is written without it");
        removeButton.onClick = [this]
        {
            juce::Component::SafePointer<PartSoundsPanel> safe (&panel);
            juce::MessageManager::callAsync (
                [safe, row = index]
                {
                    if (safe != nullptr)
                        safe->removePart (row);
                });
        };
        choice.setTooltip ("The closest sounds for this part, the closest first: pick another to swap it");
        listenButton.setTooltip ("Hear the chosen sound play a short phrase");
        listenButton.onClick = [this] { panel.listen (index); };
        // The instrument in its own window, to set up a sound (a kit, a patch) and keep it.
        saveButton.onClick = [this]
        {
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<PartSoundsPanel> (&panel), row = index]
                {
                    if (safe != nullptr)
                        safe->saveSound (row);
                });
        };
        songsButton.setTooltip ("Hear the part in the reference song it was compared with");
        songsButton.onClick = [this] { panel.listenToSongs (index); };
        for (auto* c : std::initializer_list<juce::Component*> { &title, &role, &removeButton, &choice, &listenButton,
                                                                 &saveButton, &why })
            addAndMakeVisible (c);
        addChildComponent (tuneToggle);
        addChildComponent (songsButton);
    }

    void update (const Row& row)
    {
        title.setText (row.part.name, juce::dontSendNotification);
        role.setSelectedId (parts::songRoles().indexOf (row.part.role) + 1, juce::dontSendNotification);
        choice.clear (juce::dontSendNotification);
        for (size_t i = 0; i < row.choices.size(); ++i)
            choice.addItem (juce::String ((int)i + 1) + ". " + row.choices[i].title() +
                                (i == 0 && row.picked ? " (your choice)" : ""),
                            (int)i + 1);
        choice.addSeparator();
        choice.addItem ("Other sound...", otherId);
        // A plugin's own window, to set up a sound (a kit, a patch) and keep it: for the chosen instrument.
        choice.addItem ("Save this sound...", saveId);
        choice.setItemEnabled (saveId, panel.whySaveSoundIsOff (index).isEmpty());
        if (! row.choices.empty())
            choice.setSelectedId (row.selected + 1, juce::dontSendNotification);
        choice.setTextWhenNothingSelected (row.choices.empty() && row.measured == 0 && row.note.isEmpty()
                                               ? "Choose sounds finds one (or Other sound...)"
                                               : "No sound in the index fits: Other sound...");
        why.setText (panel.describe (index).fromFirstOccurrenceOf ("\n", false, false), juce::dontSendNotification);
        listenButton.setEnabled (! row.choices.empty());
        // Only with references: it tunes toward their part.
        tuneToggle.setVisible (row.stem != juce::File() && row.chosen() != nullptr &&
                               row.chosen()->kind == parts::Choice::Kind::preset);
        tuneToggle.setToggleState (row.tune, juce::dontSendNotification);
        songsButton.setVisible (panel.hasReferences());
        songsButton.setEnabled (row.stem != juce::File());
        updateSaveButton();
        resized();
    }

    /** Save this sound: on whenever the part has an instrument; its tooltip says why when it is off. */
    void updateSaveButton()
    {
        const auto off = panel.whySaveSoundIsOff (index);
        saveButton.setEnabled (off.isEmpty());
        saveButton.setTooltip (off.isNotEmpty()
                                   ? off
                                   : "Open this part's instrument in its own window: set a sound up there (choose a "
                                     "kit or a patch), name it and Save. The Track creator uses it from then on (for "
                                     "an instrument that is silent until a sound is chosen, such as some drum "
                                     "players)");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (2);
        auto top = r.removeFromTop (26);
        title.setBounds (top.removeFromLeft (120));
        role.setBounds (top.removeFromLeft (132).reduced (0, 1));
        top.removeFromLeft (4);
        removeButton.setBounds (top.removeFromRight (26).reduced (0, 1));
        top.removeFromRight (4);
        if (songsButton.isVisible())
        {
            songsButton.setBounds (top.removeFromRight (90).reduced (0, 1));
            top.removeFromRight (4);
        }
        saveButton.setBounds (top.removeFromRight (120).reduced (0, 1));
        top.removeFromRight (4);
        listenButton.setBounds (top.removeFromRight (64).reduced (0, 1));
        top.removeFromRight (6);
        choice.setBounds (top.reduced (0, 1));
        if (tuneToggle.isVisible())
            tuneToggle.setBounds (r.removeFromBottom (22).removeFromLeft (240));
        why.setBounds (r);
    }

    juce::ComboBox choice;

private:
    PartSoundsPanel& panel;
    int index;
    juce::Label title, why;
    juce::ComboBox role;
    juce::ToggleButton tuneToggle { "Tune toward the references" };
    juce::TextButton listenButton { "Listen" }, saveButton { "Save this sound..." }, songsButton { "The songs'" },
        removeButton { "x" };
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
    intro.setText ("The song's parts, each with a role and its sound: the closest you have (from the sound index), or "
                   "any instrument and preset with Other sound. The song is written with exactly these.",
                   juce::dontSendNotification);
    intro.setMinimumHorizontalScale (1.0f);
    intro.setJustificationType (juce::Justification::topLeft);
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (1.0f);
    closerButton.setTooltip ("Compare the closest presets with the songs' stems by listening, with closest preset's "
                             "CLAP model (AI on, the analysis engine and its model files)");
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
    addPartButton.setTooltip ("Add a part to the song: drums, bass, chords, pad, arpeggio, lead, counter-melody or an "
                              "effect (two of one role play different things)");
    addPartButton.onClick = [this] { showAddMenu(); };
    suggestButton.setTooltip ("Fill the list with the parts a song like this usually has: your genre's, a sketch "
                              "(drums, bass, chords, lead) or a full arrangement");
    suggestButton.onClick = [this] { showSuggestMenu(); };
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    for (auto* component : std::initializer_list<juce::Component*> {
             &intro, &closerButton, &effectsButton, &chooseButton, &addPartButton, &suggestButton, &effectsAgainButton,
             &addButton, &viewport, &status })
        addAndMakeVisible (component);
    addChildComponent (stopButton);
    updateButtons();
}

PartSoundsPanel::~PartSoundsPanel()
{
    SaveSoundWindow::closeAll(); // their instruments live in the session's engine
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
    return work ("stems/" + row.songDigest.substring (0, 12) + "-" + row.target.id + ".wav");
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
    if (rows.empty())
        resetParts();
    if (rows.empty())
    {
        status.setText ("There are no parts yet: Add part..., or Suggest parts.", juce::dontSendNotification);
        return false;
    }
    busy = true;
    *stopping = false;
    notes.clear();
    updateButtons();
    auto state = std::make_shared<Measuring>();
    state->context = context;
    // The user's parts, the sound each has now (kept when it is among the new choices) and the one the user
    // picked themselves (kept first, whatever).
    struct Wanted
    {
        parts::SongPart part;
        juce::String had;
        std::optional<parts::Choice> picked;
        bool tune;
    };
    std::vector<Wanted> wanted;
    wanted.reserve (rows.size());
    for (const auto& row : rows)
        wanted.push_back ({ row.part, row.chosen() != nullptr ? row.chosen()->title() : juce::String(), row.picked,
                            row.tune || tuneByDefault });
    // Another seed each time: Choose sounds again proposes others.
    const auto seed =
        (juce::uint32)juce::JSON::toString (context.summary, true).hashCode() + (juce::uint32)++chooseCount;
    runInBackground (
        "Measuring the songs' parts and choosing sounds...",
        [state, wanted, seed] (const std::function<bool()>& shouldStop)
        {
            SoundIndex index (state->context.indexFolder);
            index.load();
            UserRoles marked (state->context.indexFolder);
            marked.load();
            std::map<juce::String, int> instances;
            std::set<juce::String> used; // the instruments suggested first for the parts before
            for (const auto& [part, had, picked, tune] : wanted)
            {
                const auto target = parts::targetFor (part, instances[part.role]++, state->context.summary);
                Row row;
                row.part = part;
                row.target = target;
                row.picked = picked;
                row.tune = tune;
                parts::Reference reference;
                size_t most = 0;
                for (const auto& song : state->context.songs)
                {
                    if (! target.measure)
                        break;
                    if (shouldStop())
                        return juce::String ("Stopped.");
                    const auto entry = state->context.library.read (song.digest);
                    // The Track creator's stems: 6 stems, or the 4 of a song analyzed before.
                    juce::String model = parts::separationModel;
                    auto stems = state->context.library.findStems (song.digest, model);
                    if (stems.find (target.part) == stems.end())
                    {
                        model = parts::olderSeparationModel;
                        stems = state->context.library.findStems (song.digest, model);
                    }
                    const auto stem = stems.find (target.part);
                    if (! entry.isObject() || stem == stems.end())
                        continue;
                    std::vector<HiddenInstrument::Note> played;
                    if (target.part == "drums")
                    {
                        const auto hits = parts::stemHitsFromVar (entry["drums"][juce::Identifier (model)]);
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
                        const auto stemNotes =
                            parts::stemNotesFromVar (entry["notes"][juce::Identifier (model + "/" + target.part)]);
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
                row.choices = parts::vary (parts::choose (target, reference, index, 40), used,
                                           seed + (juce::uint32)state->rows.size(), 6);
                // Instruments the index has not measured that the user marked for this role: offered too.
                for (const auto& source : state->context.sources)
                    if (marked.get (source.key) == part.role && ! index.instruments.contains (source.key))
                    {
                        parts::Choice c;
                        c.kind = part.role == "drums" || part.role == "percussion" ? parts::Choice::Kind::kit
                                                                                   : parts::Choice::Kind::preset;
                        c.instrument = source.key;
                        c.source = source;
                        c.name = "Default sound";
                        c.drumNotes = { { "kick", 36 },   { "snare", 38 },  { "hat", 42 },     { "openHat", 46 },
                                        { "tomLow", 45 }, { "tomMid", 47 }, { "tomHigh", 50 }, { "crash", 49 } };
                        c.why = { "you marked it as " + parts::roleName (part.role).toLowerCase(), "not measured" };
                        row.choices.insert (row.choices.begin(), c);
                    }
                if (! row.choices.empty())
                    used.insert (row.choices.front().instrument);
                if (picked)
                    row.choices.insert (row.choices.begin(), *picked);
                for (size_t i = 0; i < row.choices.size(); ++i)
                    if (had.isNotEmpty() && row.choices[i].title() == had)
                    {
                        row.selected = (int)i;
                        break;
                    }
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
    const auto& candidates = rows[part];
    // The first five presets of one instrument (the closest's), compared by closest preset's CLAP model.
    std::vector<size_t> presets;
    const auto* first = candidates.chosen();
    for (size_t i = 0; i < candidates.choices.size(); ++i)
        if (candidates.choices[i].kind == parts::Choice::Kind::preset && first != nullptr &&
            first->kind == parts::Choice::Kind::preset && candidates.choices[i].instrument == first->instrument)
            presets.push_back (i);
    if (presets.size() < 2 || candidates.stem == juce::File())
        return listenCloser (part + 1);

    // Is the CLAP model there?
    ai::checkAnalysisEngine (
        *context.service, alive,
        [this, part, presets] (const ai::EngineCheck& engine)
        {
            bool hasClap = false;
            if (auto* models = engine.models["models"].getArray())
                for (auto& m : *models)
                    hasClap = hasClap || (m["id"].toString() == "clap_music" && (bool)m["downloaded"]);
            if (engine.error.isNotEmpty() || ! engine.installed || ! hasClap)
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
                const auto file = work ("previews/" + row.target.id + "-" + juce::String ((int)i) + ".wav");
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
                "Playing the " + row.target.name.toLowerCase() + "'s closest presets to listen closer...",
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
                    status.setText ("Listening to the " + rows[part].target.name.toLowerCase() + "'s presets...",
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
                                    else if (const auto* heard = found["presets"].getArray())
                                    {
                                        // Ranked again: the measurements' order and the listening's (by its
                                        // CLAP score alone: the measurements are already counted) alike.
                                        for (auto i : presets)
                                            for (int m = 0; m < heard->size(); ++m)
                                                if ((*heard)[m]["id"].toString() == r.choices[i].preset)
                                                    r.choices[i].clap = (double)(*heard)[m]["score"];
                                        std::vector<std::pair<double, size_t>> order;
                                        for (size_t k = 0; k < presets.size(); ++k)
                                        {
                                            const auto clap = r.choices[presets[k]].clap;
                                            int place = 0;
                                            for (auto other : presets)
                                                place += r.choices[other].clap > clap ? 1 : 0;
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
                                "Listening to the " + rows[part].target.name.toLowerCase() + "'s presets: " +
                                    juce::String (juce::roundToInt (100.0 * done / juce::jmax (1.0, total))) + " %...",
                                juce::dontSendNotification);
                        });
                });
        });
}

void PartSoundsPanel::tuneNext (size_t part)
{
    if (part >= rows.size())
        return tuningOne ? finish ({}) : setEffectsFor();
    auto& row = rows[part];
    const auto* c = row.chosen();
    if (! row.tune || c == nullptr || c->kind != parts::Choice::Kind::preset || row.excerpt.notes.empty() ||
        row.stem == juce::File())
        return tuningOne ? finish ({}) : tuneNext (part + 1);
    const auto source = c->source;
    const auto preset = c->preset;
    const auto stem = row.stem;
    const auto excerpt = row.excerpt;
    const auto options = context.helper;
    const auto tries = tuneTries;
    const auto name = row.target.name.toLowerCase();
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
            if (tuningOne)
            {
                tuningOne = false;
                return finish ({});
            }
            tuneNext (part + 1);
        });
}

juce::var PartSoundsPanel::renderItem (const Row& row, const parts::Choice& c,
                                       const std::vector<OfflineInstrument::Note>& played, bool tuned,
                                       const juce::File& output) const
{
    using Kind = parts::Choice::Kind;
    if (c.kind == Kind::loop)
        return object ({ { "id", row.target.id },
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
        return object ({ { "id", row.target.id },
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
    auto item = object ({ { "id", row.target.id },
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
        const auto output = work ("mix/" + row.target.id + ".wav");
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
        toMeasure.add (object ({ { "name", row.target.id }, { "file", output.getFullPathName() } }));
        toMeasure.add (object ({ { "name", row.target.id + " reference" }, { "file", wav.getFullPathName() } }));
        names.add (row.target.id);
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
                    juce::Array<juce::var> partNames;
                    for (const auto& n : names)
                        partNames.add (n);
                    context.service->call ("ai.v1.mix.toward",
                                           object ({ { "mix", mix },
                                                     { "parts", partNames },
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
                                                           if (row.target.id == insert)
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
    const auto output = work ("listen/" + row.target.id + "-" + juce::String (row.selected) + ".wav");
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
    const auto output = work ("listen/" + row.target.id + "-song.wav");
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
        [this, output, name = row.songName, part = row.target.name] (const juce::String& problem)
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

std::vector<parts::SongPart> PartSoundsPanel::getParts() const
{
    std::vector<parts::SongPart> found;
    found.reserve (rows.size());
    for (const auto& row : rows)
        found.push_back (row.part);
    return found;
}

void PartSoundsPanel::resetParts (const juce::String& why)
{
    if (busy)
        stop();
    context = getContext ? getContext() : Context {};
    rows.clear();
    master.clear();
    // The parts a description names ("guitar, bass and drums"); none otherwise.
    setParts (parts::describedParts (context.summary));
    status.setText ((why.isNotEmpty() ? why + " " : juce::String()) +
                        (rows.empty() ? "Add parts, or Suggest parts, then Choose sounds."
                                      : "These are the parts your description names: add or remove parts, give each "
                                        "a role, then Choose sounds."),
                    juce::dontSendNotification);
    updateRows();
    updateButtons();
}

bool PartSoundsPanel::addPart (const juce::String& role)
{
    if (busy || ! parts::songRoles().contains (role) || rows.size() >= 16)
        return false;
    context = getContext ? getContext() : Context {};
    Row row;
    row.part = parts::newPart (getParts(), role);
    row.target = parts::targetFor (row.part, 0, context.summary);
    rows.push_back (std::move (row));
    status.setText ("Added " + rows.back().part.name + ": Choose sounds finds its sound (the others keep theirs).",
                    juce::dontSendNotification);
    updateRows();
    updateButtons();
    return true;
}

bool PartSoundsPanel::removePart (int index)
{
    if (busy || ! juce::isPositiveAndBelow (index, (int)rows.size()) || rows.size() <= 1)
        return false;
    const auto name = rows[(size_t)index].part.name;
    rows.erase (rows.begin() + index);
    status.setText ("Removed " + name + ": the song is written without it.", juce::dontSendNotification);
    updateRows();
    updateButtons();
    return true;
}

bool PartSoundsPanel::setRole (int index, const juce::String& role)
{
    if (busy || ! juce::isPositiveAndBelow (index, (int)rows.size()) || ! parts::songRoles().contains (role))
        return false;
    context = getContext ? getContext() : Context {};
    auto& row = rows[(size_t)index];
    if (row.part.role == role)
        return true;
    row.part.role = role;
    row.target = parts::targetFor (row.part, 0, context.summary);
    row.choices.clear();
    row.selected = 0;
    row.tuned.reset();
    row.effects.clear();
    row.note = "Its role changed: Choose sounds finds a sound for it.";
    updateRows();
    return true;
}

void PartSoundsPanel::setParts (const std::vector<parts::SongPart>& wanted)
{
    rows.clear();
    std::map<juce::String, int> instances;
    for (const auto& part : wanted)
    {
        Row row;
        row.part = part;
        row.target = parts::targetFor (part, instances[part.role]++, context.summary);
        rows.push_back (std::move (row));
    }
    updateRows();
    updateButtons();
}

bool PartSoundsPanel::suggestParts (const juce::String& which)
{
    if (busy)
        return false;
    context = getContext ? getContext() : Context {};
    const auto suggested = parts::suggestedParts (which, context.summary);
    if (suggested.empty())
        return false;
    setParts (suggested);
    juce::StringArray names;
    for (const auto& p : suggested)
        names.add (p.name);
    status.setText ("Suggested: " + names.joinIntoString (", ") + ". Change them as you like, then Choose sounds.",
                    juce::dontSendNotification);
    return true;
}

void PartSoundsPanel::showSuggestMenu()
{
    context = getContext ? getContext() : Context {};
    juce::PopupMenu menu;
    auto label = [this] (const juce::String& which)
    {
        juce::StringArray names;
        for (const auto& p : parts::suggestedParts (which, context.summary))
            names.add (p.name);
        return names.joinIntoString (", ");
    };
    if (const auto genre = context.summary["described"]["genreName"].toString(); genre.isNotEmpty())
        menu.addItem ("For " + genre + ": " + label ("genre"), [this] { suggestParts ("genre"); });
    menu.addItem ("Sketch: " + label ("sketch"), [this] { suggestParts ("sketch"); });
    menu.addItem ("Full: " + label ("full"), [this] { suggestParts ("full"); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&suggestButton));
}

void PartSoundsPanel::showAddMenu()
{
    juce::PopupMenu menu;
    for (const auto& role : parts::songRoles())
        menu.addItem (parts::roleName (role), [this, role] { addPart (role); });
    menu.addSeparator();
    // A part from an instrument: its role the one you marked it with, or what its name says ("Steel Guitar": an
    // acoustic guitar); its sound that one.
    juce::PopupMenu instruments;
    fillInstrumentMenu (instruments, [this] (const parts::Choice& c) { addPartFrom (c); });
    menu.addSubMenu ("From an instrument", instruments);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addPartButton));
}

juce::String PartSoundsPanel::roleOf (const parts::Choice& c) const
{
    UserRoles marked (context.indexFolder);
    marked.load();
    const auto mine = marked.get (c.instrument.isNotEmpty() ? c.instrument : c.source.key, c.preset);
    return mine.isNotEmpty() ? mine : parts::roleFromName (c.source.name + " " + c.name + " " + c.category);
}

bool PartSoundsPanel::addPartFrom (const parts::Choice& c)
{
    auto role = roleOf (c);
    if (role.isEmpty())
        role = c.source.stockType == "spinus.drum-machine" ? "drums" : "lead";
    if (! addPart (role))
        return false;
    return pickSound ((int)rows.size() - 1, c);
}

namespace
{
/** A saved sound the index has not measured (yet), as the index would list it. */
juce::var savedSoundVar (const SavedSounds::Sound& s)
{
    return object (
        { { "id", s.id }, { "name", s.name }, { "version", s.version }, { "category", SavedSounds::category } });
}

/** The notes a drum instrument plays each drum on: its own map when the index measured one, the Drum Machine's
    pads, or General MIDI's drums. */
std::map<juce::String, int> drumNotesOf (const SoundSource& source, const juce::var& sound)
{
    if (source.stockType == "spinus.drum-machine")
        return { { "kick", 60 },   { "snare", 61 },  { "hat", 63 },     { "openHat", 64 },
                 { "tomLow", 65 }, { "tomMid", 66 }, { "tomHigh", 67 }, { "crash", 70 } };
    std::map<juce::String, int> notes { { "kick", 36 },   { "snare", 38 },  { "hat", 42 },     { "openHat", 46 },
                                        { "tomLow", 45 }, { "tomMid", 47 }, { "tomHigh", 50 }, { "crash", 49 } };
    if (const auto* map = sound["drumMap"].getArray())
        for (const auto& entry : *map)
        {
            const auto cls = entry["class"].toString();
            const auto drum = cls == "closed_hat" ? juce::String ("hat") : cls == "clap" ? juce::String ("snare") : cls;
            if (notes.contains (drum))
                notes[drum] = (int)entry["note"];
        }
    return notes;
}
} // namespace

parts::Choice PartSoundsPanel::choiceOf (const SoundSource& source, const juce::var& sound, bool measured) const
{
    parts::Choice c;
    c.kind = parts::Choice::Kind::preset;
    c.instrument = source.key;
    c.source = source;
    c.preset = sound["id"].toString();
    c.version = sound["version"].toString();
    c.name = sound.isObject() ? sound["name"].toString() : juce::String ("Default sound");
    c.category = sound["category"].toString();
    c.role = sound["role"].toString();
    c.low = (int)sound["low"];
    c.high = (int)sound["high"];
    c.polyphonic = (bool)sound["polyphonic"];
    c.drumNotes = drumNotesOf (source, sound);
    if (SavedSounds::isSaved (c.preset))
    {
        c.state = SavedSounds::stateFileIn (context.indexFolder, c.preset);
        c.why.add ("your saved sound");
    }
    else
        c.why.add ("chosen by you");
    if (measured && (bool)sound["silent"] && ! sound.hasProperty ("problem"))
        c.why.add ("silent as it opens: set a sound up in its window with Save this sound");
    else if (! measured || ! sound.isObject() || sound.hasProperty ("problem") || (bool)sound["silent"])
        c.why.add ("not measured");
    return c;
}

void PartSoundsPanel::fillInstrumentMenu (juce::PopupMenu& menu,
                                          const std::function<void (const parts::Choice&)>& picked)
{
    context = getContext ? getContext() : Context {};
    SoundIndex index (context.indexFolder);
    index.load();
    auto sources = context.sources;
    std::sort (sources.begin(), sources.end(),
               [] (const SoundSource& a, const SoundSource& b) { return a.name.compareIgnoreCase (b.name) < 0; });

    // The sounds the user saved (Save this sound), first: those of instruments still installed.
    SavedSounds saved (context.indexFolder);
    saved.load();
    juce::PopupMenu savedMenu;
    for (const auto& s : saved.all())
    {
        const auto source = std::find_if (sources.begin(), sources.end(),
                                          [&s] (const SoundSource& o) { return o.key == s.instrument; });
        if (source == sources.end())
            continue;
        const auto found = index.instruments.find (source->key);
        juce::var sound;
        if (found != index.instruments.end())
            for (const auto& known : found->second.sounds)
                if (known["id"].toString() == s.id)
                    sound = known;
        if (! sound.isObject()) // not measured yet: Update sound index measures it
            sound = savedSoundVar (s);
        const auto heard = sound.hasProperty ("role") && ! (bool)sound["silent"];
        auto c = choiceOf (*source, sound, heard);
        savedMenu.addItem (s.name + " (" + source->name + ")" + (heard ? "" : " (not measured)"),
                           [picked, c] { picked (c); });
    }
    if (savedMenu.getNumItems() > 0)
    {
        menu.addSubMenu (SavedSounds::category, savedMenu);
        menu.addSeparator();
    }

    for (const auto& source : sources)
    {
        const auto found = index.instruments.find (source.key);
        const bool measured = found != index.instruments.end() && found->second.problem.isEmpty();
        auto make = [this, source, measured] (const juce::var& sound) { return choiceOf (source, sound, measured); };
        // Its sound as it opens: as measured, when it was (an instrument without presets).
        juce::var asItOpens;
        if (found != index.instruments.end())
            for (const auto& sound : found->second.sounds)
                if (sound["id"].toString().isEmpty())
                    asItOpens = sound;
        const auto silent = (bool)asItOpens["silent"] && ! asItOpens.hasProperty ("problem");
        juce::PopupMenu presets;
        presets.addItem (juce::String ("Default sound") +
                             (! measured ? " (not measured)"
                              : silent   ? " (silent until a sound is chosen in its window: Save this sound)"
                                         : ""),
                         [picked, make, asItOpens] { picked (make (asItOpens)); });
        if (found != index.instruments.end())
        {
            // Many presets: by category.
            std::map<juce::String, juce::PopupMenu> byCategory;
            for (const auto& sound : found->second.sounds)
            {
                if (sound["id"].toString().isEmpty())
                    continue; // Default sound, above
                const auto unmeasured = sound.hasProperty ("problem") || (bool)sound["silent"];
                const auto name = sound["name"].toString() + (unmeasured ? " (not measured)" : "");
                auto& into = found->second.sounds.size() > 40 ? byCategory[sound["category"].toString()] : presets;
                into.addItem (name, [picked, make, sound] { picked (make (sound)); });
            }
            for (auto& [category, sub] : byCategory)
                presets.addSubMenu (category.isEmpty() ? juce::String ("Other") : category, sub);
        }
        // The user's own role for the instrument, kept beside the sound index (it survives a new scan).
        UserRoles marked (context.indexFolder);
        marked.load();
        const auto mine = marked.get (source.key);
        juce::PopupMenu markAs;
        for (const auto& role : parts::songRoles())
            markAs.addItem (parts::roleName (role), true, mine == role,
                            [this, key = source.key, role] { markInstrument (key, role); });
        if (mine.isNotEmpty())
            markAs.addItem ("Forget it", [this, key = source.key] { markInstrument (key, {}); });
        presets.addSeparator();
        presets.addSubMenu (
            "Mark it as" + (mine.isNotEmpty() ? " (now " + parts::roleName (mine).toLowerCase() + ")" : juce::String()),
            markAs);
        menu.addSubMenu (source.name + " (" + (source.badge.isNotEmpty() ? source.badge : source.format) + ")" +
                             (measured ? "" : ", not measured") +
                             (mine.isNotEmpty() ? ", yours: " + parts::roleName (mine).toLowerCase() : juce::String()),
                         presets);
    }
    if (sources.empty())
        menu.addItem ("No instruments found (Browser > Instruments lists them)", false, false, [] {});
}

juce::String PartSoundsPanel::whySaveSoundIsOff (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int)rows.size()))
        return "There is no such part.";
    const auto& row = rows[(size_t)index];
    const auto* now = row.chosen();
    if (now == nullptr)
        return "Choose a sound for " + row.part.name +
               " first (Choose sounds, or Other sound... in its list), then Save this sound opens its "
               "instrument's window.";
    if (now->kind != parts::Choice::Kind::preset && now->kind != parts::Choice::Kind::kit)
        return now->title() + " plays samples or a loop: Save this sound keeps an instrument plugin's own sound. " +
               "Pick an instrument with Other sound... first.";
    if (now->source.key.isEmpty() || (now->source.stockType.isEmpty() && now->source.file.isEmpty()))
        return "Spinus does not know where " + now->source.name + " is: pick it again with Other sound...";
    if (busy)
        return "Spinus is busy with this list: wait until it finishes (or Stop), then Save this sound.";
    return {};
}

bool PartSoundsPanel::saveSound (int index)
{
    if (const auto why = whySaveSoundIsOff (index); why.isNotEmpty())
    {
        status.setText ("Save this sound: " + why, juce::dontSendNotification);
        return false;
    }
    context = getContext ? getContext() : Context {};
    const auto start = *rows[(size_t)index].chosen();
    juce::String problem;
    const auto opened = SaveSoundWindow::open (
        session, start, rows[(size_t)index].part.name + " (" + start.source.name + ")", this,
        [safe = juce::Component::SafePointer<PartSoundsPanel> (this), index, start] (const juce::String& name,
                                                                                     const juce::MemoryBlock& state)
        {
            if (safe != nullptr)
                safe->keepSavedSound (index, start, name, state);
        },
        problem);
    status.setText (opened ? "Set the sound up in " + start.source.name + "'s window, name it, then Save."
                           : "Save this sound: " + problem,
                    juce::dontSendNotification);
    return opened;
}

bool PartSoundsPanel::keepSavedSound (int index, const parts::Choice& start, const juce::String& name,
                                      const juce::MemoryBlock& state)
{
    if (busy || ! juce::isPositiveAndBelow (index, (int)rows.size()))
        return false;
    context = getContext ? getContext() : Context {};
    if (state.isEmpty())
    {
        fail (start.source.name + " gave Spinus nothing to keep: its sound was not saved.");
        return false;
    }
    SavedSounds saved (context.indexFolder);
    saved.load();
    const auto sound = saved.add (start.source.key, name, state);
    if (! sound)
    {
        fail ("Spinus could not keep that sound (the settings folder cannot be written).");
        return false;
    }
    busy = true;
    *stopping = false;
    updateButtons();
    const auto source = start.source;
    const auto folder = context.indexFolder;
    const auto helper = context.helper;
    auto measured = std::make_shared<juce::var>();
    runInBackground (
        "Saved " + sound->name + ": Spinus is listening to it (a few seconds, longer for a big instrument)...",
        [folder, helper, source, id = sound->id, measured] (const std::function<bool()>& shouldStop)
        {
            SoundIndex sounds (folder);
            sounds.load();
            SoundIndexer::updateInstrument (sounds, helper, source, shouldStop);
            if (const auto found = sounds.instruments.find (source.key); found != sounds.instruments.end())
                for (const auto& s : found->second.sounds)
                    if (s["id"].toString() == id)
                        *measured = s;
            return measured->isObject() && ! measured->hasProperty ("problem") ? juce::String()
                                                                               : juce::String ("not measured");
        },
        [this, index, source, sound = *sound, measured] (const juce::String& problem)
        {
            busy = false;
            const auto measuredSound = measured->isObject() ? *measured : savedSoundVar (sound);
            const bool silent = (bool)measuredSound["silent"];
            pickSound (index, choiceOf (source, measuredSound, problem.isEmpty()));
            status.setText ("Saved " + sound.name + " and chose it for " +
                                (juce::isPositiveAndBelow (index, (int)rows.size()) ? rows[(size_t)index].part.name
                                                                                    : juce::String ("the part")) +
                                (problem.isNotEmpty() ? ". Spinus could not listen to it yet; Update sound index "
                                                        "tries again."
                                 : silent ? ". It made no sound when Spinus played it: open it again and check its "
                                            "window."
                                          : ". Choose sounds and the AI writer can use it from now on."),
                            juce::dontSendNotification);
            updateButtons();
        });
    return true;
}

juce::var PartSoundsPanel::getRemembered() const
{
    juce::Array<juce::var> remembered;
    for (const auto& row : rows)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", row.part.id);
        o->setProperty ("name", row.part.name);
        o->setProperty ("role", row.part.role);
        o->setProperty ("tune", row.tune);
        if (row.picked)
            o->setProperty ("picked", parts::toVar (*row.picked));
        else if (const auto* chosen = row.chosen())
            o->setProperty ("chosen", parts::toVar (*chosen));
        remembered.add (juce::var (o));
    }
    return object ({ { "version", 1 }, { "parts", remembered } });
}

void PartSoundsPanel::restore (const juce::var& from)
{
    const auto* saved = from["parts"].getArray();
    if ((int)from["version"] != 1 || saved == nullptr || busy)
        return;
    context = getContext ? getContext() : Context {};
    std::vector<parts::SongPart> wanted;
    for (const auto& p : *saved)
        if (parts::songRoles().contains (p["role"].toString()) && p["id"].toString().isNotEmpty())
            wanted.push_back ({ p["id"].toString(), p["name"].toString(), p["role"].toString() });
    if (wanted.empty())
        return;
    setParts (wanted);
    for (size_t i = 0, j = 0; i < (size_t)saved->size() && j < rows.size(); ++i)
    {
        const auto& p = (*saved)[(int)i];
        if (! parts::songRoles().contains (p["role"].toString()) || p["id"].toString().isEmpty())
            continue;
        auto& row = rows[j++];
        row.tune = (bool)p["tune"];
        if (p["picked"].isObject())
        {
            row.picked = parts::choiceFromVar (p["picked"]);
            row.choices = { *row.picked };
        }
        else if (p["chosen"].isObject())
            row.choices = { parts::choiceFromVar (p["chosen"]) };
        row.selected = 0;
    }
    status.setText ("Your parts as you left them: Choose sounds finds sounds for the parts without one.",
                    juce::dontSendNotification);
    updateRows();
    updateButtons();
}

bool PartSoundsPanel::setTuneFor (int index, bool on)
{
    if (! juce::isPositiveAndBelow (index, (int)rows.size()))
        return false;
    rows[(size_t)index].tune = on;
    if (! on || busy || rows[(size_t)index].stem == juce::File())
        return true;
    // Tuned now, on its own.
    busy = true;
    *stopping = false;
    tuningOne = true;
    updateButtons();
    tuneNext ((size_t)index);
    return true;
}

bool PartSoundsPanel::markInstrument (const juce::String& instrument, const juce::String& role)
{
    context = getContext ? getContext() : Context {};
    UserRoles marked (context.indexFolder);
    marked.load();
    marked.set (instrument, {}, role);
    if (! marked.save())
        return false;
    status.setText (role.isEmpty() ? juce::String ("Its role is Spinus's guess again.")
                                   : "Marked as " + parts::roleName (role).toLowerCase() +
                                         ": Choose sounds and From an instrument use it.",
                    juce::dontSendNotification);
    return true;
}

void PartSoundsPanel::showOtherMenu (int index)
{
    juce::PopupMenu menu;
    fillInstrumentMenu (menu, [this, index] (const parts::Choice& c) { pickSound (index, c); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (
        juce::isPositiveAndBelow (index, (int)views.size()) ? (juce::Component*)views[(size_t)index].get() : this));
}

namespace
{
/** Whether a sound of role `says` fits a part of role `role` anyway: an effect part (a riser, impacts) plays
    whatever sound the user picks, and drums, percussion and impacts all play drum sounds. */
bool sameKind (const juce::String& says, const juce::String& role)
{
    // Kinds a sound's category does not tell apart: a "Keys" preset may be an organ or an electric piano.
    static const std::array<juce::StringArray, 3> kinds { juce::StringArray { "drums", "percussion", "impact" },
                                                          juce::StringArray { "piano", "organ", "chords" },
                                                          juce::StringArray { "pad", "strings", "choir" } };
    return role == "riser" || role == "impact" ||
           std::any_of (kinds.begin(), kinds.end(),
                        [&] (const juce::StringArray& k) { return k.contains (says) && k.contains (role); });
}
} // namespace

bool PartSoundsPanel::pickSound (int index, const parts::Choice& choice)
{
    if (busy || ! juce::isPositiveAndBelow (index, (int)rows.size()))
        return false;
    auto& row = rows[(size_t)index];
    auto c = choice;
    if (row.part.role == "drums" || row.part.role == "percussion" || row.part.role == "impact")
        c.kind = parts::Choice::Kind::kit; // a drum instrument: its notes per drum
    if (row.picked)
        row.choices.erase (row.choices.begin()); // the earlier pick
    row.picked = c;
    row.choices.insert (row.choices.begin(), c);
    row.selected = 0;
    row.tuned.reset();
    row.effects.clear();
    row.volumeDb = 0.0;
    const auto says = roleOf (c);
    row.note = "Your choice: the song plays " + c.title() + "." +
               (says.isNotEmpty() && says != row.part.role && ! sameKind (says, row.part.role)
                    ? " It is marked or named as " + parts::roleName (says).toLowerCase() + ": change the role if so."
                    : juce::String());
    updateRows();
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
    juce::String text = row.part.name + " (" + parts::roleName (row.part.role).toLowerCase() + "): ";
    const auto* c = row.chosen();
    text += c == nullptr ? juce::String ("no sound in the index fits") : c->title();
    juce::StringArray lines;
    if (c != nullptr)
        lines.add (c->reasons());
    if (row.measured > 0)
        lines.add ("Compared with " + juce::String (row.measured) + (row.target.part == "drums" ? " hits" : " notes") +
                   " of the songs' " + parts::displayName (row.target.part).toLowerCase() + " stems.");
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
    if (onChanged)
        onChanged();
}

void PartSoundsPanel::updateButtons()
{
    chooseButton.setEnabled (! busy);
    addPartButton.setEnabled (! busy && rows.size() < 16);
    suggestButton.setEnabled (! busy);
    addButton.setEnabled (! busy && ! rows.empty());
    effectsAgainButton.setEnabled (! busy && ! rows.empty());
    stopButton.setVisible (busy);
    for (auto* b : { &closerButton, &effectsButton })
        b->setEnabled (! busy);
    for (auto& view : views)
        view->updateSaveButton();
    resized();
}

void PartSoundsPanel::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (36));
    {
        auto r = area.removeFromTop (26);
        closerButton.setBounds (r.removeFromLeft (160));
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
        addPartButton.setBounds (r.removeFromLeft (100).reduced (0, 1));
        r.removeFromLeft (6);
        suggestButton.setBounds (r.removeFromLeft (120).reduced (0, 1));
        r.removeFromLeft (6);
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

#include "SongWriterPanel.h"

#include "ai/AnalysisJobs.h"
#include "ai/Proposal.h"
#include "dsp/sampler/SampleStorage.h"

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

SongWriterPanel::SongWriterPanel (Session& s, std::function<Context()> c) : session (s), getContext (std::move (c))
{
    intro.setText ("A new song in the style of the songs: sections, chords, drums, a bass, chords and a lead, "
                   "written by rules on this computer (no AI connection needed). Experimental: a sketch to change.",
                   juce::dontSendNotification);
    intro.setMinimumHorizontalScale (1.0f);
    intro.setJustificationType (juce::Justification::topLeft);
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (1.0f);
    refineButton.setTooltip ("Ask your AI connection (Options > AI) to vary the sections, the chord loops and the "
                             "melody's shape; Spinus checks what it says against the style and writes every note "
                             "itself. Off: rules only, nothing is sent anywhere");
    writeButton.setTooltip ("Write a song from the style summary");
    writeButton.onClick = [this] { write(); };
    againButton.setTooltip ("Write another song in the same style (a new random seed)");
    againButton.onClick = [this] { anotherVersion(); };
    redoButton.setTooltip ("Write one part (drums, bass, chords, lead) or one section again, keeping the rest");
    redoButton.onClick = [this] { showRedoMenu(); };
    listenButton.setTooltip ("Hear the project as it would be with the song, before applying it");
    listenButton.onClick = [this] { listen(); };
    stopButton.onClick = [this] { stop(); };
    applyButton.setTooltip ("Put the song into the project: patterns, channels, the Playlist, tempo and key. One "
                            "undo step");
    applyButton.onClick = [this] { apply(); };
    mixButton.setTooltip ("Open the automixer to balance the parts, with the first reference song as its reference");
    mixButton.onClick = [this] { mix(); };
    text.setMultiLine (true);
    text.setReadOnly (true);
    text.setScrollbarsShown (true);
    text.setCaretVisible (false);
    for (auto* component :
         std::initializer_list<juce::Component*> { &intro, &refineButton, &writeButton, &againButton, &redoButton,
                                                   &listenButton, &applyButton, &mixButton, &status, &text })
        addAndMakeVisible (component);
    addChildComponent (stopButton);
    updateButtons();
}

SongWriterPanel::~SongWriterPanel()
{
    *alive = false;
    rendering = nullptr;
    if (context.service != nullptr && job.isNotEmpty())
        context.service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

bool SongWriterPanel::aiReady() const { return context.service != nullptr && context.service->isEnabled(); }

void SongWriterPanel::run (const juce::var& params, const juce::String& words,
                           std::function<void (const juce::var&)> then)
{
    busy = true;
    status.setText (words, juce::dontSendNotification);
    updateButtons();
    ai::runJob (
        *context.service, "ai.v1.song.write", params, alive, [this] (const juce::String& started) { job = started; },
        [this, then = std::move (then)] (const juce::var& result, const juce::String& error)
        {
            job.clear();
            busy = false;
            if (error.isNotEmpty())
                status.setText ("The song could not be written: " + error, juce::dontSendNotification);
            else
                then (result);
            updateButtons();
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
    const auto seed = fixedSeed >= 0 ? fixedSeed++ : juce::Random::getSystemRandom().nextInt (0x7fffffff);
    const bool refine = refineButton.getToggleState();
    run (object ({ { "summary", context.summary }, { "seed", seed }, { "refine", refine } }),
         refine ? "Writing a song (the AI may vary its plan)..." : "Writing a song...",
         [this] (const juce::var& result)
         {
             song = result;
             whole = true;
             redoneParts.clear();
             redoneSections.clear();
             show();
             status.setText ("Written. Listen to it, write another version or redo a part, then Apply.",
                             juce::dontSendNotification);
         });
    return true;
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
    const auto what = part.isNotEmpty() ? "the " + song::displayName (part).toLowerCase() : "the " + section;
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

song::Options SongWriterPanel::optionsFor (const model::Project& project) const
{
    song::Options options;
    options.channels = song::findChannels (project);
    options.name = song::nextSongName (project);
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

juce::var SongWriterPanel::makeProposal (juce::String* error) const
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
        return song::makeProposal (song, project, optionsFor (project), error);
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
    juce::String error;
    const auto json = makeProposal (&error);
    if (! json.isObject())
    {
        status.setText (error, juce::dontSendNotification);
        return false;
    }
    auto& project = session.getProject();
    const bool wholeSong = whole || ! applied.isIn (project);
    const auto options = optionsFor (project);
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
        // The sounds chosen on the Sounds page, into the channels the song made for them (as the Sounds page
        // loads them: a plugin's own presets, samples onto pads or keys, tuned settings on top).
        parts::Plan chosen;
        std::vector<model::ChannelId> channels;
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

bool SongWriterPanel::listen()
{
    if (busy)
        return false;
    context = getContext();
    juce::String error;
    const auto json = makeProposal (&error);
    if (! json.isObject())
    {
        status.setText (error, juce::dontSendNotification);
        return false;
    }
    const auto proposal = ai::Proposal::prepare (json, session.getHistory());
    if (! proposal.canApply())
    {
        status.setText ("The song cannot be tried: " + proposal.error, juce::dontSendNotification);
        return false;
    }
    const auto folder = context.workFolder != juce::File()
                            ? context.workFolder
                            : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-song");
    folder.createDirectory();
    const auto file = folder.getChildFile ("song-preview.wav");
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav24;
    settings.tailSeconds = 2.0;
    settings.tunedAudio = session.getTuner().getRenderedFiles();
    if (session.getProjectFolder() != juce::File())
        settings.audioFolder = session.getProjectFolder().getChildFile ("audio");
    busy = true;
    status.setText ("Playing the song through the instruments to listen (as an export, not out loud yet)...",
                    juce::dontSendNotification);
    updateButtons();
    rendering = std::make_unique<engine::ExportJob> (
        session.getEdit().engine, session.getDeviceFactory(), proposal.preview,
        session.getAdapter().captureDeviceStates(), file, settings,
        [this, a = alive, file] (const engine::ExportJob::Result& result)
        {
            // The job calls back from inside itself: finish on the next turn of the message loop.
            juce::MessageManager::callAsync (
                [this, a, file, result]
                {
                    if (! *a)
                        return;
                    rendering = nullptr;
                    busy = false;
                    if (result.ok && file.existsAsFile())
                    {
                        lastPreview = file;
                        session.getSamplePreview().play (file, dsp::getAudioFormats());
                        status.setText ("Playing the song as it would be after Apply (Stop stops it).",
                                        juce::dontSendNotification);
                    }
                    else if (! result.cancelled)
                        status.setText ("The song could not be played: " + result.error, juce::dontSendNotification);
                    updateButtons();
                });
        });
    rendering->start();
    return true;
}

void SongWriterPanel::stop()
{
    if (rendering != nullptr)
    {
        rendering = nullptr; // cancels it
        busy = false;
    }
    if (context.service != nullptr && job.isNotEmpty())
        context.service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
    session.getSamplePreview().stop();
    status.setText ("Stopped.", juce::dontSendNotification);
    updateButtons();
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
    for (const auto& part : song::partNames())
        menu.addItem ("New " + song::displayName (part).toLowerCase(), [this, part] { redo (part); });
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
    lines.add (song::describe (song));
    const auto& checks = song["checks"];
    if (checks.isObject())
    {
        const auto& chords = checks["chords"];
        lines.add ({});
        lines.add ("Compared with the songs:");
        lines.add ("- tempo " + checks["tempo"]["song"].toString() + " BPM (theirs " +
                   checks["tempo"]["range"][0].toString() + " to " + checks["tempo"]["range"][1].toString() +
                   ", typically " + checks["tempo"]["style"].toString() + ")");
        lines.add ("- chords: " + juce::String (juce::roundToInt (100.0 * (double)chords["inVocabulary"])) +
                   " % from the songs' own chords; " + juce::String ((int)chords["loopsOfOneReference"]) +
                   " chord loop(s) as one of the songs plays it, the others new");
        juce::StringArray groove, reference;
        if (auto* o = checks["groove"].getDynamicObject())
            for (const auto& p : o->getProperties())
                groove.add (p.name.toString() + " " + juce::String (juce::roundToInt (100.0 * (double)p.value)) + " %");
        if (auto* o = checks["referenceGroove"].getDynamicObject())
            for (const auto& p : o->getProperties())
                reference.add (p.name.toString() + " " + juce::String (juce::roundToInt (100.0 * (double)p.value)) +
                               " %");
        lines.add ("- drums alike the songs' groove: " + groove.joinIntoString (", ") +
                   (reference.isEmpty()
                        ? juce::String()
                        : " (each song alike its own average: " + reference.joinIntoString (", ") + ")"));
        lines.add ("- notes in the key: " + juce::String (juce::roundToInt (100.0 * (double)checks["notesInKey"])) +
                   " %; fitting the chords: " +
                   juce::String (juce::roundToInt (100.0 * (double)checks["notesFitChords"])) + " %");
    }
    lines.add ({});
    const auto& project = session.getProject();
    if (whole || ! applied.isIn (project))
        lines.add ("Apply adds it as " + song::nextSongName (project) +
                   ": a pattern per section, a Playlist track, markers, the tempo and key; parts without a channel "
                   "of the Sounds page get Spinus's own instruments or the sounds chosen there.");
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
    listenButton.setEnabled (! busy && has);
    applyButton.setEnabled (! busy && has);
    mixButton.setEnabled (! busy && applied.isIn (session.getProject()));
    refineButton.setEnabled (! busy);
    stopButton.setVisible (busy || session.getSamplePreview().isPlaying());
    resized();
}

void SongWriterPanel::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (36));
    refineButton.setBounds (area.removeFromTop (26).removeFromLeft (420));
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (28);
        for (auto* b : { &writeButton, &againButton, &redoButton, &listenButton })
        {
            b->setBounds (r.removeFromLeft (b == &againButton ? 130 : b == &redoButton ? 110 : 80).reduced (0, 1));
            r.removeFromLeft (6);
        }
        if (stopButton.isVisible())
            stopButton.setBounds (r.removeFromLeft (60).reduced (0, 1));
        mixButton.setBounds (r.removeFromRight (80).reduced (0, 1));
        r.removeFromRight (6);
        applyButton.setBounds (r.removeFromRight (80).reduced (0, 1));
    }
    area.removeFromTop (4);
    status.setBounds (area.removeFromBottom (54));
    text.setBounds (area);
}

void SongWriterPanel::paint (juce::Graphics&) {}

} // namespace spinus::ui

#include "Automixer.h"

#include "Theme.h"
#include "engine/ParameterIds.h"
#include "dsp/sampler/SampleStorage.h"

namespace spinus::ui
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : properties)
        o->setProperty (key, value);
    return juce::var (o);
}

juce::String number (double v, int decimals = 1) { return juce::String (v, decimals); }

/** Any song file Spinus reads (MP3 too) as a WAV the AI service can read, at most 10 minutes. */
juce::Result decodeToWav (const juce::File& song, const juce::File& wav)
{
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (song));
    if (reader == nullptr)
        return juce::Result::fail ("Spinus cannot read " + song.getFileName() + ".");
    const auto channels = (int)juce::jmin (2u, reader->numChannels);
    const auto total = juce::jmin (reader->lengthInSamples, (juce::int64)(600.0 * reader->sampleRate));
    wav.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (wav);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                      .withSampleRate (reader->sampleRate)
                                                                      .withNumChannels (channels)
                                                                      .withBitsPerSample (24));
    if (writer == nullptr)
        return juce::Result::fail ("Spinus could not write " + wav.getFullPathName() + ".");
    juce::AudioBuffer<float> block (channels, 65536);
    for (juce::int64 at = 0; at < total; at += block.getNumSamples())
    {
        const auto count = (int)juce::jmin ((juce::int64)block.getNumSamples(), total - at);
        reader->read (&block, 0, count, at, true, channels > 1);
        writer->writeFromAudioSampleBuffer (block, 0, count);
    }
    return juce::Result::ok();
}
} // namespace

//==============================================================================
/** One change: a tick box with its title, and the reason under it. */
class Automixer::ItemRow final : public juce::Component
{
public:
    ItemRow (Automixer& owner, int index) : automixer (owner), row (index)
    {
        const auto& item = automixer.items[(size_t)row];
        tick.setButtonText (item.json["title"].toString());
        tick.setToggleState (item.ticked, juce::dontSendNotification);
        tick.onClick = [this] { automixer.setTicked (row, tick.getToggleState()); };
        reason.setText (item.json["reason"].toString(), juce::dontSendNotification);
        reason.setColour (juce::Label::textColourId, findColour (juce::Label::textColourId).withAlpha (0.75f));
        reason.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (tick);
        addAndMakeVisible (reason);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        tick.setBounds (r.removeFromTop (24));
        reason.setBounds (r.withTrimmedLeft (26));
    }

    juce::ToggleButton tick;
    juce::Label reason;

private:
    Automixer& automixer;
    int row;
};

//==============================================================================
Automixer::Automixer (Session& s, ai::AiService* a) : Panel ("automixer", "Automixer"), session (s), service (nullptr)
{
    intro.setText ("Spinus listens to each mixer insert and the whole mix, then suggests changes with a reason "
                   "each. Untick what you do not want, apply the rest, and compare before and after.",
                   juce::dontSendNotification);
    targetLabel.setText ("Loudness", juce::dontSendNotification);
    targetChoice.addItem ("Leave the loudness", 1);
    targetChoice.addItem ("Streaming (-14 LUFS)", 2);
    targetChoice.addItem ("Club (-9 LUFS)", 3);
    targetChoice.setSelectedId (1, juce::dontSendNotification);
    targetChoice.setTooltip ("Optionally a limiter on the master brings the whole mix to this loudness");
    analyzeButton.onClick = [this] { analyze ((Target)(targetChoice.getSelectedId() - 1)); };
    cancelButton.onClick = [this] { cancel(); };
    applyButton.setHelpText ("Apply the ticked changes as one undo step");
    applyButton.onClick = [this] { apply(); };
    beforeButton.setTooltip ("Hear the mix as it was (at the same loudness as after)");
    afterButton.setTooltip ("Hear the mix with the changes (at the same loudness as before)");
    beforeButton.onClick = [this] { listen (true); };
    afterButton.onClick = [this] { listen (false); };
    keepButton.setTooltip ("Done comparing: keep the changes");
    keepButton.onClick = [this] { keep(); };
    referenceButton.setTooltip ("A finished song whose tonal balance the mix should move towards");
    referenceButton.onClick = [this]
    {
        chooser = std::make_unique<core::FileDialog> ("Choose a reference song", reference,
                                                      dsp::getAudioFormats().getWildcardForAllFormats());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<Automixer> (this)] (const core::FileDialog& c)
                              {
                                  if (safe != nullptr && c.getResult().existsAsFile())
                                      safe->setReference (c.getResult());
                              });
    };
    clearReferenceButton.onClick = [this] { setReference ({}); };
    referenceName.setText ("No reference song", juce::dontSendNotification);
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);

    for (auto* c :
         std::initializer_list<juce::Component*> { &intro, &status, &note, &targetLabel, &targetChoice, &analyzeButton,
                                                   &viewport, &referenceButton, &referenceName })
        addAndMakeVisible (c);
    for (auto* c : std::initializer_list<juce::Component*> { &cancelButton, &bar, &applyButton, &beforeButton,
                                                             &afterButton, &keepButton, &clearReferenceButton })
        addChildComponent (c);
    setService (a);
    updateButtons();
}

Automixer::~Automixer()
{
    *alive = false;
    if (compareReady)
        session.getAdapter().setMonitorTrim (0.0f, session.getProject());
    setService (nullptr);
}

void Automixer::setService (ai::AiService* newService)
{
    if (service != nullptr && listener != 0)
        service->removeListener (listener);
    listener = 0;
    service = newService;
    if (service != nullptr)
        listener = service->addListener (
            [this] (const juce::String& method, const juce::var& params)
            {
                if (method != "ai.v1.job.update" || job.isEmpty() || params["job"].toString() != job)
                    return;
                if (params["kind"].toString() == "progress")
                {
                    const auto total = (double)params["total"];
                    progress = total > 0.0 ? (double)params["done"] / total : -1.0;
                    if (params["step"].toString().isNotEmpty())
                        status.setText (params["step"].toString() + "...", juce::dontSendNotification);
                }
            });
}

juce::File Automixer::getWorkFolder() const
{
    if (workFolder == juce::File())
        workFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("spinus-automixer-" + juce::Uuid().toString());
    return workFolder;
}

void Automixer::setReference (const juce::File& song)
{
    reference = song;
    referenceMeasured = juce::var();
    referenceName.setText (song.existsAsFile() ? "Tone like: " + song.getFileName() : "No reference song",
                           juce::dontSendNotification);
    updateButtons();
}

//==============================================================================
bool Automixer::analyze (Target wanted)
{
    if (busy)
        return false;
    if (service == nullptr || ! service->isEnabled())
    {
        status.setText ("The automixer measures the mix with Spinus's AI service: turn on AI in Options > AI.",
                        juce::dontSendNotification);
        return false;
    }
    keep();
    target = wanted;
    targetChoice.setSelectedId ((int)wanted + 1, juce::dontSendNotification);
    items.clear();
    updateRows();
    note.setText ({}, juce::dontSendNotification);
    busy = true;
    status.setText ("Recording each insert...", juce::dontSendNotification);
    updateButtons();
    render (true, [this] (const std::vector<juce::File>& files) { measure (files); });
    return true;
}

void Automixer::render (bool withStems, const std::function<void (const std::vector<juce::File>&)>& then)
{
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav24;
    settings.stems = withStems;
    settings.tailSeconds = 1.0;
    const auto folder = getWorkFolder().getChildFile (juce::String (juce::Time::currentTimeMillis()));
    folder.createDirectory();
    progress = 0.0;
    exporter = session.createExportJob (folder.getChildFile ("Mix"), settings,
                                        [this, a = alive, then] (const engine::ExportJob::Result& result)
                                        {
                                            if (! *a)
                                                return;
                                            // Let the job go once its callback has returned.
                                            juce::MessageManager::callAsync (
                                                [this, a, result, then]
                                                {
                                                    if (! *a)
                                                        return;
                                                    exporter = nullptr;
                                                    if (result.cancelled)
                                                        return fail ("Stopped.");
                                                    if (! result.ok)
                                                        return fail (result.error);
                                                    then (result.files);
                                                });
                                        });
    exporter->start();
}

void Automixer::measure (const std::vector<juce::File>& files)
{
    // The mix first, then one file per insert that gets a stem, in ExportJob::getStemInserts's order.
    const auto& project = session.getProject();
    const auto stemInserts = engine::ExportJob::getStemInserts (project);
    insertsByName.clear();
    juce::Array<juce::var> inserts;
    for (size_t i = 0; i < stemInserts.size() && i + 1 < files.size(); ++i)
    {
        const auto* insert = project.findInsert (stemInserts[i]);
        if (insert == nullptr)
            continue;
        auto name = insert->name.isNotEmpty() ? insert->name : "Insert " + juce::String (insert->id.value);
        if (insertsByName.contains (name) || name == "Master")
            name << " (" << juce::String (insert->id.value) << ")";
        insertsByName[name] = insert->id;
        inserts.add (object ({ { "name", name }, { "file", files[i + 1].getFullPathName() } }));
    }
    if (inserts.isEmpty())
        return fail ("Nothing plays into the mixer's inserts yet: there is no mix to balance.");
    status.setText ("Measuring the inserts...", juce::dontSendNotification);
    runJob ("ai.v1.mix.analyze", object ({ { "inserts", inserts } }),
            [this, mixFile = files.front()] (const juce::var& measured)
            {
                mix = measured;
                status.setText ("Measuring the whole mix...", juce::dontSendNotification);
                juce::Array<juce::var> whole;
                whole.add (object ({ { "name", "Master" }, { "file", mixFile.getFullPathName() } }));
                runJob ("ai.v1.mix.analyze", object ({ { "inserts", whole } }),
                        [this] (const juce::var& measuredMaster)
                        {
                            master = measuredMaster["inserts"]["Master"];
                            loudnessBefore = (float)(double)master["loudness"];
                            if (reference.existsAsFile() && referenceMeasured.isVoid())
                                return measureReference();
                            plan();
                        });
            });
}

void Automixer::measureReference()
{
    // Decoding a song takes a moment: on a thread of its own, then measured like the mix.
    status.setText ("Reading the reference song...", juce::dontSendNotification);
    const auto wav = getWorkFolder().getChildFile ("Reference.wav");
    getWorkFolder().createDirectory();
    juce::Thread::launch (
        [this, a = alive, song = reference, wav]
        {
            const auto decoded = decodeToWav (song, wav);
            juce::MessageManager::callAsync (
                [this, a, decoded, wav]
                {
                    if (! *a)
                        return;
                    if (decoded.failed())
                        return fail (decoded.getErrorMessage());
                    status.setText ("Measuring the reference song...", juce::dontSendNotification);
                    juce::Array<juce::var> files;
                    files.add (object ({ { "name", "Reference" }, { "file", wav.getFullPathName() } }));
                    runJob ("ai.v1.mix.analyze", object ({ { "inserts", files } }),
                            [this] (const juce::var& measured)
                            {
                                referenceMeasured = measured["inserts"]["Reference"];
                                plan();
                            });
                });
        });
}

void Automixer::plan()
{
    status.setText ("Planning the changes...", juce::dontSendNotification);
    const char* targets[] = { "none", "streaming", "club" };
    runJob ("ai.v1.mix.plan",
            object ({ { "mix", mix },
                      { "master", master },
                      { "target", targets[(int)target] },
                      { "reference", referenceMeasured },
                      { "review", true } }),
            [this] (const juce::var& planned)
            {
                items.clear();
                if (auto* planItems = planned["items"].getArray())
                    for (auto& item : *planItems)
                        items.push_back ({ item, true });
                updateRows();
                const auto reviewed = planned["reviewedBy"].toString();
                note.setText (reviewed.isNotEmpty() ? "Reviewed by " + reviewed + "." : planned["note"].toString(),
                              juce::dontSendNotification);
                finish (items.empty() ? "The mix measures well: nothing to change."
                                      : juce::String ((int)items.size()) +
                                            " suggested changes. Untick any you do not want, then apply.");
            });
}

void Automixer::runJob (const juce::String& method, const juce::var& params,
                        const std::function<void (const juce::var&)>& done)
{
    job = "starting";
    service->call (method, params,
                   [this, a = alive, done] (const juce::var& started, const juce::String& error)
                   {
                       if (! *a)
                           return;
                       if (error.isNotEmpty())
                           return fail (error);
                       job = started["job"].toString();
                       auto id = std::make_shared<int> (0);
                       *id = service->addListener (
                           [this, a2 = alive, id, jobId = job, done] (const juce::String& m, const juce::var& p)
                           {
                               if (! *a2 || m != "ai.v1.job.update" || p["job"].toString() != jobId)
                                   return;
                               const auto kind = p["kind"].toString();
                               if (kind != "done" && kind != "error")
                                   return;
                               juce::MessageManager::callAsync ([s = service, id] { s->removeListener (*id); });
                               job.clear();
                               if (kind == "error")
                                   return fail (p["message"].toString());
                               done (p["result"]);
                           });
                   });
}

void Automixer::cancel()
{
    if (exporter != nullptr)
        exporter->cancel();
    else if (service != nullptr && job.isNotEmpty() && job != "starting")
        service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

void Automixer::fail (const juce::String& message)
{
    job.clear();
    finish (message);
}

void Automixer::finish (const juce::String& message)
{
    busy = false;
    progress = -1.0;
    status.setText (message, juce::dontSendNotification);
    updateButtons();
}

void Automixer::setTicked (int index, bool ticked)
{
    if (juce::isPositiveAndBelow (index, (int)items.size()))
        items[(size_t)index].ticked = ticked;
    updateButtons();
}

//==============================================================================
juce::var Automixer::settingValues (const juce::String& stockType,
                                    const std::vector<std::pair<juce::String, juce::String>>& settings,
                                    juce::String& error)
{
    // The plugin's own text conversion turns "250 Hz" into its 0..1 value, on a hidden copy of the effect.
    auto& converter = converters[stockType];
    if (converter == nullptr)
        converter = std::make_unique<HiddenInstrument> (session, stockType);
    auto* instance = converter->getInstance();
    if (instance == nullptr)
    {
        error = "The " + stockType + " effect could not be loaded.";
        return {};
    }
    auto* values = new juce::DynamicObject();
    juce::var result (values);
    for (auto& [name, text] : settings)
    {
        juce::AudioProcessorParameter* found = nullptr;
        for (auto* parameter : instance->getParameters())
            if (parameter->getName (200) == name)
                found = parameter;
        if (found == nullptr)
        {
            error = stockType + " has no setting \"" + name + "\".";
            return {};
        }
        values->setProperty (engine::getParameterId (*found), juce::jlimit (0.0f, 1.0f, found->getValueForText (text)));
    }
    return result;
}

ai::Proposal Automixer::makeProposal()
{
    const auto& project = session.getProject();
    juce::Array<juce::var> steps;
    juce::String error;
    int count = 0;
    auto step = [&steps] (const juce::String& command, const juce::var& parameters)
    {
        steps.add (object ({ { "command", command }, { "parameters", parameters } }));
        return steps.size(); // its number, for "$n.device"
    };
    auto addEffect = [&] (model::InsertId insert, const juce::String& stockType,
                          const std::vector<std::pair<juce::String, juce::String>>& settings)
    {
        const auto values = settingValues (stockType, settings, error);
        if (error.isNotEmpty())
            return;
        const auto added =
            step ("insert.addEffect",
                  object ({ { "insert", (int)insert.value },
                            { "device", object ({ { "kind", "stock" }, { "stockType", stockType } }) } }));
        step ("device.setParameters",
              object ({ { "device", "$" + juce::String (added) + ".device" }, { "values", values } }));
    };

    for (auto& item : items)
    {
        if (! item.ticked)
            continue;
        const auto& json = item.json;
        const auto name = json["insert"].toString();
        const auto insertId = name == "Master"                ? model::masterInsertId
                              : insertsByName.contains (name) ? insertsByName[name]
                                                              : model::InsertId {};
        const auto* insert = project.findInsert (insertId);
        if (insert == nullptr)
            continue;
        const auto kind = json["kind"].toString();
        ++count;
        if (kind == "volume")
            step ("insert.set",
                  object ({ { "insert", (int)insertId.value },
                            { "volumeDb", juce::jlimit (-100.0, 12.0, insert->volumeDb + (double)json["db"]) } }));
        else if (kind == "pan")
            step ("insert.set", object ({ { "insert", (int)insertId.value }, { "pan", (double)json["pan"] } }));
        else if (kind == "eq")
        {
            // The plan's band types, as the Spinus EQ names them; the plan's bands go into bands 1, 2, ...
            static const std::map<juce::String, juce::String> types {
                { "Bell", "Bell" }, { "Hi-pass", "Low cut" }, { "Lo-shelf", "Low shelf" }, { "Hi-shelf", "High shelf" }
            };
            std::vector<std::pair<juce::String, juce::String>> settings;
            int band = 0;
            if (auto* bands = json["bands"].getArray())
                for (auto& b : *bands)
                {
                    const auto type = types.find (b["type"].toString());
                    if (type == types.end() || band >= 8)
                        continue;
                    const auto n = "Band " + juce::String (++band);
                    settings.push_back ({ n + " on", "On" });
                    settings.push_back ({ n + " type", type->second });
                    settings.push_back ({ n + " frequency", number ((double)b["freq"], 0) + " Hz" });
                    settings.push_back ({ n + " gain", number ((double)b["gain"]) + " dB" });
                    settings.push_back ({ n + " Q", number ((double)b["q"], 2) });
                }
            addEffect (insertId, "spinus.eq", settings);
        }
        else if (kind == "compressor")
            addEffect (insertId, "spinus.compressor",
                       { { "Threshold", number ((double)json["threshold"]) + " dB" },
                         { "Ratio", number ((double)json["ratio"], 2) },
                         { "Attack", number ((double)json["attack"]) + " ms" },
                         { "Release", number ((double)json["release"]) + " ms" },
                         { "Makeup", number ((double)json["makeup"]) + " dB" } });
        else if (kind == "limiter")
            addEffect (insertId, "spinus.limiter",
                       { { "Input gain", number ((double)json["gain"]) + " dB" },
                         { "Ceiling", number ((double)json["threshold"]) + " dB" } });
        else
            --count;
    }

    undoName = "Automixer: " + juce::String (count) + (count == 1 ? " change" : " changes");
    auto proposal =
        ai::Proposal::prepare (object ({ { "summary", undoName }, { "steps", steps } }), session.getHistory());
    proposal.undoName = undoName;
    if (error.isNotEmpty())
        proposal.error = std::move (error);
    return proposal;
}

bool Automixer::apply()
{
    if (busy || items.empty())
        return false;
    auto proposal = makeProposal();
    const auto result = proposal.apply (session.getHistory());
    if (result.failed())
    {
        status.setText (result.getErrorMessage(), juce::dontSendNotification);
        return false;
    }
    // Measure the mix after the changes, for the comparison at matched loudness.
    busy = true;
    status.setText ("Applied. Measuring the result for the comparison...", juce::dontSendNotification);
    updateButtons();
    render (false,
            [this] (const std::vector<juce::File>& files)
            {
                juce::Array<juce::var> whole;
                whole.add (object ({ { "name", "Master" }, { "file", files.front().getFullPathName() } }));
                runJob ("ai.v1.mix.analyze", object ({ { "inserts", whole } }),
                        [this] (const juce::var& measured)
                        {
                            const auto after = (float)(double)measured["inserts"]["Master"]["loudness"];
                            // The after, brought to the before's loudness while comparing.
                            matchTrim = after > -70.0f && loudnessBefore > -70.0f ? loudnessBefore - after : 0.0f;
                            compareReady = true;
                            hearingBefore = false;
                            session.getAdapter().setMonitorTrim (matchTrim, session.getProject());
                            finish ("Applied (one undo step). Compare Before and After; both play equally loud. "
                                    "Click Keep when done.");
                        });
            });
    return true;
}

bool Automixer::listen (bool before)
{
    if (! compareReady || busy || before == hearingBefore)
        return compareReady && before == hearingBefore;
    auto& history = session.getHistory();
    if (before)
    {
        // Only while the changes are the last step: anything done since would be undone too.
        if (history.getUndoDescriptions().strings.getLast() != undoName || ! history.undo())
            return false;
        session.getAdapter().setMonitorTrim (0.0f, session.getProject());
    }
    else
    {
        if (history.getRedoDescriptions()[0] != undoName || ! history.redo())
            return false;
        session.getAdapter().setMonitorTrim (matchTrim, session.getProject());
    }
    hearingBefore = before;
    updateButtons();
    return true;
}

void Automixer::keep()
{
    if (! compareReady)
        return;
    if (hearingBefore)
        listen (false);
    session.getAdapter().setMonitorTrim (0.0f, session.getProject());
    compareReady = false;
    updateButtons();
}

//==============================================================================
void Automixer::updateRows()
{
    rows.clear();
    for (size_t i = 0; i < items.size(); ++i)
    {
        rows.push_back (std::make_unique<ItemRow> (*this, (int)i));
        list.addAndMakeVisible (*rows.back());
    }
    resized();
}

void Automixer::updateButtons()
{
    const bool anyTicked = std::any_of (items.begin(), items.end(), [] (auto& i) { return i.ticked; });
    analyzeButton.setEnabled (! busy);
    targetChoice.setEnabled (! busy);
    cancelButton.setVisible (busy);
    bar.setVisible (busy);
    applyButton.setVisible (! items.empty() && ! compareReady);
    applyButton.setEnabled (! busy && anyTicked);
    for (auto* b : { &beforeButton, &afterButton, &keepButton })
        b->setVisible (compareReady);
    beforeButton.setToggleState (compareReady && hearingBefore, juce::dontSendNotification);
    afterButton.setToggleState (compareReady && ! hearingBefore, juce::dontSendNotification);
    for (auto& row : rows)
        row->tick.setEnabled (! busy && ! compareReady);
    referenceButton.setEnabled (! busy);
    clearReferenceButton.setVisible (reference.existsAsFile());
    clearReferenceButton.setEnabled (! busy);
    resized();
}

void Automixer::resized()
{
    auto area = getLocalBounds().reduced (10);
    intro.setBounds (area.removeFromTop (36));
    area.removeFromTop (6);
    {
        auto r = area.removeFromTop (28);
        targetLabel.setBounds (r.removeFromLeft (70));
        targetChoice.setBounds (r.removeFromLeft (200).reduced (0, 2));
        r.removeFromLeft (8);
        analyzeButton.setBounds (r.removeFromLeft (150).reduced (0, 1));
        if (cancelButton.isVisible())
            cancelButton.setBounds (r.removeFromRight (80).reduced (0, 1));
    }
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (28);
        r.removeFromLeft (70);
        referenceButton.setBounds (r.removeFromLeft (150).reduced (0, 1));
        r.removeFromLeft (8);
        if (clearReferenceButton.isVisible())
            clearReferenceButton.setBounds (r.removeFromRight (110).reduced (0, 1));
        referenceName.setBounds (r);
    }
    area.removeFromTop (6);
    status.setBounds (area.removeFromTop (22));
    if (bar.isVisible())
        bar.setBounds (area.removeFromTop (16));
    {
        auto bottom = area.removeFromBottom (28);
        if (applyButton.isVisible())
            applyButton.setBounds (bottom.removeFromLeft (180).reduced (0, 1));
        if (compareReady)
        {
            beforeButton.setBounds (bottom.removeFromLeft (80).reduced (0, 1));
            afterButton.setBounds (bottom.removeFromLeft (80).reduced (0, 1));
            bottom.removeFromLeft (8);
            keepButton.setBounds (bottom.removeFromLeft (80).reduced (0, 1));
        }
        area.removeFromBottom (4);
        note.setBounds (area.removeFromBottom (20));
    }
    area.removeFromTop (4);
    viewport.setBounds (area);
    const auto width = juce::jmax (100, area.getWidth() - viewport.getScrollBarThickness());
    int y = 0;
    for (auto& row : rows)
    {
        row->setBounds (0, y, width, 60); // the reason gets two lines
        y += 64;
    }
    list.setSize (width, y);
}

void Automixer::paint (juce::Graphics& g) { g.fillAll (findColour (juce::ResizableWindow::backgroundColourId)); }

} // namespace spinus::ui

#include "AnalysisEnginePage.h"

#include "Theme.h"

namespace spinus::ui
{

namespace
{
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

constexpr auto startingJob = "starting";

/** Where a build's files come from, for the question before installing. */
juce::String sourcesOf (const juce::String& vendor)
{
    if (vendor == "amd")
        return "AMD (repo.amd.com) and the Python Package Index (pypi.org)";
    return "pytorch.org and the Python Package Index (pypi.org)";
}
} // namespace

AnalysisEnginePage::AnalysisEnginePage (ai::AiService& s) : service (s)
{
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

    intro.setText ("Analyzing songs (splitting them into stems, finding the beat) uses PyTorch, which runs on this "
                   "computer: nothing is sent anywhere. PyTorch is large, so Spinus downloads it only when you ask. "
                   "It works on the CPU of any computer; a graphics card makes it faster and keeps the CPU free for "
                   "the music.",
                   juce::dontSendNotification);
    for (auto* l : std::initializer_list<juce::Label*> { &intro, &gpuText, &usesText, &statusText, &folderText })
        l->setJustificationType (juce::Justification::topLeft);
    list.setRowHeight (44);

    installButton.onClick = [this] { askToInstall (list.getSelectedRow()); };
    removeButton.onClick = [this] { remove (list.getSelectedRow()); };
    cancelButton.onClick = [this] { cancelInstall(); };
    testButton.onClick = [this] { runTest(); };
    backButton.onClick = [this]
    {
        if (onBack)
            onBack();
    };
    forceCpu.setTooltip ("Leave the graphics card alone (for games or a local AI chat model), at the cost of speed");
    forceCpu.onClick = [this] { setForceCpu (forceCpu.getToggleState()); };

    for (auto* c :
         std::initializer_list<juce::Component*> { &intro, &gpuText, &usesText, &statusText, &folderText, &list,
                                                   &installButton, &removeButton, &testButton, &backButton, &forceCpu })
        addAndMakeVisible (c);
    addChildComponent (bar);
    addChildComponent (cancelButton);

    listener = service.addListener (
        [this] (const juce::String& method, const juce::var& params)
        {
            if (method != "ai.v1.job.update" || job.isEmpty() || params["job"].toString() != job)
                return;
            const auto kind = params["kind"].toString();
            if (kind == "progress")
            {
                progress = (double)params["done"];
                if (! testing)
                    statusText.setText (params["step"].toString() + ": " +
                                            juce::String (juce::roundToInt (progress * 100)) + " %",
                                        juce::dontSendNotification);
                return;
            }
            const bool wasTest = testing;
            job.clear();
            testing = false;
            progress = -1.0;
            if (kind == "error")
                statusText.setText (params["message"].toString(), juce::dontSendNotification);
            else if (wasTest)
            {
                const auto& test = params["result"]["test"];
                const auto device =
                    test["device"].toString() == "cpu" ? juce::String ("the CPU") : test["deviceName"].toString();
                statusText.setText (
                    (test["ok"] ? "Test passed on " : "Test failed on ") + device + " (PyTorch " +
                        test["torch"].toString() + ")." +
                        (test["note"].toString().isNotEmpty() ? " " + test["note"].toString() : juce::String()),
                    juce::dontSendNotification);
            }
            else
                statusText.setText ("Installed. Spinus can analyze songs now.", juce::dontSendNotification);
            if (wasTest && params["result"]["status"].isObject())
                show (params["result"]["status"]);
            else
                refresh();
        });

    setSize (652, 560);
    refresh();
}

AnalysisEnginePage::~AnalysisEnginePage()
{
    *alive = false;
    service.removeListener (listener);
    list.setModel (nullptr);
}

juce::String AnalysisEnginePage::formatSize (juce::int64 bytes)
{
    if (bytes >= 1'000'000'000)
        return juce::String ((double)bytes / 1.0e9, 1) + " GB";
    return juce::String (juce::roundToInt ((double)bytes / 1.0e6)) + " MB";
}

void AnalysisEnginePage::call (const juce::String& method, const juce::var& params,
                               std::function<void (const juce::var&)> onResult)
{
    ++waiting;
    updateButtons();
    service.call (
        method, params,
        [this, a = alive, onResult = std::move (onResult)] (const juce::var& result, const juce::String& error)
        {
            if (! *a)
                return;
            --waiting;
            if (error.isNotEmpty())
            {
                statusText.setText (error, juce::dontSendNotification);
                if (job == startingJob)
                {
                    job.clear();
                    testing = false;
                }
            }
            else if (onResult)
                onResult (result);
            updateButtons();
        });
}

void AnalysisEnginePage::refresh()
{
    call ("ai.v1.compute.status", {}, [this] (const juce::var& status) { show (status); });
}

void AnalysisEnginePage::show (const juce::var& status)
{
    const auto selected = list.getSelectedRow();
    builds.clear();
    if (auto* items = status["builds"].getArray())
        for (auto& b : *items)
        {
            BuildInfo info;
            info.id = b["id"];
            info.label = b["label"];
            info.vendor = b["vendor"];
            info.download = (juce::int64)b["download"];
            info.disk = (juce::int64)b["disk"];
            info.size = (juce::int64)b["size"];
            info.tested = b["tested"];
            info.installed = b["installed"];
            info.outdated = b["outdated"];
            info.installing = b["installing"];
            builds.push_back (info);
        }
    hasUv = status["uv"];
    forceCpu.setToggleState (status["forceCpu"], juce::dontSendNotification);

    // The graphics card, and why it cannot be used when that is so.
    juce::StringArray lines;
    if (auto* gpus = status["gpus"].getArray(); gpus != nullptr && ! gpus->isEmpty())
        for (auto& g : *gpus)
        {
            lines.add ("Graphics card: " + g["name"].toString() +
                       (g["target"].toString().isNotEmpty() && g["vendor"].toString() == "amd"
                            ? " (" + g["target"].toString() + ")"
                            : juce::String()));
            if (auto* notes = g["notes"].getArray())
                for (auto& n : *notes)
                    lines.add (n.toString());
        }
    else
        lines.add (
            "No graphics card was found that PyTorch could use, so the CPU does the work: slower, but it works.");
    if (status["suggested"].toString() == "cpu" && status["reason"].toString().isNotEmpty())
        lines.add (status["reason"].toString() + " The CPU does the work.");
    gpuText.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);

    const auto& uses = status["uses"];
    if (! uses.isObject())
        usesText.setText ("Not installed yet. Choose a build below and click Install.", juce::dontSendNotification);
    else
    {
        const auto build =
            std::find_if (builds.begin(), builds.end(), [&] (auto& b) { return b.id == uses["build"].toString(); });
        const auto name = build != builds.end() ? build->label : uses["build"].toString();
        const auto onGpu = uses["device"].toString() != "cpu";
        usesText.setText ("In use: " + name + (onGpu ? ", on the graphics card." : ", on the CPU.") +
                              (uses["note"].toString().isNotEmpty() ? "\n" + uses["note"].toString() : juce::String()),
                          juce::dontSendNotification);
    }
    folderText.setText ("Kept in " + status["folder"].toString() + ", shared by all projects.",
                        juce::dontSendNotification);
    if (! hasUv)
        statusText.setText ("Installing needs the uv tool: sudo pacman -S uv", juce::dontSendNotification);

    // Reopened while an installation goes on: follow it.
    if (job.isEmpty() && status["installJob"].toString().isNotEmpty())
    {
        job = status["installJob"].toString();
        testing = false;
        progress = 0.0;
        statusText.setText ("Installing...", juce::dontSendNotification);
    }

    list.updateContent();
    list.selectRow (juce::jlimit (0, juce::jmax (0, (int)builds.size() - 1), selected));
    list.repaint();
    updateButtons();
}

juce::String AnalysisEnginePage::getButtonText (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int)builds.size()))
        return "Install";
    const auto& b = builds[(size_t)row];
    if (b.installed)
        return b.outdated ? "Update" : "Installed";
    return "Install (" + formatSize (b.download) + ")";
}

void AnalysisEnginePage::updateButtons()
{
    const auto row = list.getSelectedRow();
    const bool valid = juce::isPositiveAndBelow (row, (int)builds.size());
    const bool busy = job.isNotEmpty();
    installButton.setButtonText (getButtonText (row));
    installButton.setEnabled (valid && hasUv && ! busy &&
                              (! builds[(size_t)row].installed || builds[(size_t)row].outdated));
    removeButton.setEnabled (valid && ! busy && builds[(size_t)row].installed);
    testButton.setEnabled (! busy && std::any_of (builds.begin(), builds.end(), [] (auto& b) { return b.installed; }));
    forceCpu.setEnabled (! busy);
    bar.setVisible (isInstalling());
    cancelButton.setVisible (isInstalling());
    resized();
}

void AnalysisEnginePage::askToInstall (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)builds.size()) || job.isNotEmpty())
        return;
    const auto& b = builds[(size_t)row];
    auto message = "Spinus will download about " + formatSize (b.download) + " from " + sourcesOf (b.vendor) +
                   ". Installed, it takes about " + formatSize (b.disk) +
                   " of disk space. You can remove it here at any time.";
    if (b.vendor == "nvidia")
        message << "\n\nNvidia's build includes Nvidia's own CUDA libraries, which are not open source.";
    if (! b.tested)
        message << "\n\nThis build has not been tried on this kind of graphics card yet. It should work; if it does "
                   "not, Spinus uses the CPU instead.";
    messageBox (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon,
                                                              "Install " + b.label + "?", message, "Install", "Cancel"),
                [safe = juce::Component::SafePointer (this), row] (int choice)
                {
                    if (safe != nullptr && choice == 0)
                        safe->install (row);
                });
}

void AnalysisEnginePage::install (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)builds.size()) || job.isNotEmpty())
        return;
    job = startingJob;
    testing = false;
    progress = 0.0;
    statusText.setText ("Installing " + builds[(size_t)row].label + "...", juce::dontSendNotification);
    call ("ai.v1.compute.install", object ({ { "build", builds[(size_t)row].id } }),
          [this] (const juce::var& result)
          {
              job = result["job"].toString();
              refresh();
          });
}

void AnalysisEnginePage::cancelInstall()
{
    if (isInstalling() && job != startingJob)
        service.call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

void AnalysisEnginePage::remove (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)builds.size()) || job.isNotEmpty())
        return;
    const auto label = builds[(size_t)row].label;
    call ("ai.v1.compute.remove", object ({ { "build", builds[(size_t)row].id } }),
          [this, label] (const juce::var& status)
          {
              statusText.setText ("Removed " + label + ".", juce::dontSendNotification);
              show (status);
          });
}

void AnalysisEnginePage::setForceCpu (bool on)
{
    call ("ai.v1.compute.setForceCpu", object ({ { "on", on } }), [this] (const juce::var& status) { show (status); });
}

void AnalysisEnginePage::runTest()
{
    if (job.isNotEmpty())
        return;
    job = startingJob;
    testing = true;
    statusText.setText ("Testing (the first time on a graphics card takes a little longer)...",
                        juce::dontSendNotification);
    call ("ai.v1.compute.test", {}, [this] (const juce::var& result) { job = result["job"].toString(); });
}

//==============================================================================
void AnalysisEnginePage::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int)builds.size()))
        return;
    const auto& theme = themeOf (*this);
    const auto& b = builds[(size_t)row];
    if (selected)
        g.fillAll (theme.get ("accent").withAlpha (0.25f));
    g.setColour (theme.get ("text"));
    g.setFont (juce::FontOptions (14.0f));
    const auto state = b.installing  ? juce::String ("Installing")
                       : b.outdated  ? juce::String ("Installed, an update is available")
                       : b.installed ? "Installed (" + formatSize (b.size) + ")"
                                     : juce::String ("Not installed");
    g.drawText (b.label, 10, 2, width - 20, height / 2, juce::Justification::bottomLeft, true);
    g.setColour (theme.get ("textDim"));
    g.drawText (state, 10, 2, width - 20, height / 2, juce::Justification::bottomRight, true);
    g.setFont (juce::FontOptions (11.0f));
    const auto tested = b.tested ? juce::String ("Tested") : juce::String ("Untested: should work, not tried yet");
    g.drawText (tested + " - download " + formatSize (b.download) + ", " + formatSize (b.disk) + " on disk", 10,
                height / 2, width - 20, height / 2 - 2, juce::Justification::topLeft, true);
}

void AnalysisEnginePage::paint (juce::Graphics&)
{
    const auto& theme = themeOf (*this);
    for (auto* l : { &intro, &gpuText, &usesText })
        l->setColour (juce::Label::textColourId, theme.get ("text"));
    for (auto* l : std::initializer_list<juce::Label*> { &statusText, &folderText })
        l->setColour (juce::Label::textColourId, theme.get ("textDim"));
    forceCpu.setColour (juce::ToggleButton::textColourId, theme.get ("text"));
}

void AnalysisEnginePage::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (72));
    area.removeFromTop (4);
    gpuText.setBounds (area.removeFromTop (gpuText.getText().containsChar ('\n') ? 54 : 36));
    usesText.setBounds (area.removeFromTop (usesText.getText().containsChar ('\n') ? 54 : 36));

    auto bottom = area.removeFromBottom (32);
    backButton.setBounds (bottom.removeFromRight (80));
    forceCpu.setBounds (bottom.removeFromLeft (180));
    area.removeFromBottom (4);
    folderText.setBounds (area.removeFromBottom (22));
    statusText.setBounds (area.removeFromBottom (52));
    if (bar.isVisible())
    {
        auto row = area.removeFromBottom (26);
        cancelButton.setBounds (row.removeFromRight (80));
        row.removeFromRight (6);
        bar.setBounds (row);
        area.removeFromBottom (6);
    }
    auto buttons = area.removeFromBottom (32);
    installButton.setBounds (buttons.removeFromLeft (150));
    buttons.removeFromLeft (8);
    removeButton.setBounds (buttons.removeFromLeft (90));
    testButton.setBounds (buttons.removeFromRight (90));
    area.removeFromBottom (6);
    list.setBounds (area);
}

} // namespace spinus::ui

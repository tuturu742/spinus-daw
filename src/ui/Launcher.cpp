#include "Launcher.h"

#include "AskForName.h"

#include "Theme.h"
#include "commands/CommandRegistry.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "project/AudioImport.h"
#include "project/ProjectFiles.h"

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

using LaunchState = engine::EngineAdapter::LaunchState;

} // namespace

//==============================================================================
/** The grid itself: track names along the top, scenes down the left, stop buttons along the bottom. */
class Launcher::Grid final : public juce::Component
{
public:
    explicit Grid (Launcher& l) : owner (l) { setRepaintsOnMouseActivity (false); }

    void updateSize()
    {
        setSize (sceneWidth + owner.getNumColumns() * cellWidth + 1,
                 headerHeight + owner.getNumRows() * cellHeight + footerHeight + 1);
    }

    std::optional<std::pair<int, int>> slotAt (juce::Point<int> p) const
    {
        if (p.x < sceneWidth || p.y < headerHeight)
            return std::nullopt;
        const int column = (p.x - sceneWidth) / cellWidth, row = (p.y - headerHeight) / cellHeight;
        if (column >= owner.getNumColumns() || row >= owner.getNumRows())
            return std::nullopt;
        return std::pair { column, row };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const auto& launcher = owner.session.getProject().launcher;
        auto& adapter = owner.session.getAdapter();
        g.fillAll (theme.get ("panel"));
        g.setFont (juce::FontOptions (13.0f));

        // Track names.
        g.setColour (theme.get ("panelHeader"));
        g.fillRect (0, 0, getWidth(), headerHeight);
        for (int column = 0; column < owner.getNumColumns(); ++column)
        {
            const juce::Rectangle<int> r { sceneWidth + column * cellWidth, 0, cellWidth, headerHeight };
            const bool real = column < (int)launcher.tracks.size();
            if (real)
            {
                g.setColour (juce::Colour (launcher.tracks[(size_t)column].colour));
                g.fillRect (r.withTop (r.getBottom() - 3).reduced (2, 0));
            }
            g.setColour (real ? theme.get ("text") : theme.get ("textDim").withAlpha (0.4f));
            g.drawFittedText (real ? launcher.tracks[(size_t)column].name
                                   : juce::String ("Track ") + juce::String (column + 1),
                              r.reduced (6, 0), juce::Justification::centredLeft, 1);
        }

        // Scenes: a launch triangle and the name.
        for (int row = 0; row < owner.getNumRows(); ++row)
        {
            juce::Rectangle<int> r { 0, headerHeight + row * cellHeight, sceneWidth, cellHeight };
            const bool real = row < (int)launcher.scenes.size();
            g.setColour (theme.get ("panelHeader"));
            g.fillRect (r.reduced (0, 1));
            auto play = r.removeFromLeft (28).reduced (8, 10).toFloat();
            juce::Path triangle;
            triangle.addTriangle (play.getTopLeft(), play.getBottomLeft(), { play.getRight(), play.getCentreY() });
            g.setColour (real ? theme.get ("accent") : theme.get ("textDim").withAlpha (0.3f));
            g.fillPath (triangle);
            g.setColour (real ? theme.get ("text") : theme.get ("textDim").withAlpha (0.4f));
            g.drawFittedText (real ? launcher.scenes[(size_t)row].name
                                   : juce::String ("Scene ") + juce::String (row + 1),
                              r.reduced (2, 0), juce::Justification::centredLeft, 1);
        }

        // Slots.
        const auto now = juce::Time::getMillisecondCounter();
        for (int column = 0; column < owner.getNumColumns(); ++column)
            for (int row = 0; row < owner.getNumRows(); ++row)
            {
                auto r = owner.getSlotBounds (column, row).reduced (2);
                const auto* clip = owner.getClip (column, row);
                if (clip == nullptr)
                {
                    g.setColour (theme.get ("border").withAlpha (0.5f));
                    g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.0f);
                }
                else
                {
                    const auto* pc = std::get_if<model::PatternClip> (&clip->content);
                    const auto* pattern =
                        pc != nullptr ? owner.session.getProject().findPattern (pc->pattern) : nullptr;
                    const auto colour = pattern != nullptr ? juce::Colour (pattern->colour) : theme.get ("accentAlt");
                    const auto state = adapter.getLaunchState (clip->id);
                    g.setColour (colour.withAlpha (state == LaunchState::stopped ? 0.55f : 0.9f));
                    g.fillRoundedRectangle (r.toFloat(), 3.0f);

                    // Starting blinks; playing shows how far through its pass it is.
                    if (state == LaunchState::starting || state == LaunchState::stopping)
                    {
                        g.setColour (theme.get ("text").withAlpha ((now / 250) % 2 == 0 ? 0.9f : 0.2f));
                        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 2.0f);
                    }
                    if (const auto progress = adapter.getLaunchProgress (clip->id))
                    {
                        g.setColour (theme.get ("text").withAlpha (0.85f));
                        g.fillRect (r.withTop (r.getBottom() - 3).withWidth ((int)(r.getWidth() * *progress)));
                    }

                    auto text = r.reduced (4, 2);
                    auto icon = text.removeFromLeft (12).toFloat().withSizeKeepingCentre (8.0f, 9.0f);
                    juce::Path triangle;
                    triangle.addTriangle (icon.getTopLeft(), icon.getBottomLeft(),
                                          { icon.getRight(), icon.getCentreY() });
                    g.setColour (juce::Colours::black.withAlpha (0.7f));
                    g.fillPath (triangle);
                    g.drawFittedText (owner.nameOf (*clip) + (clip->loop ? "" : " (once)"), text,
                                      juce::Justification::centredLeft, 1);
                }

                if (owner.selected == std::pair { column, row } && owner.hasKeyboardFocus (true))
                {
                    g.setColour (theme.get ("accent"));
                    g.drawRoundedRectangle (r.toFloat().expanded (1.0f), 3.0f, 1.5f);
                }
            }

        // Stop buttons.
        const auto footer = headerHeight + owner.getNumRows() * cellHeight;
        g.setColour (theme.get ("panelHeader"));
        g.fillRect (0, footer, getWidth(), footerHeight);
        for (int column = 0; column < owner.getNumColumns(); ++column)
        {
            const auto square = juce::Rectangle<int> (sceneWidth + column * cellWidth, footer, cellWidth, footerHeight)
                                    .withSizeKeepingCentre (10, 10);
            g.setColour (column < (int)launcher.tracks.size() ? theme.get ("textDim")
                                                              : theme.get ("textDim").withAlpha (0.3f));
            g.fillRect (square);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.grabKeyboardFocus();
        dragFrom.reset();
        const auto p = e.getPosition();
        const auto& launcher = owner.session.getProject().launcher;

        if (p.y >= headerHeight + owner.getNumRows() * cellHeight) // stop buttons
        {
            if (p.x >= sceneWidth)
                owner.stopColumn ((p.x - sceneWidth) / cellWidth);
            return;
        }
        if (p.y < headerHeight) // track names
        {
            const int column = (p.x - sceneWidth) / cellWidth;
            if (p.x >= sceneWidth && column < (int)launcher.tracks.size() && e.mods.isPopupMenu())
                showTrackMenu (column);
            return;
        }
        if (p.x < sceneWidth) // scenes
        {
            const int row = (p.y - headerHeight) / cellHeight;
            if (e.mods.isPopupMenu())
            {
                if (row < (int)launcher.scenes.size())
                    showSceneMenu (row);
            }
            else if (p.x < 28)
                owner.launchScene (row);
            return;
        }

        if (auto slot = slotAt (p))
        {
            owner.selectSlot (slot->first, slot->second);
            const auto* clip = owner.getClip (slot->first, slot->second);
            if (e.mods.isPopupMenu())
            {
                if (clip != nullptr)
                    showClipMenu (*clip, *slot);
                return;
            }
            if (clip == nullptr)
            {
                owner.placePattern (slot->first, slot->second, owner.session.getCurrentPattern());
                return;
            }
            // As on the Playlist: clicking a pattern clip makes its pattern the current one (and a click
            // still launches it).
            if (auto* pc = std::get_if<model::PatternClip> (&clip->content))
                owner.session.setCurrentPattern (pc->pattern);
            if (e.getNumberOfClicks() > 1)
                return;
            dragFrom = std::move (slot);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragFrom && e.getDistanceFromDragStart() > 4)
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        if (! dragFrom)
            return;
        const auto from = *dragFrom;
        dragFrom.reset();
        const auto* clip = owner.getClip (from.first, from.second);
        if (clip == nullptr)
            return;
        if (e.getDistanceFromDragStart() <= 4)
        {
            owner.launch (from.first, from.second);
            return;
        }
        if (auto to = slotAt (e.getPosition()); to && *to != from)
            owner.moveClip (clip->id, to->first, to->second);
    }

private:
    void showClipMenu (const model::LauncherClip& clip, std::pair<int, int> slot)
    {
        const auto id = clip.id;
        juce::PopupMenu lengths;
        lengths.addItem (std::holds_alternative<model::PatternClip> (clip.content) ? "The pattern's length"
                                                                                   : "The file's length",
                         [this, id] { owner.setLengthInBars (id, 0); });
        for (int bars : { 1, 2, 4, 8, 16 })
            lengths.addItem (juce::String (bars) + (bars == 1 ? " bar" : " bars"),
                             [this, id, bars] { owner.setLengthInBars (id, bars); });
        juce::PopupMenu menu;
        menu.addItem ("Launch", [this, slot] { owner.launch (slot.first, slot.second); });
        menu.addItem ("Loop", true, clip.loop, [this, id, loop = clip.loop] { owner.setLoop (id, ! loop); });
        menu.addSubMenu ("Length", lengths);
        menu.addSeparator();
        menu.addItem ("Remove", [this, id] { owner.removeClip (id); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showTrackMenu (int column)
    {
        const auto name = owner.session.getProject().launcher.tracks[(size_t)column].name;
        juce::PopupMenu menu;
        menu.addItem ("Rename...",
                      [this, column, name]
                      {
                          askForName ("Rename launcher track", name,
                                      [this, column] (const juce::String& n) { owner.renameTrack (column, n); });
                      });
        menu.addItem ("Remove the track and its clips", [this, column] { owner.removeTrack (column); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showSceneMenu (int row)
    {
        const auto name = owner.session.getProject().launcher.scenes[(size_t)row].name;
        juce::PopupMenu menu;
        menu.addItem ("Launch", [this, row] { owner.launchScene (row); });
        menu.addItem ("Rename...",
                      [this, row, name]
                      {
                          askForName ("Rename scene", name,
                                      [this, row] (const juce::String& n) { owner.renameScene (row, n); });
                      });
        menu.addItem ("Remove the scene and its clips", [this, row] { owner.removeScene (row); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    Launcher& owner;
    std::optional<std::pair<int, int>> dragFrom;
};

//==============================================================================
Launcher::Launcher (Session& s) : Panel ("launcher", "Launcher"), session (s)
{
    grid = std::make_unique<Grid> (*this);
    viewport.setViewedComponent (grid.get(), false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    launchLabel.setText ("Launch", juce::dontSendNotification);
    quantize.addItem ("On the next bar", 1 + (int)model::LaunchQuantize::bar);
    quantize.addItem ("On the next beat", 1 + (int)model::LaunchQuantize::beat);
    quantize.addItem ("At once", 1 + (int)model::LaunchQuantize::none);
    quantize.setTooltip ("When a launched clip starts (and a stopped one stops)");
    quantize.onChange = [this] { setQuantize ((model::LaunchQuantize)(quantize.getSelectedId() - 1)); };
    stopAllButton.setTooltip ("Stop every launched clip (on the next bar or beat)");
    stopAllButton.onClick = [this] { session.stopLauncher(); };
    backButton.setTooltip ("Stop the launcher at once: every channel plays the Playlist again");
    backButton.onClick = [this] { session.backToPlaylist(); };
    recordButton.setClickingTogglesState (true);
    recordButton.setTooltip ("Record what you launch, then write it into the Playlist when playback stops "
                             "(song mode; one undo step)");
    recordButton.onClick = [this] { session.setLauncherRecording (recordButton.getToggleState()); };
    status.setText ("Click an empty slot to put the current pattern there, or drop audio files on it. Click a "
                    "clip to launch it.",
                    juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &launchLabel, &quantize, &stopAllButton, &backButton,
                                                             &recordButton, &status })
        addAndMakeVisible (c);

    setWantsKeyboardFocus (true);
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    session.addChangeListener (this);
    handleAsyncUpdate();
    startTimerHz (30);
}

Launcher::~Launcher()
{
    session.removeChangeListener (this);
    session.getHistory().removeListener (historyListener);
}

juce::Component& Launcher::getGrid() noexcept { return *grid; }

//==============================================================================
const model::LauncherClip* Launcher::getClip (int column, int row) const
{
    const auto& launcher = session.getProject().launcher;
    if (column < 0 || row < 0 || column >= (int)launcher.tracks.size() || row >= (int)launcher.scenes.size())
        return nullptr;
    return launcher.findClip (launcher.tracks[(size_t)column].id, launcher.scenes[(size_t)row].id);
}

int Launcher::getNumColumns() const
{
    // One empty column to grow into, as the Playlist keeps empty tracks below the last.
    return juce::jmax (minimumColumns, (int)session.getProject().launcher.tracks.size() + 1);
}

int Launcher::getNumRows() const
{
    return juce::jmax (minimumRows, (int)session.getProject().launcher.scenes.size() + 1);
}

juce::Rectangle<int> Launcher::getSlotBounds (int column, int row) const
{
    return { sceneWidth + column * cellWidth, headerHeight + row * cellHeight, cellWidth, cellHeight };
}

juce::String Launcher::nameOf (const model::LauncherClip& clip) const
{
    if (auto* pc = std::get_if<model::PatternClip> (&clip.content))
    {
        const auto* pattern = session.getProject().findPattern (pc->pattern);
        return pattern != nullptr ? pattern->name : juce::String ("?");
    }
    return juce::File::createFileWithoutCheckingPath (std::get<model::AudioClip> (clip.content).file)
        .getFileNameWithoutExtension();
}

//==============================================================================
bool Launcher::perform (const juce::String& command, const juce::var& parameters)
{
    juce::String error;
    auto c = commands::CommandRegistry::get().create (command, parameters, error);
    if (c == nullptr)
        return false;
    const auto result = session.getHistory().perform (std::move (c));
    if (result.failed())
        say (result.getErrorMessage());
    return result.wasOk();
}

model::LauncherTrackId Launcher::ensureTrack (int column)
{
    const auto& tracks = session.getProject().launcher.tracks;
    if (column < (int)tracks.size())
        return tracks[(size_t)column].id;

    // Columns after the last track are empty ones waiting to be used: create them (one undo step).
    juce::Array<juce::var> steps;
    for (auto i = (int)tracks.size(); i <= column; ++i)
        steps.add (object ({ { "command", "launcher.addTrack" }, { "parameters", object ({}) } }));
    juce::String error;
    auto transaction = commands::CommandRegistry::get().createTransaction (
        object ({ { "description", "Add launcher tracks" }, { "commands", steps } }), error);
    if (transaction == nullptr || session.getHistory().perform (std::move (transaction)).failed())
        return {};
    return session.getProject().launcher.tracks[(size_t)column].id;
}

model::SceneId Launcher::ensureScene (int row)
{
    const auto& scenes = session.getProject().launcher.scenes;
    if (row < (int)scenes.size())
        return scenes[(size_t)row].id;

    juce::Array<juce::var> steps;
    for (auto i = (int)scenes.size(); i <= row; ++i)
        steps.add (object ({ { "command", "launcher.addScene" }, { "parameters", object ({}) } }));
    juce::String error;
    auto transaction = commands::CommandRegistry::get().createTransaction (
        object ({ { "description", "Add scenes" }, { "commands", steps } }), error);
    if (transaction == nullptr || session.getHistory().perform (std::move (transaction)).failed())
        return {};
    return session.getProject().launcher.scenes[(size_t)row].id;
}

bool Launcher::placePattern (int column, int row, model::PatternId pattern)
{
    if (session.getProject().findPattern (pattern) == nullptr)
    {
        say ("Make a pattern first (in the Channel Rack), then click a slot to put it there.");
        return false;
    }
    const auto track = ensureTrack (column);
    const auto scene = ensureScene (row);
    if (! track.isValid() || ! scene.isValid())
        return false;
    return perform (
        "launcher.addPattern",
        object ({ { "track", (int)track.value }, { "scene", (int)scene.value }, { "pattern", (int)pattern.value } }));
}

bool Launcher::addAudioFile (int column, int row, const juce::File& file)
{
    // Into the project first (decoded to FLAC if the engine cannot play it as it is).
    juce::String path;
    const auto folder = session.getProjectFolder();
    if (! project::isImportableAudio (file))
        return false;
    if (auto result = project::importAudio (file, folder, path); result.failed())
    {
        say (result.getErrorMessage());
        return false;
    }
    const auto imported = folder != juce::File() && ! juce::File::isAbsolutePath (path)
                              ? folder.getChildFile ("audio").getChildFile (path)
                              : juce::File (path);
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::SamplerProcessor::getFormats().createReaderFor (imported));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return false;

    // One pass: the file's length at the project tempo, to the nearest beat so loops stay in time.
    const auto seconds = (double)reader->lengthInSamples / reader->sampleRate;
    const auto beat = session.getProject().settings.timeSignature.beatTicks();
    const auto ticks = seconds * session.getProject().settings.tempoBpm / 60.0 * (double)model::ticksPerQuarterNote;
    const auto length = juce::jmax<model::Ticks> (beat, (model::Ticks)std::llround (ticks / (double)beat) * beat);

    const auto track = ensureTrack (column);
    const auto scene = ensureScene (row);
    if (! track.isValid() || ! scene.isValid())
        return false;
    return perform ("launcher.addAudio", object ({ { "track", (int)track.value },
                                                   { "scene", (int)scene.value },
                                                   { "file", path },
                                                   { "length", (juce::int64)length } }));
}

bool Launcher::moveClip (model::ClipId clip, int column, int row)
{
    const auto track = ensureTrack (column);
    const auto scene = ensureScene (row);
    if (! track.isValid() || ! scene.isValid())
        return false;
    return perform (
        "launcher.setClip",
        object ({ { "clip", (int)clip.value }, { "track", (int)track.value }, { "scene", (int)scene.value } }));
}

bool Launcher::removeClip (model::ClipId clip)
{
    return perform ("launcher.removeClip", object ({ { "clip", (int)clip.value } }));
}

bool Launcher::setLoop (model::ClipId clip, bool loop)
{
    return perform ("launcher.setClip", object ({ { "clip", (int)clip.value }, { "loop", loop } }));
}

bool Launcher::setLengthInBars (model::ClipId id, int bars)
{
    const auto& project = session.getProject();
    const auto* clip = project.launcher.findClip (id);
    if (clip == nullptr)
        return false;
    model::Ticks length = 0;
    if (bars > 0)
        length = bars * project.settings.timeSignature.barTicks();
    else if (auto* pc = std::get_if<model::PatternClip> (&clip->content))
        length = project.findPattern (pc->pattern) != nullptr ? project.findPattern (pc->pattern)->length : 0;
    else
    {
        const auto& audio = std::get<model::AudioClip> (clip->content);
        std::unique_ptr<juce::AudioFormatReader> reader (
            dsp::SamplerProcessor::getFormats().createReaderFor (session.resolveAudioFile (audio.file)));
        if (reader != nullptr && reader->sampleRate > 0.0)
        {
            const auto beat = project.settings.timeSignature.beatTicks();
            const auto ticks = (double)reader->lengthInSamples / reader->sampleRate * project.settings.tempoBpm / 60.0 *
                               (double)model::ticksPerQuarterNote;
            length = juce::jmax<model::Ticks> (beat, (model::Ticks)std::llround (ticks / (double)beat) * beat);
        }
    }
    if (length <= 0)
        return false;
    return perform ("launcher.setClip", object ({ { "clip", (int)id.value }, { "length", (juce::int64)length } }));
}

bool Launcher::setQuantize (model::LaunchQuantize q)
{
    if (session.getProject().launcher.quantize == q)
        return true;
    const char* names[] = { "none", "beat", "bar" };
    return perform ("launcher.setQuantize", object ({ { "quantize", names[(int)q] } }));
}

bool Launcher::renameTrack (int column, const juce::String& name)
{
    const auto& tracks = session.getProject().launcher.tracks;
    return column >= 0 && column < (int)tracks.size() &&
           perform ("launcher.setTrack",
                    object ({ { "track", (int)tracks[(size_t)column].id.value }, { "name", name } }));
}

bool Launcher::renameScene (int row, const juce::String& name)
{
    const auto& scenes = session.getProject().launcher.scenes;
    return row >= 0 && row < (int)scenes.size() &&
           perform ("launcher.setScene", object ({ { "scene", (int)scenes[(size_t)row].id.value }, { "name", name } }));
}

bool Launcher::removeTrack (int column)
{
    const auto& tracks = session.getProject().launcher.tracks;
    return column >= 0 && column < (int)tracks.size() &&
           perform ("launcher.removeTrack", object ({ { "track", (int)tracks[(size_t)column].id.value } }));
}

bool Launcher::removeScene (int row)
{
    const auto& scenes = session.getProject().launcher.scenes;
    return row >= 0 && row < (int)scenes.size() &&
           perform ("launcher.removeScene", object ({ { "scene", (int)scenes[(size_t)row].id.value } }));
}

//==============================================================================
bool Launcher::launch (int column, int row)
{
    const auto* clip = getClip (column, row);
    if (clip == nullptr)
        return false;
    if (! session.launchClip (clip->id))
    {
        say ("Nothing to play in " + nameOf (*clip).quoted() +
             ": its pattern is empty, its file is missing, or its channel is frozen.");
        return false;
    }
    grid->repaint();
    return true;
}

void Launcher::launchScene (int row)
{
    const auto& scenes = session.getProject().launcher.scenes;
    if (row >= 0 && row < (int)scenes.size())
        session.launchScene (scenes[(size_t)row].id);
    grid->repaint();
}

void Launcher::stopColumn (int column)
{
    const auto& tracks = session.getProject().launcher.tracks;
    if (column >= 0 && column < (int)tracks.size())
        session.stopLauncherTrack (tracks[(size_t)column].id);
    grid->repaint();
}

void Launcher::selectSlot (int column, int row)
{
    selected = { juce::jlimit (0, getNumColumns() - 1, column), juce::jlimit (0, getNumRows() - 1, row) };
    const auto bounds = getSlotBounds (selected.first, selected.second);
    auto view = viewport.getViewArea();
    if (! view.contains (bounds))
        viewport.setViewPosition (
            juce::jlimit (0, juce::jmax (0, grid->getWidth() - view.getWidth()), bounds.getX() - sceneWidth),
            juce::jlimit (0, juce::jmax (0, grid->getHeight() - view.getHeight()), bounds.getY() - headerHeight));
    grid->repaint();
}

bool Launcher::keyPressed (const juce::KeyPress& key)
{
    const auto [column, row] = selected;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey ||
        code == juce::KeyPress::downKey)
    {
        selectSlot (column + (code == juce::KeyPress::rightKey) - (code == juce::KeyPress::leftKey),
                    row + (code == juce::KeyPress::downKey) - (code == juce::KeyPress::upKey));
        return true;
    }
    if (code == juce::KeyPress::returnKey)
    {
        if (key.getModifiers().isCommandDown())
            launchScene (row);
        else if (key.getModifiers().isShiftDown())
            stopColumn (column);
        else if (getClip (column, row) != nullptr)
            launch (column, row);
        else
            placePattern (column, row, session.getCurrentPattern());
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        if (const auto* clip = getClip (column, row))
            removeClip (clip->id);
        return true;
    }
    return false;
}

//==============================================================================
bool Launcher::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (project::isImportableAudio (juce::File (f)))
            return true;
    return false;
}

void Launcher::filesDropped (const juce::StringArray& files, int x, int y)
{
    // Several files go down the column, one per scene.
    const auto p = grid->getLocalPoint (this, juce::Point<int> (x, y));
    const int column = juce::jmax (0, (p.x - sceneWidth) / cellWidth);
    int row = juce::jmax (0, (p.y - headerHeight) / cellHeight);
    int added = 0;
    say ({});
    juce::MouseCursor::showWaitCursor(); // MP3s, Opus and others are decoded first: a second or two each
    for (auto& f : files)
        if (addAudioFile (column, row, juce::File (f)))
        {
            ++added;
            ++row;
        }
    juce::MouseCursor::hideWaitCursor();
    if (added == 0 && status.getText().isEmpty())
        say ("Spinus cannot read that as audio.");
}

//==============================================================================
void Launcher::say (const juce::String& text) { status.setText (text, juce::dontSendNotification); }

void Launcher::updateControls()
{
    quantize.setSelectedId (1 + (int)session.getProject().launcher.quantize, juce::dontSendNotification);
    backButton.setEnabled (session.getAdapter().isLauncherActive());
    recordButton.setToggleState (session.isLauncherRecording(), juce::dontSendNotification);
    if (session.getLastLauncherRecorded() != shownRecorded)
    {
        shownRecorded = session.getLastLauncherRecorded();
        if (shownRecorded > 0)
            say ("Recorded " + juce::String (shownRecorded) + (shownRecorded == 1 ? " clip" : " clips") +
                 " to the Playlist, on new tracks named \"Launcher: ...\". Ctrl+Z takes them away.");
    }
}

void Launcher::handleAsyncUpdate()
{
    grid->updateSize();
    selected = { juce::jmin (selected.first, getNumColumns() - 1), juce::jmin (selected.second, getNumRows() - 1) };
    updateControls();
    grid->repaint();
}

void Launcher::timerCallback()
{
    // Launch states and progress change as the music plays.
    if (! isShowing())
        return;
    updateControls();
    for (auto& clip : session.getProject().launcher.clips)
        if (session.getAdapter().getLaunchState (clip.id) != LaunchState::stopped)
        {
            grid->repaint();
            return;
        }
}

void Launcher::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    launchLabel.setColour (juce::Label::textColourId, theme.get ("text"));
    status.setColour (juce::Label::textColourId, theme.get ("textDim"));
    backButton.setColour (juce::TextButton::buttonColourId,
                          backButton.isEnabled() ? theme.get ("accent").withAlpha (0.6f) : theme.get ("panelHeader"));
    recordButton.setColour (juce::TextButton::buttonOnColourId, theme.get ("danger"));
}

void Launcher::resized()
{
    auto area = getLocalBounds().reduced (6);
    auto top = area.removeFromTop (26);
    launchLabel.setBounds (top.removeFromLeft (56));
    quantize.setBounds (top.removeFromLeft (160).reduced (0, 1));
    top.removeFromLeft (10);
    stopAllButton.setBounds (top.removeFromLeft (80).reduced (0, 1));
    top.removeFromLeft (6);
    backButton.setBounds (top.removeFromLeft (160).reduced (0, 1));
    top.removeFromLeft (6);
    recordButton.setBounds (top.removeFromLeft (150).reduced (0, 1));
    status.setBounds (area.removeFromBottom (22));
    area.removeFromTop (4);
    viewport.setBounds (area);
}

} // namespace spinus::ui

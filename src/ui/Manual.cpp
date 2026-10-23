#include "Manual.h"

#include "Theme.h"
#include "session/ProgramFolder.h"

#include <cmath>

namespace spinus::ui
{

namespace
{
const juce::StringArray& stopWords()
{
    static const juce::StringArray words { "the",  "a",    "an",  "and",  "or",  "to", "of",   "in",
                                           "on",   "for",  "is",  "it",   "be",  "as", "by",   "with",
                                           "this", "that", "you", "your", "are", "at", "from", "how" };
    return words;
}

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

constexpr const char* onlineManual = "https://github.com/tuturu742/spinus-daw/blob/main/docs/user/";
constexpr int sideWidth = 270, topHeight = 34;
} // namespace

//==============================================================================
juce::File Manual::findFolder (const juce::File& programFolder)
{
    for (const auto& candidate : { programFolder.getChildFile ("ai/docs/user"), programFolder.getChildFile ("docs") })
        if (candidate.getChildFile ("README.md").existsAsFile())
            return candidate;
#if defined(SPINUS_SOURCE_DIR)
    if (const auto source = juce::File (SPINUS_SOURCE_DIR).getChildFile ("docs/user");
        source.getChildFile ("README.md").existsAsFile())
        return source;
#endif
    return programFolder.getChildFile ("ai/docs/user");
}

juce::File Manual::findFolder() { return findFolder (getProgramFolder()); }

juce::StringArray Manual::wordsOf (const juce::String& text)
{
    juce::StringArray words;
    juce::String word;
    for (auto c : text.toLowerCase() + " ")
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            word << juce::String::charToString (c);
        else if (word.isNotEmpty())
        {
            words.add (word);
            word.clear();
        }
    }
    return words;
}

Manual::Manual (const juce::File& f) : folder (f)
{
    if (! folder.isDirectory())
        return;

    // The contents page first, then its order.
    juce::StringArray order { "README.md" };
    const auto readme = folder.getChildFile ("README.md").loadFileAsString();
    for (auto& block : markdown::parse (readme))
        for (auto& span : block.spans)
            if (span.link.endsWith (".md") && ! span.link.contains ("/"))
                order.addIfNotAlreadyThere (span.link);
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.md");
    files.sort();
    for (auto& file : files)
        order.addIfNotAlreadyThere (file.getFileName());

    for (auto& name : order)
    {
        const auto file = folder.getChildFile (name);
        if (! file.existsAsFile())
            continue;
        Page page { name, {}, markdown::parse (file.loadFileAsString()) };
        for (auto& block : page.blocks)
            if (block.kind == markdown::BlockKind::heading)
            {
                page.title = markdown::plainText (block.spans);
                break;
            }
        if (name == "README.md")
            page.title = "Contents";
        if (page.title.isEmpty())
            page.title = file.getFileNameWithoutExtension();
        pages.push_back (std::move (page));
    }

    // Sections to search: the top of each page, then each heading below its title.
    double totalLength = 0;
    for (int p = 0; p < (int)pages.size(); ++p)
    {
        Section section { p, {}, pages[(size_t)p].title };
        juce::StringArray texts;
        auto finish = [&]
        {
            section.text = texts.joinIntoString (" ").replaceCharacters ("\n", " ");
            section.lowerText = (section.heading + " " + section.text).toLowerCase();
            for (auto& w : wordsOf (section.heading))
                section.terms[w] += 3;
            for (auto& w : wordsOf (section.text))
                section.terms[w] += 1;
            for (auto& [term, count] : section.terms)
                section.length += count;
            totalLength += section.length;
            sections.push_back (std::move (section));
            texts.clear();
        };
        bool titleSeen = false; // the page's own title starts its first section
        for (auto& block : pages[(size_t)p].blocks)
        {
            if (block.kind != markdown::BlockKind::heading)
            {
                texts.add (markdown::plainText (block));
                continue;
            }
            const bool isTitle = ! titleSeen && block.level == 1;
            titleSeen = true;
            if (isTitle)
                continue;
            finish();
            section = { p, block.anchor,
                        pages[(size_t)p].title + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\xba ")) +
                            markdown::plainText (block.spans) };
        }
        finish();
    }
    if (! sections.empty())
        averageLength = juce::jmax (1.0, totalLength / (double)sections.size());
}

int Manual::findPage (const juce::String& file) const
{
    for (size_t i = 0; i < pages.size(); ++i)
        if (pages[i].file == file)
            return (int)i;
    return -1;
}

std::vector<Manual::Result> Manual::search (const juce::String& query, int maxResults) const
{
    auto words = wordsOf (query);
    juce::StringArray meaningful;
    for (auto& w : words)
        if (! stopWords().contains (w))
            meaningful.addIfNotAlreadyThere (w);
    if (meaningful.isEmpty())
        meaningful = words;
    if (meaningful.isEmpty() || sections.empty())
        return {};

    // How much a section has of a word: whole words count fully, longer words starting with it a little less.
    auto countIn = [] (const Section& s, const juce::String& word)
    {
        double count = 0;
        for (auto it = s.terms.lower_bound (word); it != s.terms.end() && it->first.startsWith (word); ++it)
            count += it->first == word ? it->second : 0.7 * it->second;
        return count;
    };

    const auto n = (double)sections.size();
    std::vector<double> idf;
    for (auto& word : meaningful)
    {
        int having = 0;
        for (auto& s : sections)
            having += countIn (s, word) > 0 ? 1 : 0;
        idf.push_back (std::log (1.0 + (n - having + 0.5) / (having + 0.5)));
    }

    constexpr double k1 = 1.2, b = 0.75;
    const auto phrase = query.trim().toLowerCase();
    std::vector<std::pair<Result, int>> scored; // with the number of words missing
    for (size_t i = 0; i < sections.size(); ++i)
    {
        const auto& s = sections[i];
        double score = 0;
        int missing = 0;
        for (int w = 0; w < meaningful.size(); ++w)
        {
            const auto tf = countIn (s, meaningful[w]);
            if (tf <= 0)
            {
                ++missing;
                continue;
            }
            score += idf[(size_t)w] * tf * (k1 + 1) / (tf + k1 * (1 - b + b * s.length / averageLength));
        }
        if (missing == meaningful.size())
            continue;
        if (meaningful.size() > 1 && s.lowerText.contains (phrase))
            score *= 1.5;
        Result r { s.page, s.anchor, s.heading, {}, score };
        scored.emplace_back (std::move (r), missing);
    }

    const bool anyComplete = std::any_of (scored.begin(), scored.end(), [] (auto& r) { return r.second == 0; });
    std::vector<Result> results;
    for (auto& [r, missing] : scored)
        if (! anyComplete || missing == 0)
            results.push_back (r);
    std::stable_sort (results.begin(), results.end(),
                      [] (const Result& x, const Result& y) { return x.score > y.score; });
    if ((int)results.size() > maxResults)
        results.resize ((size_t)maxResults);

    // The words around the first match.
    for (auto& r : results)
    {
        const auto* section = [&]() -> const Section*
        {
            for (auto& s : sections)
                if (s.page == r.page && s.anchor == r.anchor)
                    return &s;
            return nullptr;
        }();
        if (section == nullptr)
            continue;
        const auto& text = section->text;
        const auto lower = text.toLowerCase();
        int at = -1;
        for (auto& word : meaningful)
            for (int from = 0; (from = lower.indexOf (from, word)) >= 0; ++from)
                if (from == 0 || ! juce::CharacterFunctions::isLetterOrDigit (lower[from - 1]))
                {
                    at = at < 0 ? from : juce::jmin (at, from);
                    break;
                }
        int start = juce::jmax (0, at - 50);
        while (start > 0 && start < text.length() && text[start - 1] != ' ')
            --start;
        auto snippet = text.substring (start, start + 170);
        if (start + 170 < text.length())
            snippet = snippet.upToLastOccurrenceOf (" ", false, false) +
                      juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));
        r.snippet =
            (start > 0 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) : juce::String()) + snippet.trim();
    }
    return results;
}

//==============================================================================
ManualWindow::ManualWindow (const juce::File& folder) : manual (folder)
{
    search.setTextToShowWhenEmpty ("Search the manual", juce::Colours::grey);
    search.setHelpText ("Type words to find them in every page of the manual (no AI needed)");
    search.onTextChange = [this] { setSearchText (search.getText()); };
    search.onEscapeKey = [this] { search.clear(); };
    search.onReturnKey = [this]
    {
        if (! results.empty())
            openResult (0);
    };
    addAndMakeVisible (search);

    list.setRowHeight (26);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);

    backButton.onClick = [this] { back(); };
    backButton.setEnabled (false);
    addAndMakeVisible (backButton);
    pageTitle.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    addAndMakeVisible (pageTitle);

    viewport.setViewedComponent (&view, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    viewport.getVerticalScrollBar().setSingleStepSize (40.0);
    addAndMakeVisible (viewport);
    view.onLink = [this] (const juce::String& target) { openLink (target); };

    setSize (1060, 740);
    if (! manual.getPages().empty())
        showPage (0, {}, false);
    else
    {
        view.setContent (
            markdown::parse ("# The manual is not installed\n\nSpinus did not find its manual next to it (in `" +
                             folder.getFullPathName() + "`).\n\nRead it online: [the Spinus manual](" + onlineManual +
                             "README.md)."),
            folder);
        pageTitle.setText ("Manual", juce::dontSendNotification);
    }
}

ManualWindow::~ManualWindow() = default;

void ManualWindow::setSearchText (const juce::String& text)
{
    if (search.getText() != text)
        search.setText (text, false);
    searching = text.trim().isNotEmpty();
    results = searching ? manual.search (text) : std::vector<Manual::Result> {};
    list.setRowHeight (searching ? 54 : 26);
    list.updateContent();
    list.deselectAllRows();
    if (! searching && current >= 0)
        list.selectRow (current, false, true);
    list.repaint();
}

void ManualWindow::focusSearch()
{
    if (search.isShowing())
        search.grabKeyboardFocus();
}

void ManualWindow::openResult (int index)
{
    if (! juce::isPositiveAndBelow (index, (int)results.size()))
        return;
    const auto& r = results[(size_t)index];
    showPage (r.page, r.anchor, true);
}

bool ManualWindow::openLink (const juce::String& target)
{
    if (target.startsWith ("http://") || target.startsWith ("https://") || target.startsWith ("mailto:"))
        return juce::URL (target).launchInDefaultBrowser();

    const auto file = target.upToFirstOccurrenceOf ("#", false, false).fromLastOccurrenceOf ("./", false, false);
    const auto anchor = target.fromFirstOccurrenceOf ("#", false, false);
    if (file.isEmpty())
    {
        if (current < 0)
            return false;
        showPage (current, anchor, true);
        return true;
    }
    if (const auto page = manual.findPage (file); page >= 0 && ! target.startsWith ("../"))
    {
        showPage (page, anchor, true);
        return true;
    }
    // Outside the manual (the changelog, THIRD_PARTY.md): on GitHub.
    if (! juce::File::isAbsolutePath (target))
        return juce::URL (onlineManual + target).launchInDefaultBrowser();
    return false;
}

void ManualWindow::back()
{
    if (history.empty())
        return;
    const auto [page, y] = history.back();
    history.pop_back();
    showPage (page, {}, false);
    viewport.setViewPosition (0, y);
}

juce::String ManualWindow::getCurrentPage() const
{
    return juce::isPositiveAndBelow (current, (int)manual.getPages().size()) ? manual.getPages()[(size_t)current].file
                                                                             : juce::String();
}

void ManualWindow::showPage (int page, const juce::String& anchor, bool remember)
{
    if (! juce::isPositiveAndBelow (page, (int)manual.getPages().size()))
        return;
    if (page != current)
    {
        if (remember && current >= 0)
            history.emplace_back (current, viewport.getViewPositionY());
        current = page;
        const auto& p = manual.getPages()[(size_t)page];
        view.setContent (p.blocks, manual.getFolder());
        view.setWidth (viewport.getMaximumVisibleWidth());
        pageTitle.setText (p.title, juce::dontSendNotification);
        if (! searching)
            list.selectRow (page, false, true);
    }
    else if (remember && anchor.isNotEmpty())
    {
        history.emplace_back (current, viewport.getViewPositionY());
    }
    backButton.setEnabled (! history.empty());
    scrollTo (anchor);
}

void ManualWindow::scrollTo (const juce::String& anchor)
{
    const auto y = anchor.isEmpty() ? 0 : view.getAnchorY (anchor);
    viewport.setViewPosition (0, juce::jmax (0, y - 8));
    if (anchor.isNotEmpty() && y >= 0)
        view.flashAnchor (anchor);
}

//==============================================================================
int ManualWindow::getNumRows()
{
    return searching ? juce::jmax (1, (int)results.size()) : (int)manual.getPages().size();
}

void ManualWindow::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto& theme = themeOf (*this);
    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (10, 0);
    if (selected)
    {
        g.setColour (theme.get ("accent").withAlpha (0.18f));
        g.fillRoundedRectangle (juce::Rectangle<int> (0, 0, width, height).reduced (4, 1).toFloat(), 4.0f);
    }

    if (! searching)
    {
        if (! juce::isPositiveAndBelow (row, (int)manual.getPages().size()))
            return;
        g.setColour (selected ? theme.get ("text") : theme.get ("text").withAlpha (0.85f));
        g.setFont (juce::FontOptions (13.0f, row == 0 ? juce::Font::bold : juce::Font::plain));
        g.drawText (manual.getPages()[(size_t)row].title, area, juce::Justification::centredLeft, true);
        return;
    }

    if (results.empty())
    {
        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (12.5f, juce::Font::italic));
        g.drawText ("Nothing found", area, juce::Justification::centredLeft, true);
        return;
    }
    if (! juce::isPositiveAndBelow (row, (int)results.size()))
        return;
    const auto& r = results[(size_t)row];
    g.setColour (theme.get ("text"));
    g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    g.drawText (r.heading, area.removeFromTop (20).withTrimmedTop (4), juce::Justification::centredLeft, true);
    g.setColour (theme.get ("textDim"));
    g.setFont (juce::FontOptions (11.5f));
    g.drawFittedText (r.snippet, area.withTrimmedBottom (3), juce::Justification::topLeft, 2, 1.0f);
}

void ManualWindow::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (searching)
        openResult (row);
    else
        showPage (row, {}, true);
}

void ManualWindow::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    g.setColour (theme.get ("background"));
    g.fillRect (getLocalBounds().removeFromLeft (sideWidth));
    g.setColour (theme.get ("panelHeader"));
    g.fillRect (getLocalBounds().withTrimmedLeft (sideWidth).removeFromTop (topHeight));
    g.setColour (theme.get ("border"));
    g.fillRect (sideWidth - 1, 0, 1, getHeight());
    pageTitle.setColour (juce::Label::textColourId, theme.get ("text"));
}

void ManualWindow::resized()
{
    auto area = getLocalBounds();
    auto side = area.removeFromLeft (sideWidth);
    search.setBounds (side.removeFromTop (topHeight).reduced (8, 4));
    list.setBounds (side.reduced (0, 4));

    auto top = area.removeFromTop (topHeight);
    backButton.setBounds (top.removeFromLeft (80).reduced (6, 5));
    pageTitle.setBounds (top.reduced (6, 0));
    viewport.setBounds (area);
    const auto y = viewport.getViewPositionY();
    view.setWidth (viewport.getMaximumVisibleWidth());
    viewport.setViewPosition (0, y);
}

//==============================================================================
namespace
{
juce::Component::SafePointer<juce::DocumentWindow>& openWindow()
{
    static juce::Component::SafePointer<juce::DocumentWindow> window;
    return window;
}

class Window final : public juce::DocumentWindow
{
public:
    Window (ManualWindow* content, juce::Component* owner)
        : DocumentWindow ("Spinus manual", juce::Colour (0xff1e2227), DocumentWindow::allButtons)
    {
        if (owner != nullptr)
            if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&owner->getLookAndFeel()))
            {
                setBackgroundColour (lf->getTheme().get ("panel"));
                content->setLookAndFeel (lf);
            }
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (640, 420, 4000, 3000);
        if (owner != nullptr)
            centreAroundComponent (owner->getTopLevelComponent(), getWidth(), getHeight());
        else
            centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~Window() override
    {
        if (auto* content = getContentComponent())
            content->setLookAndFeel (nullptr);
        clearContentComponent();
    }

    void closeButtonPressed() override { delete this; }

    void activeWindowStatusChanged() override
    {
        DocumentWindow::activeWindowStatusChanged();
        if (auto* manual = dynamic_cast<ManualWindow*> (getContentComponent());
            manual != nullptr && isActiveWindow() && ! manual->hasKeyboardFocus (true))
            manual->focusSearch();
    }
};
} // namespace

void ManualWindow::show (juce::Component* centreAround, const juce::String& page, const juce::File& folder)
{
    if (openWindow() == nullptr)
        openWindow() =
            new Window (new ManualWindow (folder != juce::File() ? folder : Manual::findFolder()), centreAround);
    openWindow()->toFront (true);
    if (auto* manual = getOpen())
    {
        if (page.isNotEmpty())
            manual->openLink (page);
        manual->focusSearch();
    }
}

ManualWindow* ManualWindow::getOpen()
{
    return openWindow() != nullptr ? dynamic_cast<ManualWindow*> (openWindow()->getContentComponent()) : nullptr;
}

void ManualWindow::closeWindow()
{
    if (auto* window = openWindow().getComponent())
        delete window;
}

} // namespace spinus::ui

#pragma once

#include "Markdown.h"

namespace spinus::ui
{

/** The user manual (docs/user) as Help › Manual shows it: its pages and a plain text search over them, with
    no AI and no AI service (docs/user/manual-and-about.md). The pages are the ones installed for
    the assistant (cmake/SpinusInstall.cmake, packaging/windows/stage.py); see findFolder. */
class Manual
{
public:
    /** Reads every page in the folder (nothing if it is not there). */
    explicit Manual (const juce::File& folder);

    /** Where the manual is installed, found from Spinus's own folder: <program>/ai/docs/user (Arch, AppImage,
        Flatpak and Windows; the AI service's copy), else <program>/docs (Windows), else, in a development
        build, the source tree's docs/user. A nonexistent folder if none is there. */
    static juce::File findFolder();
    static juce::File findFolder (const juce::File& programFolder);

    struct Page
    {
        juce::String file;  // "mixer.md"
        juce::String title; // its first heading ("Mixer"); "Contents" for README.md
        std::vector<markdown::Block> blocks;
    };

    const juce::File& getFolder() const noexcept { return folder; }
    /** README.md (the contents) first, then the pages in the order it lists them, then any others by name. */
    const std::vector<Page>& getPages() const noexcept { return pages; }
    /** The page with this file name, or -1. */
    int findPage (const juce::String& file) const;

    struct Result
    {
        int page = -1;
        juce::String anchor;  // the section's heading, as a link name; empty: the top of the page
        juce::String heading; // "Mixer › Sends"
        juce::String snippet; // the words around the first match
        double score = 0;
    };

    /** The sections that have every word searched for (words may be the start of longer ones: "autom" finds
        "automation"), best first: words in a heading count more, rare words more than common ones (BM25). If no
        section has them all, sections with some of them. */
    std::vector<Result> search (const juce::String& query, int maxResults = 60) const;

    static juce::StringArray wordsOf (const juce::String& text);

private:
    struct Section
    {
        int page = 0;
        juce::String anchor, heading, text, lowerText;
        std::map<juce::String, int> terms; // heading words count three times
        int length = 0;
    };

    juce::File folder;
    std::vector<Page> pages;
    std::vector<Section> sections;
    double averageLength = 1.0;
};

//==============================================================================
/** Help › Manual (F1): the pages on the left (or, while searching, what was found), the page on the right, in
    Spinus's colours. Links to other pages open them, Back returns; links to the web open the browser. */
class ManualWindow final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit ManualWindow (const juce::File& folder);
    ~ManualWindow() override;

    /** Opens the manual's window (or brings it to the front), at this page if one is given ("mixer.md#sends"). */
    static void show (juce::Component* centreAround, const juce::String& page = {}, const juce::File& folder = {});
    static ManualWindow* getOpen();
    static void closeWindow();

    const Manual& getManual() const noexcept { return manual; }
    /** Opens a page ("mixer.md", "mixer.md#sends", "#sends" on this page); false if there is no such page. */
    bool openLink (const juce::String& target);
    void back();
    juce::String getCurrentPage() const;
    markdown::View& getPageView() noexcept { return view; }
    juce::Viewport& getViewport() noexcept { return viewport; }

    void setSearchText (const juce::String&);
    /** Typing goes to the search box (the window opening, or coming to the front). */
    void focusSearch();
    const std::vector<Manual::Result>& getResults() const noexcept { return results; }
    /** Opens a search result: its page, scrolled to its section. */
    void openResult (int index);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showPage (int page, const juce::String& anchor, bool remember);
    void scrollTo (const juce::String& anchor);

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;

    Manual manual;
    juce::TextEditor search;
    juce::ListBox list { "Pages", this };
    juce::TextButton backButton { juce::CharPointer_UTF8 ("\xe2\x80\xb9 Back") };
    juce::Label pageTitle;
    juce::Viewport viewport;
    markdown::View view;
    std::vector<Manual::Result> results;
    int current = -1;
    std::vector<std::pair<int, int>> history; // page and scroll position, for Back
    bool searching = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManualWindow)
};

} // namespace spinus::ui

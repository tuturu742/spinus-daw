#include <catch2/catch_test_macros.hpp>

#include "ui/About.h"
#include "ui/Manual.h"
#include "ui/Markdown.h"
#include "ui/Theme.h"

#include <SpinusVersion.h>

using namespace spinus;
using namespace spinus::ui;
namespace md = spinus::ui::markdown;

namespace
{
juce::File docs() { return juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("docs/user"); }

struct Temp
{
    Temp() { root.createDirectory(); }
    ~Temp() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-manual-" + juce::Uuid().toString());
};

const char* const samplePage = R"(# Mixer

The **mixer** has *inserts*, `Ctrl+M` and a [link](eq.md#bands) to the EQ, snake_case_words and 2 * 3.

![The mixer](images/mixer.png)

## Sends and buses

- One
- Two with **bold**
  wrapped on the next line
  - Nested
1. First
2. Second

| Key | Does |
|---|---|
| **F9** | Opens the `Mixer` \| here |
| Ctrl+Z | Undo |

> A quote

```
code block
  indented
```

---

## Sends and buses
)";
} // namespace

TEST_CASE ("Markdown: inline styles, links and anchors", "[ui][manual]")
{
    const auto spans =
        md::parseInline ("A **bold** and *italic* `code`, [the **EQ**](eq.md#bands), snake_case, 2 * 3 \\*");
    REQUIRE (spans.size() >= 8);
    CHECK (spans[0] == md::Span { "A " });
    CHECK (spans[1] == md::Span { "bold", true });
    CHECK (spans[3] == md::Span { "italic", false, true });
    CHECK (spans[5] == md::Span { "code", false, false, true });
    CHECK (spans[7].text == "the ");
    CHECK (spans[7].link == "eq.md#bands");
    CHECK (spans[8] == md::Span { "EQ", true, false, false, "eq.md#bands" });
    CHECK (md::plainText (spans) == "A bold and italic code, the EQ, snake_case, 2 * 3 *");

    CHECK (md::anchorFor ("Listen to the demo song first") == "listen-to-the-demo-song-first");
    CHECK (md::anchorFor ("1. Sound") == "1-sound");
    CHECK (md::anchorFor (juce::CharPointer_UTF8 ("Help \xe2\x80\xba Manual")) == "help--manual");
}

TEST_CASE ("Markdown: a page's blocks", "[ui][manual]")
{
    const auto blocks = md::parse (samplePage);
    juce::StringArray kinds;
    for (auto& b : blocks)
        kinds.add (juce::String ((int)b.kind) + ":" + juce::String (b.level));
    using K = md::BlockKind;
    auto kind = [] (K k, int level = 0) { return juce::String ((int)k) + ":" + juce::String (level); };
    CHECK (kinds == juce::StringArray { kind (K::heading, 1), kind (K::paragraph), kind (K::image),
                                        kind (K::heading, 2), kind (K::listItem), kind (K::listItem),
                                        kind (K::listItem, 1), kind (K::listItem), kind (K::listItem), kind (K::table),
                                        kind (K::quote), kind (K::code), kind (K::rule), kind (K::heading, 2) });
    CHECK (blocks[2].text == "images/mixer.png");
    CHECK (blocks[2].alt == "The mixer");
    CHECK (blocks[3].anchor == "sends-and-buses");
    CHECK (blocks[13].anchor == "sends-and-buses-1"); // a heading again, as GitHub numbers it
    CHECK (md::plainText (blocks[5]) == "Two with bold wrapped on the next line");
    CHECK (blocks[7].ordered);
    CHECK (blocks[8].number == 2);
    REQUIRE (blocks[9].rows.size() == 3);
    CHECK (md::plainText (blocks[9].rows[1][1]) == "Opens the Mixer | here");
    CHECK (blocks[11].text == "code block\n  indented");
}

TEST_CASE ("Markdown is drawn: sized to its content, with links and images", "[ui][manual]")
{
    Temp t;
    const auto images = t.root.getChildFile ("images");
    images.createDirectory();
    juce::Image picture (juce::Image::RGB, 800, 400, true);
    {
        juce::FileOutputStream out (images.getChildFile ("mixer.png"));
        REQUIRE (juce::PNGImageFormat().writeImageToStream (picture, out));
    }

    SpinusLookAndFeel lf { Theme::dark() };
    md::View view;
    view.setLookAndFeel (&lf);
    view.setContent (md::parse (samplePage), t.root);
    view.setWidth (600);
    CHECK (view.getWidth() == 600);
    CHECK (view.getHeight() > 600);
    CHECK (view.getNumImagesLoaded() == 1);

    // Headings can be scrolled to; the first one again is lower down.
    const auto sends = view.getAnchorY ("sends-and-buses");
    CHECK (sends > 200);
    CHECK (view.getAnchorY ("sends-and-buses-1") > sends);
    CHECK (view.getAnchorY ("nowhere") == -1);
    CHECK (view.getAnchorAbove (sends + 5) == "sends-and-buses");

    // Narrower: taller (the words wrap, the picture is smaller).
    const auto wide = view.getHeight();
    view.setWidth (360);
    CHECK (view.getHeight() > wide - 200);

    // The link is found where it is drawn: somewhere on the first paragraph's lines.
    view.setWidth (600);
    juce::String found;
    for (int y = 40; y < 140 && found.isEmpty(); y += 2)
        for (int x = 24; x < 580 && found.isEmpty(); x += 3)
            found = view.getLinkAt ({ (float)x, (float)y });
    CHECK (found == "eq.md#bands");
    auto image = view.createComponentSnapshot (view.getLocalBounds().withHeight (300));
    CHECK (image.isValid());
    view.setLookAndFeel (nullptr);
}

TEST_CASE ("The manual's pages are found and searched without AI", "[ui][manual]")
{
    // Installed beside the program (ai/docs/user), or in the source tree in a development build.
    Temp t;
    const auto installed = t.root.getChildFile ("lib/spinus/ai/docs/user");
    installed.createDirectory();
    installed.getChildFile ("README.md").replaceWithText ("# Manual\n\n- [B](b.md)\n- [A](a.md)\n");
    installed.getChildFile ("a.md").replaceWithText ("# Alpha page\n\nAbout automation.\n\n## Recording automation\n\n"
                                                     "Record knob moves as automation clips. Automation automation.\n");
    installed.getChildFile ("b.md").replaceWithText (
        "# Bravo page\n\nThe mixer and its sends.\n\n## Freeze\n\nFreezing "
        "a channel saves CPU.\n");
    installed.getChildFile ("z.md").replaceWithText ("No heading here, about the mixer.\n");
    CHECK (Manual::findFolder (t.root.getChildFile ("lib/spinus")) == installed);
    const auto windows = t.root.getChildFile ("Spinus");
    windows.getChildFile ("docs").createDirectory();
    windows.getChildFile ("docs/README.md").replaceWithText ("# Manual");
    CHECK (Manual::findFolder (windows) == windows.getChildFile ("docs"));
    CHECK (Manual::findFolder (t.root.getChildFile ("nowhere")) == docs());

    // Pages in the contents' order, others after; titles from their first heading.
    Manual manual (installed);
    juce::StringArray titles;
    for (auto& p : manual.getPages())
        titles.add (p.title);
    CHECK (titles == juce::StringArray { "Contents", "Bravo page", "Alpha page", "z" });

    // Every word, also the start of longer ones; the section with them in its heading first.
    auto results = manual.search ("automation record");
    REQUIRE (results.size() == 1);
    CHECK (manual.getPages()[(size_t)results[0].page].file == "a.md");
    CHECK (results[0].anchor == "recording-automation");
    CHECK (results[0].heading ==
           juce::String (juce::CharPointer_UTF8 ("Alpha page \xe2\x80\xba Recording automation")));
    CHECK (results[0].snippet.containsIgnoreCase ("record"));

    results = manual.search ("freez");
    REQUIRE (! results.empty());
    CHECK (results[0].anchor == "freeze");
    results = manual.search ("mixer");
    CHECK (results.size() == 2);
    CHECK (manual.search ("the").empty() == false); // only common words: searched as they are
    CHECK (manual.search ("zzzz").empty());
    CHECK (manual.search ("").empty());
    // Not every word anywhere: the sections with some.
    CHECK (! manual.search ("mixer qqqq").empty());
}

TEST_CASE ("The manual window: the real pages, search, links and Back", "[ui][manual]")
{
    SpinusLookAndFeel lf { Theme::dark() };
    ManualWindow window (docs());
    window.setLookAndFeel (&lf);
    window.setSize (1000, 700);
    const auto& manual = window.getManual();
    REQUIRE (manual.getPages().size() > 30);
    CHECK (window.getCurrentPage() == "README.md");

    // Every link between pages leads to a page that is there, and to a heading that is there.
    for (auto& page : manual.getPages())
        for (auto& block : page.blocks)
        {
            auto spans = block.spans;
            for (auto& row : block.rows)
                for (auto& cell : row)
                    spans.insert (spans.end(), cell.begin(), cell.end());
            for (auto& span : spans)
            {
                const auto file = span.link.upToFirstOccurrenceOf ("#", false, false);
                if (! file.endsWith (".md") || file.contains ("/"))
                    continue;
                INFO (page.file << " links to " << span.link);
                const auto target = manual.findPage (file);
                REQUIRE (target >= 0);
                if (const auto anchor = span.link.fromFirstOccurrenceOf ("#", false, false); anchor.isNotEmpty())
                {
                    bool there = false;
                    for (auto& b : manual.getPages()[(size_t)target].blocks)
                        there = there || b.anchor == anchor;
                    CHECK (there);
                }
            }
        }

    // A search finds the page about it; its result opens the page at its section.
    window.setSearchText ("freeze");
    REQUIRE (! window.getResults().empty());
    CHECK (manual.getPages()[(size_t)window.getResults()[0].page].file == "freeze.md");
    window.openResult (0);
    CHECK (window.getCurrentPage() == "freeze.md");

    // Links: to a heading on another page, then Back.
    REQUIRE (window.openLink ("main-window.md#keyboard-shortcuts"));
    CHECK (window.getCurrentPage() == "main-window.md");
    CHECK (window.getViewport().getViewPositionY() > 0);
    const auto heading = window.getPageView().getAnchorY ("keyboard-shortcuts");
    CHECK (heading >= window.getViewport().getViewPositionY());
    CHECK (heading < window.getViewport().getViewPositionY() + window.getViewport().getViewHeight() - 40);
    window.back();
    CHECK (window.getCurrentPage() == "freeze.md");
    window.setLookAndFeel (nullptr);

    // Without the manual: says where to read it.
    ManualWindow none (juce::File ("/nonexistent/manual"));
    CHECK (none.getManual().getPages().empty());
    CHECK (md::plainText (none.getPageView().getBlocks().front()) == "The manual is not installed");
}

TEST_CASE ("About Spinus: version, licenses, the project's page and the credits", "[ui][about]")
{
    AboutSpinus about;
    const auto text = about.getText();
    CHECK (text.contains ("Spinus DAW"));
    CHECK (text.contains (versionString));
    CHECK (juce::String (versionString).contains ("-alpha")); // this alpha says so
    CHECK (text.contains ("AGPL-3.0-or-later"));
    CHECK (text.contains ("CC BY 4.0"));
    CHECK (text.contains ("https://github.com/tuturu742/spinus-daw"));
    CHECK (text.contains ("JUCE"));
    CHECK (text.contains ("Tracktion"));

    const auto info = AboutSpinus::getVersionInfo();
    CHECK (info.startsWith (juce::String ("Spinus DAW ") + versionString));
    CHECK (info.contains ("JUCE"));

    // The credits are THIRD_PARTY.md, built in.
    const auto credits = AboutSpinus::getThirdPartyText();
    CHECK (credits == juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("THIRD_PARTY.md").loadFileAsString());
    CHECK (credits.contains ("Tracktion Engine"));
}

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>

namespace spinus::ui
{
class Theme;
}

namespace spinus::ui::markdown
{

/*  The Markdown the manual (docs/user) and THIRD_PARTY.md are written in, read for Help › Manual and About:
    headings, paragraphs, lists (nested, numbered), **bold**, *italic*, `code`, code blocks, tables, links,
    images on a line of their own, quotes and rules. Not a full CommonMark reader: what these pages use. */

struct Span
{
    juce::String text;
    bool bold = false, italic = false, code = false;
    juce::String link; // the target of a [link](target)

    bool operator== (const Span&) const = default;
};

using Spans = std::vector<Span>;

enum class BlockKind
{
    heading,   // level 1-6, spans, anchor
    paragraph, // spans
    listItem,  // level (nesting, from 0), ordered and number, spans
    code,      // text
    table,     // rows of cells; the first row is the header
    image,     // text: the image's path as written; alt
    quote,     // spans
    rule
};

struct Block
{
    BlockKind kind = BlockKind::paragraph;
    int level = 0;
    bool ordered = false;
    int number = 0;
    Spans spans;
    juce::String text, alt, anchor;
    std::vector<std::vector<Spans>> rows;
};

std::vector<Block> parse (const juce::String& markdown);
Spans parseInline (const juce::String& text);

/** A heading's link name as GitHub makes it ("Listen to the demo song first" -> "listen-to-the-demo-song-first"),
    so links like `main-window.md#keyboard-shortcuts` find it. */
juce::String anchorFor (const juce::String& headingText);

juce::String plainText (const Spans&);
/** A block's words without Markdown (a table: its cells, a row a line). */
juce::String plainText (const Block&);

//==============================================================================
/** Shows parsed Markdown at the width it is given (setWidth): sized to its content, for a Viewport. Links
    call onLink with their target; images are read from the folder given (relative paths, as written). */
class View final : public juce::Component, private juce::Timer
{
public:
    View();

    void setContent (std::vector<Block>, const juce::File& imageFolder);
    const std::vector<Block>& getBlocks() const noexcept { return blocks; }

    /** Lays the content out for this width and takes its height. */
    void setWidth (int width);

    /** Where a heading is (by its anchor), for scrolling to it; -1 if there is none. */
    int getAnchorY (const juce::String& anchor) const;
    /** The link under a point, if any. */
    juce::String getLinkAt (juce::Point<float>) const;
    /** The heading to show as the reader's place: the last one at or above this y. */
    juce::String getAnchorAbove (int y) const;
    /** Draws a highlight behind the section of this heading for a moment (a search result). */
    void flashAnchor (const juce::String& anchor);

    std::function<void (const juce::String& target)> onLink;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void lookAndFeelChanged() override;

    /** The images the content shows that were found (tests). */
    int getNumImagesLoaded() const;

private:
    struct Text
    {
        juce::TextLayout layout;
        juce::Point<float> origin;
        std::vector<std::pair<juce::Range<int>, juce::String>> links;
    };
    struct Laid
    {
        juce::Rectangle<int> bounds;
        std::vector<Text> texts;                 // one, or a table's cells (row by row)
        std::vector<juce::Rectangle<int>> cells; // tables
        int headerHeight = 0;                    // tables
        juce::Image image;
    };

    void layOut();
    Text makeText (const Spans&, float size, float width, juce::Colour colour, bool bold = false,
                   bool italic = false) const;
    juce::Colour colour (const juce::String& role) const;

    std::vector<Block> blocks;
    std::vector<Laid> laid;
    juce::File images;
    mutable std::map<juce::String, juce::Image> imageCache;
    std::map<size_t, std::pair<std::vector<float>, std::vector<float>>> columnWidths; // tables: natural, longest word
    int width = 600;
    juce::String flashed;
    float flashAlpha = 0.0f;
    void timerCallback() override;
};

} // namespace spinus::ui::markdown

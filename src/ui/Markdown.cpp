#include "Markdown.h"

#include "Theme.h"

namespace spinus::ui::markdown
{

namespace
{
bool isAlnum (juce::juce_wchar c) { return juce::CharacterFunctions::isLetterOrDigit (c); }
bool isSpace (juce::juce_wchar c) { return juce::CharacterFunctions::isWhitespace (c); }

/** The index of the `close` matching the `open` at `start` (nesting counted), or -1. */
int findClosing (const juce::String& s, int start, juce::juce_wchar open, juce::juce_wchar close)
{
    int depth = 0;
    for (int i = start; i < s.length(); ++i)
    {
        if (s[i] == '\\')
            ++i;
        else if (s[i] == open)
            ++depth;
        else if (s[i] == close && --depth == 0)
            return i;
    }
    return -1;
}

/** "[text](target)" at `start`: the text, the target (without a "title") and the index after it. */
bool readLink (const juce::String& s, int start, juce::String& text, juce::String& target, int& end)
{
    const auto close = findClosing (s, start, '[', ']');
    if (close < 0 || close + 1 >= s.length() || s[close + 1] != '(')
        return false;
    const auto paren = findClosing (s, close + 1, '(', ')');
    if (paren < 0)
        return false;
    text = s.substring (start + 1, close);
    target = s.substring (close + 2, paren).trim().upToFirstOccurrenceOf (" \"", false, false);
    end = paren + 1;
    return true;
}

bool isTableSeparator (const juce::String& line)
{
    const auto t = line.trim();
    return t.startsWithChar ('|') && t.containsChar ('-') && t.containsOnly ("|:- \t");
}

juce::StringArray splitRow (const juce::String& line)
{
    auto t = line.trim();
    if (t.startsWithChar ('|'))
        t = t.substring (1);
    if (t.endsWithChar ('|') && ! t.endsWith ("\\|"))
        t = t.dropLastCharacters (1);
    juce::StringArray cells;
    juce::String cell;
    bool inCode = false;
    for (int i = 0; i < t.length(); ++i)
    {
        const auto c = t[i];
        if (c == '\\' && i + 1 < t.length() && t[i + 1] == '|')
        {
            cell << "|";
            ++i;
            continue;
        }
        if (c == '`')
            inCode = ! inCode;
        if (c == '|' && ! inCode)
        {
            cells.add (cell.trim());
            cell.clear();
            continue;
        }
        cell << juce::String::charToString (c);
    }
    cells.add (cell.trim());
    return cells;
}

/** A list marker ("- ", "* ", "+ ", "1. ", "2) "): its indent, whether it is numbered, its number and the text. */
bool readListMarker (const juce::String& line, int& indent, bool& ordered, int& number, juce::String& rest)
{
    int i = 0;
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t'))
        i += 1;
    indent = i;
    if (i >= line.length())
        return false;
    if ((line[i] == '-' || line[i] == '*' || line[i] == '+') && i + 1 < line.length() && line[i + 1] == ' ')
    {
        ordered = false;
        rest = line.substring (i + 2).trim();
        return true;
    }
    int j = i;
    while (j < line.length() && juce::CharacterFunctions::isDigit (line[j]))
        ++j;
    if (j > i && j - i <= 3 && j + 1 < line.length() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ')
    {
        ordered = true;
        number = line.substring (i, j).getIntValue();
        rest = line.substring (j + 2).trim();
        return true;
    }
    return false;
}

bool isRule (const juce::String& trimmed)
{
    if (trimmed.length() < 3)
        return false;
    const auto c = trimmed[0];
    return (c == '-' || c == '*' || c == '_') &&
           trimmed.removeCharacters (" ").containsOnly (juce::String::charToString (c));
}
} // namespace

//==============================================================================
Spans parseInline (const juce::String& s)
{
    Spans out;
    juce::String current;
    bool bold = false, italic = false;
    auto flush = [&]
    {
        if (current.isNotEmpty())
            out.push_back ({ current, bold, italic, false, {} });
        current.clear();
    };

    const int n = s.length();
    for (int i = 0; i < n;)
    {
        const auto c = s[i];
        if (c == '\\' && i + 1 < n && ! isAlnum (s[i + 1]) && ! isSpace (s[i + 1]))
        {
            current << juce::String::charToString (s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '`')
            if (const auto end = s.indexOf (i + 1, "`"); end > i)
            {
                flush();
                out.push_back ({ s.substring (i + 1, end), bold, italic, true, {} });
                i = end + 1;
                continue;
            }
        if (c == '!' && i + 1 < n && s[i + 1] == '[')
        {
            juce::String alt, target;
            if (int end = 0; readLink (s, i + 1, alt, target, end))
            {
                current << alt; // an image inside a line: its words
                i = end;
                continue;
            }
        }
        if (c == '[')
        {
            juce::String text, target;
            if (int end = 0; readLink (s, i, text, target, end))
            {
                flush();
                for (auto span : parseInline (text))
                {
                    span.bold = span.bold || bold;
                    span.italic = span.italic || italic;
                    span.link = target;
                    out.push_back (std::move (span));
                }
                i = end;
                continue;
            }
        }
        if (c == '<')
            if (const auto end = s.indexOf (i + 1, ">"); end > i && s.substring (i + 1, end).startsWith ("http"))
            {
                flush();
                out.push_back ({ s.substring (i + 1, end), bold, italic, false, s.substring (i + 1, end) });
                i = end + 1;
                continue;
            }
        if (c == '*' || c == '_')
        {
            const bool prevAlnum = i > 0 && isAlnum (s[i - 1]);
            const bool doubled = i + 1 < n && s[i + 1] == c;
            const int after = i + (doubled ? 2 : 1);
            const bool nextAlnum = after < n && isAlnum (s[after]);
            const bool nextSpace = after >= n || isSpace (s[after]);
            const bool prevSpace = i == 0 || isSpace (s[i - 1]);
            const bool intraword = c == '_' && prevAlnum && nextAlnum; // snake_case stays as it is
            const bool opening = ! (doubled ? bold : italic);
            if (! intraword && (opening ? ! nextSpace : ! prevSpace))
            {
                flush();
                if (doubled)
                    bold = ! bold;
                else
                    italic = ! italic;
                i = after;
                continue;
            }
        }
        current << juce::String::charToString (c);
        ++i;
    }
    flush();
    return out;
}

juce::String anchorFor (const juce::String& headingText)
{
    juce::String anchor;
    for (auto c : headingText.toLowerCase())
    {
        if (isAlnum (c) || c == '-' || c == '_')
            anchor << juce::String::charToString (c);
        else if (c == ' ')
            anchor << "-";
    }
    return anchor;
}

juce::String plainText (const Spans& spans)
{
    juce::String text;
    for (auto& s : spans)
        text << s.text;
    return text;
}

juce::String plainText (const Block& block)
{
    switch (block.kind)
    {
    case BlockKind::code:
        return block.text;
    case BlockKind::image:
        return block.alt;
    case BlockKind::rule:
        return {};
    case BlockKind::table:
    {
        juce::StringArray lines;
        for (auto& row : block.rows)
        {
            juce::StringArray cells;
            for (auto& cell : row)
                cells.add (plainText (cell));
            lines.add (cells.joinIntoString (" "));
        }
        return lines.joinIntoString ("\n");
    }
    case BlockKind::heading:
    case BlockKind::paragraph:
    case BlockKind::listItem:
    case BlockKind::quote:
        break;
    }
    return plainText (block.spans);
}

std::vector<Block> parse (const juce::String& markdown)
{
    juce::StringArray lines;
    lines.addLines (markdown);
    std::vector<Block> out;
    juce::String paragraph; // raw text of the open paragraph, list item or quote
    int open = -1;          // the open list item or quote in `out`
    std::vector<int> listIndents;
    std::map<juce::String, int> anchors;

    auto addTo = [] (juce::String& text, const juce::String& line)
    {
        const bool hardBreak = line.endsWith ("  ") || line.endsWithChar ('\\');
        auto words = line.trim();
        if (words.endsWithChar ('\\'))
            words = words.dropLastCharacters (1);
        if (text.isNotEmpty() && ! text.endsWithChar ('\n'))
            text << " ";
        text << words << (hardBreak ? "\n" : "");
    };
    auto close = [&]
    {
        if (paragraph.isNotEmpty())
            out.push_back ({ BlockKind::paragraph, 0, false, 0, {}, paragraph });
        paragraph.clear();
        open = -1;
    };

    for (int i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        const auto trimmed = line.trim();

        if (trimmed.startsWith ("```") || trimmed.startsWith ("~~~"))
        {
            close();
            const auto fence = trimmed.substring (0, 3);
            juce::StringArray code;
            for (++i; i < lines.size() && ! lines[i].trim().startsWith (fence); ++i)
                code.add (lines[i]);
            out.push_back ({ BlockKind::code, 0, false, 0, {}, code.joinIntoString ("\n") });
            listIndents.clear();
            continue;
        }
        if (trimmed.isEmpty())
        {
            close();
            continue;
        }
        if (line.startsWithChar ('#'))
        {
            const auto level = line.initialSectionContainingOnly ("#").length();
            if (level <= 6 && (line.length() == level || line[level] == ' '))
            {
                close();
                listIndents.clear();
                Block heading { BlockKind::heading, level };
                heading.text = line.substring (level).trim().trimCharactersAtEnd ("#").trim();
                out.push_back (std::move (heading));
                continue;
            }
        }
        if (isRule (trimmed) && paragraph.isEmpty() && open < 0)
        {
            out.push_back ({ BlockKind::rule });
            listIndents.clear();
            continue;
        }
        if (trimmed.startsWithChar ('|') && i + 1 < lines.size() && isTableSeparator (lines[i + 1]))
        {
            close();
            listIndents.clear();
            Block table { BlockKind::table };
            auto addRow = [&] (const juce::String& row)
            {
                std::vector<Spans> cells;
                for (auto& cell : splitRow (row))
                    cells.push_back (parseInline (cell));
                table.rows.push_back (std::move (cells));
            };
            addRow (trimmed);
            for (i += 2; i < lines.size() && lines[i].trim().startsWithChar ('|'); ++i)
                addRow (lines[i]);
            --i;
            out.push_back (std::move (table));
            continue;
        }
        if (trimmed.startsWith ("!["))
        {
            juce::String alt, target;
            if (int end = 0; readLink (trimmed, 1, alt, target, end) && end == trimmed.length())
            {
                close();
                Block image { BlockKind::image };
                image.text = target;
                image.alt = alt;
                out.push_back (std::move (image));
                continue;
            }
        }
        if (trimmed.startsWithChar ('>'))
        {
            if (open < 0 || out[(size_t)open].kind != BlockKind::quote)
            {
                close();
                out.push_back ({ BlockKind::quote });
                open = (int)out.size() - 1;
            }
            addTo (out[(size_t)open].text, trimmed.substring (1));
            continue;
        }
        int indent = 0, number = 0;
        bool ordered = false;
        // A list starts after a paragraph without a blank line, as in CommonMark (a numbered one only at 1).
        if (juce::String rest;
            readListMarker (line, indent, ordered, number, rest) && (paragraph.isEmpty() || ! ordered || number == 1))
        {
            close();
            while (! listIndents.empty() && indent < listIndents.back())
                listIndents.pop_back();
            if (listIndents.empty() || indent > listIndents.back())
                listIndents.push_back (indent);
            Block item { BlockKind::listItem, (int)listIndents.size() - 1, ordered, number };
            addTo (item.text, rest);
            out.push_back (std::move (item));
            open = (int)out.size() - 1;
            continue;
        }
        if (open >= 0)
        {
            addTo (out[(size_t)open].text, line); // the item or quote goes on
            continue;
        }
        listIndents.clear();
        addTo (paragraph, line);
    }
    close();

    // Inline Markdown, and the headings' anchors (a repeated one gets -1, -2, ... as on GitHub).
    for (auto& block : out)
    {
        if (block.kind == BlockKind::heading || block.kind == BlockKind::paragraph ||
            block.kind == BlockKind::listItem || block.kind == BlockKind::quote)
        {
            block.spans = parseInline (block.text);
            block.text.clear();
        }
        if (block.kind == BlockKind::heading)
        {
            const auto base = anchorFor (plainText (block.spans));
            const auto seen = anchors[base]++;
            block.anchor = seen == 0 ? base : base + "-" + juce::String (seen);
        }
    }
    return out;
}

//==============================================================================
namespace
{
constexpr int margin = 24;
constexpr float bodySize = 14.5f;

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

float headingSize (int level) { return level == 1 ? 25.0f : level == 2 ? 19.5f : level == 3 ? 16.5f : 15.0f; }
} // namespace

View::View() { setWantsKeyboardFocus (false); }

juce::Colour View::colour (const juce::String& role) const { return themeOf (*this).get (role); }

void View::setContent (std::vector<Block> content, const juce::File& imageFolder)
{
    blocks = std::move (content);
    images = imageFolder;
    columnWidths.clear();
    flashed.clear();
    layOut();
}

void View::setWidth (int newWidth)
{
    if (newWidth == width && ! laid.empty())
        return;
    width = juce::jmax (200, newWidth);
    layOut();
}

void View::lookAndFeelChanged()
{
    columnWidths.clear();
    layOut();
}

View::Text View::makeText (const Spans& spans, float size, float textWidth, juce::Colour textColour, bool bold,
                           bool italic) const
{
    Text t;
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (size * 0.3f);
    int position = 0;
    for (auto& span : spans)
    {
        const auto style = (int)((span.bold || bold ? (unsigned)juce::Font::bold : 0u) |
                                 (span.italic || italic ? (unsigned)juce::Font::italic : 0u));
        juce::Font font (span.code ? juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size * 0.9f, style)
                                   : juce::FontOptions (size, style));
        auto c = textColour;
        if (span.link.isNotEmpty())
        {
            font.setUnderline (true);
            c = colour ("accentAlt").brighter (0.25f);
        }
        else if (span.code)
        {
            c = colour ("accent");
        }
        s.append (span.text, font, c);
        if (span.link.isNotEmpty())
            t.links.emplace_back (juce::Range<int> (position, position + span.text.length()), span.link);
        position += span.text.length();
    }
    t.layout.createLayout (s, juce::jmax (40.0f, textWidth));
    return t;
}

void View::layOut()
{
    laid.clear();
    const auto text = colour ("text"), dim = colour ("textDim");
    const float textWidth = (float)(width - 2 * margin);
    int y = 18;

    for (size_t b = 0; b < blocks.size(); ++b)
    {
        const auto& block = blocks[b];
        Laid l;
        switch (block.kind)
        {
        case BlockKind::heading:
        {
            y += b == 0 ? 0 : block.level <= 2 ? 22 : 14;
            auto t = makeText (block.spans, headingSize (block.level), textWidth, text, true);
            t.origin = { (float)margin, (float)y };
            const auto h = (int)std::ceil (t.layout.getHeight()) + (block.level <= 2 ? 10 : 4);
            l.bounds = { 0, y - 6, width, h + 8 };
            l.texts.push_back (std::move (t));
            y += h + 8;
            break;
        }
        case BlockKind::paragraph:
        case BlockKind::quote:
        {
            const bool quote = block.kind == BlockKind::quote;
            const int indent = quote ? 16 : 0;
            auto t = makeText (block.spans, bodySize, textWidth - (float)indent, quote ? dim : text);
            t.origin = { (float)(margin + indent), (float)y };
            const auto h = (int)std::ceil (t.layout.getHeight());
            l.bounds = { margin, y, width - 2 * margin, h };
            l.texts.push_back (std::move (t));
            y += h + 12;
            break;
        }
        case BlockKind::listItem:
        {
            const int indent = 22 + 22 * block.level;
            auto t = makeText (block.spans, bodySize, textWidth - (float)indent, text);
            t.origin = { (float)(margin + indent), (float)y };
            const auto h = (int)std::ceil (t.layout.getHeight());
            l.bounds = { margin, y, width - 2 * margin, h };
            l.texts.push_back (std::move (t));
            const bool lastOfList = b + 1 >= blocks.size() || blocks[b + 1].kind != BlockKind::listItem;
            y += h + (lastOfList ? 12 : 5);
            break;
        }
        case BlockKind::code:
        {
            auto t = makeText ({ Span { block.text, false, false, true } }, bodySize, textWidth - 24, text);
            t.origin = { (float)(margin + 12), (float)(y + 10) };
            const auto h = (int)std::ceil (t.layout.getHeight()) + 20;
            l.bounds = { margin, y, width - 2 * margin, h };
            l.texts.push_back (std::move (t));
            y += h + 12;
            break;
        }
        case BlockKind::rule:
            l.bounds = { margin, y + 4, width - 2 * margin, 1 };
            y += 16;
            break;
        case BlockKind::image:
        {
            const auto key = block.text;
            if (! imageCache.contains (key))
            {
                const auto file = images.getChildFile (block.text);
                imageCache[key] = file.existsAsFile() ? juce::ImageFileFormat::loadFrom (file) : juce::Image();
            }
            l.image = imageCache[key];
            if (l.image.isValid())
            {
                const auto w = juce::jmin ((float)l.image.getWidth(), textWidth);
                const auto h = w * (float)l.image.getHeight() / (float)l.image.getWidth();
                l.bounds = { margin, y, (int)w, (int)h };
                y += (int)h + 14;
            }
            else
            {
                auto t = makeText ({ Span { "[" + block.alt + "]", false, true } }, bodySize, textWidth, dim);
                t.origin = { (float)margin, (float)y };
                const auto h = (int)std::ceil (t.layout.getHeight());
                l.bounds = { margin, y, width - 2 * margin, h };
                l.texts.push_back (std::move (t));
                y += h + 12;
            }
            break;
        }
        case BlockKind::table:
        {
            size_t columns = 0;
            for (auto& row : block.rows)
                columns = juce::jmax (columns, row.size());
            if (columns == 0)
                break;
            constexpr float padX = 8.0f, padY = 5.0f, tableSize = 13.5f;
            // Each column as wide as its widest cell on one line, if they all fit; otherwise each at least as wide
            // as its longest word, and the room left shared by how much more each would like.
            auto& [natural, longestWord] = columnWidths[b];
            if (natural.empty())
            {
                natural.assign (columns, 30.0f);
                longestWord.assign (columns, 30.0f);
                for (size_t r = 0; r < block.rows.size(); ++r)
                    for (size_t c = 0; c < block.rows[r].size(); ++c)
                    {
                        auto t = makeText (block.rows[r][c], tableSize, 100000.0f, text, r == 0);
                        natural[c] = juce::jmax (natural[c], t.layout.getWidth() + 2 * padX + 2);
                        const juce::Font font (juce::FontOptions (tableSize, r == 0 ? juce::Font::bold : 0));
                        for (auto& word : juce::StringArray::fromTokens (plainText (block.rows[r][c]), " ", {}))
                            longestWord[c] =
                                juce::jmax (longestWord[c],
                                            juce::GlyphArrangement::getStringWidth (font, word) * 1.05f + 2 * padX + 2);
                    }
                for (size_t c = 0; c < columns; ++c)
                    longestWord[c] = juce::jmin (longestWord[c], natural[c]);
            }
            const float available = textWidth;
            float totalNatural = 0, totalLongest = 0;
            std::vector<float> least (columns); // a very long word (a checksum) is cut off rather than squeeze the rest
            for (size_t c = 0; c < columns; ++c)
            {
                least[c] = juce::jmin (longestWord[c], available * 0.3f);
                totalNatural += natural[c];
                totalLongest += least[c];
            }
            std::vector<float> widths = natural;
            if (totalNatural > available)
                for (size_t c = 0; c < columns; ++c)
                    widths[c] = totalLongest >= available
                                    ? least[c] * available / totalLongest
                                    : least[c] + (available - totalLongest) * (natural[c] - least[c]) /
                                                     juce::jmax (1.0f, totalNatural - totalLongest);

            float rowY = (float)y;
            for (size_t r = 0; r < block.rows.size(); ++r)
            {
                float x = (float)margin, rowHeight = 0;
                std::vector<Text> rowTexts;
                for (size_t c = 0; c < columns; ++c)
                {
                    const auto& cell = c < block.rows[r].size() ? block.rows[r][c] : Spans {};
                    auto t = makeText (cell, tableSize, widths[c] - 2 * padX, text, r == 0);
                    t.origin = { x + padX, rowY + padY };
                    rowHeight = juce::jmax (rowHeight, t.layout.getHeight() + 2 * padY);
                    x += widths[c];
                    rowTexts.push_back (std::move (t));
                }
                x = (float)margin;
                for (size_t c = 0; c < columns; ++c)
                {
                    l.cells.push_back (juce::Rectangle<float> (x, rowY, widths[c], rowHeight).toNearestInt());
                    x += widths[c];
                    l.texts.push_back (std::move (rowTexts[c]));
                }
                if (r == 0)
                    l.headerHeight = (int)std::ceil (rowHeight);
                rowY += rowHeight;
            }
            float tableWidth = 0;
            for (auto w : widths)
                tableWidth += w;
            l.bounds = { margin, y, (int)std::ceil (tableWidth), (int)std::ceil (rowY) - y };
            y = (int)std::ceil (rowY) + 14;
            break;
        }
        }
        laid.push_back (std::move (l));
    }
    setSize (width, y + 24);
    repaint();
}

//==============================================================================
void View::paint (juce::Graphics& g)
{
    const auto clip = g.getClipBounds();
    for (size_t b = 0; b < blocks.size() && b < laid.size(); ++b)
    {
        const auto& block = blocks[b];
        const auto& l = laid[b];
        if (! l.bounds.expanded (0, 20).intersects (clip))
            continue;

        switch (block.kind)
        {
        case BlockKind::heading:
            if (flashed.isNotEmpty() && block.anchor == flashed && flashAlpha > 0.0f)
            {
                g.setColour (colour ("accent").withAlpha (0.22f * flashAlpha));
                g.fillRoundedRectangle (l.bounds.reduced (margin - 8, 0).toFloat(), 4.0f);
            }
            if (block.level <= 2)
            {
                g.setColour (colour ("border").interpolatedWith (colour ("textDim"), 0.25f));
                g.fillRect (margin, l.bounds.getBottom() - 6, width - 2 * margin, 1);
            }
            break;
        case BlockKind::quote:
            g.setColour (colour ("accent").withAlpha (0.7f));
            g.fillRect (l.bounds.getX(), l.bounds.getY() - 2, 3, l.bounds.getHeight() + 4);
            break;
        case BlockKind::listItem:
        {
            const auto& t = l.texts.front();
            g.setColour (colour ("textDim"));
            const auto firstLine = t.layout.getNumLines() > 0 ? t.layout.getLine (0).lineOrigin.y : bodySize;
            if (block.ordered)
            {
                g.setFont (juce::FontOptions (bodySize));
                g.drawText (
                    juce::String (block.number) + ".",
                    juce::Rectangle<float> (t.origin.x - 30, t.origin.y + firstLine - bodySize, 26, bodySize + 4),
                    juce::Justification::centredRight);
            }
            else
            {
                const auto r = block.level == 0 ? 2.6f : 2.2f;
                const juce::Point<float> centre (t.origin.x - 12, t.origin.y + firstLine - bodySize * 0.33f);
                if (block.level == 0)
                    g.fillEllipse (juce::Rectangle<float> (2 * r, 2 * r).withCentre (centre));
                else
                    g.drawEllipse (juce::Rectangle<float> (2 * r, 2 * r).withCentre (centre), 1.0f);
            }
            break;
        }
        case BlockKind::code:
            g.setColour (colour ("background"));
            g.fillRoundedRectangle (l.bounds.toFloat(), 5.0f);
            g.setColour (colour ("border"));
            g.drawRoundedRectangle (l.bounds.toFloat().reduced (0.5f), 5.0f, 1.0f);
            break;
        case BlockKind::rule:
            g.setColour (colour ("border").interpolatedWith (colour ("textDim"), 0.25f));
            g.fillRect (l.bounds);
            break;
        case BlockKind::image:
            if (l.image.isValid())
            {
                g.drawImage (l.image, l.bounds.toFloat(), juce::RectanglePlacement::stretchToFit);
                g.setColour (colour ("border"));
                g.drawRect (l.bounds.expanded (1));
            }
            break;
        case BlockKind::table:
            if (! l.cells.empty())
            {
                g.setColour (colour ("panelHeader"));
                g.fillRect (l.bounds.withHeight (l.headerHeight));
                const auto line = colour ("border").interpolatedWith (colour ("textDim"), 0.2f);
                g.setColour (line);
                for (auto& cell : l.cells)
                    g.drawRect (cell);
            }
            break;
        case BlockKind::paragraph:
            break;
        }

        for (size_t i = 0; i < l.texts.size(); ++i)
        {
            const auto& t = l.texts[i];
            const juce::Graphics::ScopedSaveState state (g);
            if (i < l.cells.size())
                g.reduceClipRegion (l.cells[i].reduced (1)); // a word too long for its cell stays inside it
            t.layout.draw (g, { t.origin.x, t.origin.y, t.layout.getWidth() + 1, t.layout.getHeight() + 1 });
        }
    }
}

int View::getAnchorY (const juce::String& anchor) const
{
    for (size_t b = 0; b < blocks.size() && b < laid.size(); ++b)
        if (blocks[b].kind == BlockKind::heading && blocks[b].anchor == anchor)
            return laid[b].bounds.getY();
    return -1;
}

juce::String View::getAnchorAbove (int y) const
{
    juce::String anchor;
    for (size_t b = 0; b < blocks.size() && b < laid.size(); ++b)
        if (blocks[b].kind == BlockKind::heading)
        {
            if (laid[b].bounds.getY() > y)
                break;
            anchor = blocks[b].anchor;
        }
    return anchor;
}

juce::String View::getLinkAt (juce::Point<float> p) const
{
    for (auto& l : laid)
    {
        if (! l.bounds.toFloat().expanded (4).contains (p))
            continue;
        for (auto& t : l.texts)
        {
            if (t.links.empty())
                continue;
            const auto local = p - t.origin;
            for (int i = 0; i < t.layout.getNumLines(); ++i)
            {
                const auto& line = t.layout.getLine (i);
                if (! line.getLineBoundsY().expanded (1.0f).contains (local.y))
                    continue;
                for (auto* run : line.runs)
                {
                    if (! (run->getRunBoundsX() + line.lineOrigin.x).expanded (1.0f).contains (local.x))
                        continue;
                    for (auto& [range, target] : t.links)
                        if (range.contains (run->stringRange.getStart()))
                            return target;
                }
            }
        }
    }
    return {};
}

void View::mouseMove (const juce::MouseEvent& e)
{
    const auto link = getLinkAt (e.position);
    setMouseCursor (link.isNotEmpty() ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    setHelpText (link);
}

void View::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown() || e.mods.isPopupMenu())
        return;
    if (const auto link = getLinkAt (e.position); link.isNotEmpty() && onLink)
        onLink (link);
}

void View::flashAnchor (const juce::String& anchor)
{
    flashed = anchor;
    flashAlpha = 1.0f;
    startTimerHz (30);
    repaint();
}

void View::timerCallback()
{
    flashAlpha -= 1.0f / 60.0f; // two seconds
    if (flashAlpha <= 0.0f)
    {
        flashAlpha = 0.0f;
        stopTimer();
    }
    repaint();
}

int View::getNumImagesLoaded() const
{
    int n = 0;
    for (auto& l : laid)
        n += l.image.isValid() ? 1 : 0;
    return n;
}

} // namespace spinus::ui::markdown

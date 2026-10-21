#pragma once

#include <optional>
#include <string>
#include <utf8.h>
#include <utility>
#include <vector>

namespace rwe
{
    using ConstUtf8UncheckedIterator = utf8::unchecked::iterator<std::string::const_iterator>;
    using Utf8UncheckedIterator = utf8::unchecked::iterator<std::string::iterator>;

    using ConstUtf8Iterator = utf8::iterator<std::string::const_iterator>;
    using Utf8Iterator = utf8::iterator<std::string::iterator>;

    using ReverseConstUtf8Iterator = std::reverse_iterator<ConstUtf8Iterator>;
    using ReverseUtf8Iterator = std::reverse_iterator<Utf8Iterator>;

    std::vector<std::string> utf8Split(const std::string& str, const std::vector<unsigned int>& codePoints);
    std::vector<std::string> utf8Split(const std::string& str, unsigned int codePoint);
    std::optional<std::pair<std::string, std::string>> utf8SplitLast(const std::string& str, unsigned int codePoint);

    std::vector<std::string> split(const std::string& str, const std::vector<char>& codePoints);
    std::vector<std::string> split(const std::string& str, char codePoint);

    std::string toUpper(const std::string& str);

    std::string toLower(const std::string& str);

    ConstUtf8UncheckedIterator cUtf8UncheckedBegin(const std::string& str);
    ConstUtf8UncheckedIterator cUtf8UncheckedEnd(const std::string& str);
    Utf8UncheckedIterator utf8UncheckedBegin(const std::string& str);
    Utf8UncheckedIterator utf8UncheckedEnd(const std::string& str);

    Utf8Iterator utf8Begin(std::string& str);
    ConstUtf8Iterator utf8Begin(const std::string& str);
    ConstUtf8Iterator cUtf8Begin(const std::string& str);

    Utf8Iterator utf8End(std::string& str);
    ConstUtf8Iterator utf8End(const std::string& str);
    ConstUtf8Iterator cUtf8End(const std::string& str);

    void utf8TrimLeft(std::string& str);
    void utf8TrimRight(std::string& str);
    void utf8Trim(std::string& str);

    void utf8UncheckedTrimLeft(std::string& str);
    void utf8UncheckedTrimRight(std::string& str);
    void utf8UncheckedTrim(std::string& str);

    bool startsWith(const std::string& str, const std::string& prefix);
    bool endsWith(const std::string& str, const std::string& end);
    bool endsWithUtf8(const std::string& str, const std::string& end);

    std::string latin1ToUtf8(const std::string& str);

    /**
     * Splits `text` into display lines for wrapped text rendering.
     * '\n' is a hard line break (a '\n'-terminated line produces a
     * following empty line; an empty hard line produces an empty entry,
     * preserving authored blank lines). Each hard line is word-wrapped
     * on spaces so that no emitted line exceeds `maxWidth`, measured by
     * `measure(substring)`. A word wider than `maxWidth` stays whole on
     * its own line. Splitting is byte-oriented: ' ' and '\n' cannot
     * occur inside a multi-byte UTF-8 sequence, so this is UTF-8 safe.
     */
    template <typename F>
    std::vector<std::string> wrapTextLines(const std::string& text, float maxWidth, F&& measure)
    {
        const auto spaceWidth = measure(" ");

        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (true)
        {
            auto newline = text.find('\n', pos);
            auto hardEnd = newline == std::string::npos ? text.size() : newline;

            std::string current;
            float currentWidth = 0.0f;
            std::size_t i = pos;
            while (i < hardEnd)
            {
                auto space = text.find(' ', i);
                if (space == std::string::npos || space > hardEnd)
                {
                    space = hardEnd;
                }
                auto wordWidth = measure(text.substr(i, space - i));
                if (!current.empty() && currentWidth + wordWidth > maxWidth)
                {
                    lines.push_back(current);
                    current.clear();
                    currentWidth = 0.0f;
                }
                current.append(text, i, space - i);
                currentWidth += wordWidth;
                if (space < hardEnd)
                {
                    current += ' ';
                    currentWidth += spaceWidth;
                }
                i = space + 1;
            }
            lines.push_back(current);

            if (newline == std::string::npos)
            {
                break;
            }
            pos = newline + 1;
        }
        return lines;
    }

    /**
     * Groups wrapped lines into pages of `linesPerPage` lines each,
     * joined by newlines for re-rendering through a wrapping label.
     * Always returns at least one (possibly empty) page, matching the
     * retail in-game briefing pager's behaviour on empty text.
     */
    inline std::vector<std::string> paginateLines(const std::vector<std::string>& lines, unsigned int linesPerPage)
    {
        std::vector<std::string> pages;
        std::string current;
        unsigned int count = 0;
        for (const auto& line : lines)
        {
            if (count == linesPerPage)
            {
                pages.push_back(current);
                current.clear();
                count = 0;
            }
            if (!current.empty())
            {
                current += '\n';
            }
            current += line;
            ++count;
        }
        pages.push_back(current);
        return pages;
    }
}

#include <catch2/catch_test_macros.hpp>

#include <rwe/util/rwe_string.h>

namespace rwe
{
    TEST_CASE("utf8Split")
    {
        SECTION("works on empty string")
        {
            std::string s;
            std::vector<std::string> expected{""};

            auto actual = utf8Split(s, '/');

            REQUIRE(actual == expected);
        }

        SECTION("splits a utf8 string on a delimiter")
        {
            std::string s("foo/bar/baz");
            std::vector<std::string> expected{"foo", "bar", "baz"};

            auto actual = utf8Split(s, '/');

            REQUIRE(actual == expected);
        }

        SECTION("works when the delimiter is not found")
        {
            std::string s("foo.bar.baz");
            std::vector<std::string> expected{"foo.bar.baz"};

            auto actual = utf8Split(s, '/');

            REQUIRE(actual == expected);
        }
    }

    TEST_CASE("split")
    {
        SECTION("works on empty string")
        {
            std::string s;
            std::vector<std::string> expected{""};

            auto actual = split(s, '/');

            REQUIRE(actual == expected);
        }

        SECTION("splits a utf8 string on a delimiter")
        {
            std::string s("foo/bar/baz");
            std::vector<std::string> expected{"foo", "bar", "baz"};

            auto actual = split(s, '/');

            REQUIRE(actual == expected);
        }

        SECTION("works when the delimiter is not found")
        {
            std::string s("foo.bar.baz");
            std::vector<std::string> expected{"foo.bar.baz"};

            auto actual = split(s, '/');

            REQUIRE(actual == expected);
        }
    }

    TEST_CASE("toUpper")
    {
        SECTION("converts a string to uppercase")
        {
            REQUIRE(toUpper(std::string("Foo bAr baZ")) == std::string("FOO BAR BAZ"));
        }
    }

    TEST_CASE("utf8TrimChecked")
    {
        SECTION("trims leading and trailing spaces")
        {
            std::string s("    foo  ");
            utf8Trim(s);
            REQUIRE(s == "foo");
        }
    }

    TEST_CASE("utf8TrimUnchecked")
    {
        SECTION("trims leading and trailing spaces")
        {
            std::string s("    foo  ");
            utf8UncheckedTrim(s);
            REQUIRE(s == "foo");
        }
    }

    TEST_CASE("endsWith")
    {
        SECTION("returns true if the string ends with the substring")
        {
            std::string s("foo.txt");
            std::string e(".txt");
            REQUIRE(endsWith(s, e));
        }

        SECTION("returns false otherwise")
        {
            std::string s("foottxt");
            std::string e(".txt");
            REQUIRE(!endsWith(s, e));
        }

        SECTION("returns false when the string is shorter")
        {
            std::string s("txt");
            std::string e(".txt");
            REQUIRE(!endsWith(s, e));
        }
    }

    TEST_CASE("endsWithUtf8")
    {
        SECTION("returns true if the string ends with the substring")
        {
            std::string s("foo.txt");
            std::string e(".txt");
            REQUIRE(endsWithUtf8(s, e));
        }

        SECTION("returns false otherwise")
        {
            std::string s("foottxt");
            std::string e(".txt");
            REQUIRE(!endsWithUtf8(s, e));
        }

        SECTION("returns false when the string is shorter")
        {
            std::string s("txt");
            std::string e(".txt");
            REQUIRE(!endsWithUtf8(s, e));
        }
    }

    TEST_CASE("startsWith")
    {
        SECTION("returns true if the string starts with the substring")
        {
            std::string s("foo.txt");
            std::string p("foo");
            REQUIRE(startsWith(s, p));
        }

        SECTION("returns false otherwise")
        {
            std::string s("fobar.txt");
            std::string p("foo");
            REQUIRE(!startsWith(s, p));
        }

        SECTION("returns false when the string is shorter")
        {
            std::string s("foo");
            std::string p("fooo");
            REQUIRE(!startsWith(s, p));
        }
    }

    TEST_CASE("utf8SplitLast")
    {
        SECTION("splits the last occurrence of a character")
        {
            REQUIRE(utf8SplitLast("foo.bar.baz", U'.').value() == std::make_pair(std::string("foo.bar"), std::string("baz")));
            REQUIRE(!utf8SplitLast("foo:bar:baz", U'.'));
        }
    }

    TEST_CASE("wrapTextLines")
    {
        // uniform 10px characters for predictable wrap points
        auto measure = [](const std::string& s) {
            return static_cast<float>(s.size()) * 10.0f;
        };

        SECTION("preserves explicit newlines as line breaks")
        {
            auto lines = wrapTextLines("one\ntwo\nthree", 1000.0f, measure);
            REQUIRE(lines == std::vector<std::string>{"one", "two", "three"});
        }

        SECTION("preserves blank lines")
        {
            auto lines = wrapTextLines("one\n\nthree", 1000.0f, measure);
            REQUIRE(lines == std::vector<std::string>{"one", "", "three"});
        }

        SECTION("emits no line containing a newline")
        {
            auto lines = wrapTextLines("a\nb", 1000.0f, measure);
            for (const auto& line : lines)
            {
                REQUIRE(line.find('\n') == std::string::npos);
            }
        }

        SECTION("wraps words that exceed the width")
        {
            // "aaa" + space + "bbb" = 70px; limit 40px forces the break
            auto lines = wrapTextLines("aaa bbb", 40.0f, measure);
            REQUIRE(lines.size() == 2);
            REQUIRE(lines[0] == "aaa ");
            REQUIRE(lines[1] == "bbb");
        }

        SECTION("keeps a word wider than the limit on its own line")
        {
            auto lines = wrapTextLines("abcdefghij x", 40.0f, measure);
            REQUIRE(lines.size() == 2);
            REQUIRE(lines[0] == "abcdefghij ");
            REQUIRE(lines[1] == "x");
        }

        SECTION("wraps each hard line independently")
        {
            auto lines = wrapTextLines("aaa bbb\nccc ddd", 40.0f, measure);
            REQUIRE(lines == std::vector<std::string>{"aaa ", "bbb", "ccc ", "ddd"});
        }

        SECTION("empty input produces one empty line")
        {
            auto lines = wrapTextLines("", 100.0f, measure);
            REQUIRE(lines == std::vector<std::string>{""});
        }
    }

    TEST_CASE("paginateLines")
    {
        SECTION("groups lines into fixed-size pages")
        {
            auto pages = paginateLines({"a", "b", "c", "d", "e"}, 2);
            REQUIRE(pages == std::vector<std::string>{"a\nb", "c\nd", "e"});
        }

        SECTION("empty input still yields one page")
        {
            auto pages = paginateLines({}, 9);
            REQUIRE(pages == std::vector<std::string>{""});
        }

        SECTION("exact fit does not add a trailing page")
        {
            auto pages = paginateLines({"a", "b"}, 2);
            REQUIRE(pages == std::vector<std::string>{"a\nb"});
        }
    }
}

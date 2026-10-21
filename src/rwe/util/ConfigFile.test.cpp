#include <catch2/catch_test_macros.hpp>
#include <fstream>

#include <rwe/util/ConfigFile.h>
#include <rwe/util/OpaqueArgs.h>

namespace rwe
{
    namespace
    {
        struct TempDir
        {
            std::filesystem::path path;

            TempDir()
            {
                path = std::filesystem::temp_directory_path() / "rwe_configfile_test";
                std::filesystem::remove_all(path);
                std::filesystem::create_directories(path);
            }

            ~TempDir()
            {
                std::filesystem::remove_all(path);
            }
        };
    }

    TEST_CASE("readConfigFile")
    {
        TempDir dir;

        SECTION("returns empty for a missing file")
        {
            auto values = readConfigFile(dir.path / "missing.cfg");
            REQUIRE(values);
            REQUIRE(values->empty());
        }

        SECTION("parses key=value lines and skips comments")
        {
            auto file = dir.path / "test.cfg";
            {
                std::ofstream out(file);
                out << "# comment\n\ninterface-mode = right-click\n";
            }
            auto values = readConfigFile(file);
            REQUIRE(values);
            REQUIRE(values->at("interface-mode") == "right-click");
        }

        SECTION("returns nullopt on a malformed line")
        {
            auto file = dir.path / "bad.cfg";
            {
                std::ofstream out(file);
                out << "interface-mode=right-click\nbroken line\n";
            }
            REQUIRE(!readConfigFile(file));
        }
    }

    TEST_CASE("writeConfigValue")
    {
        TempDir dir;

        SECTION("creates the file when missing")
        {
            auto file = dir.path / "new.cfg";
            writeConfigValue(file, "interface-mode", "right-click");
            auto values = readConfigFile(file);
            REQUIRE(values);
            REQUIRE(values->at("interface-mode") == "right-click");
        }

        SECTION("updates an existing key and preserves other lines")
        {
            auto file = dir.path / "upd.cfg";
            {
                std::ofstream out(file);
                out << "# my config\ninterface-mode=right-click\nother=keep\n";
            }
            writeConfigValue(file, "interface-mode", "left-click");
            auto values = readConfigFile(file);
            REQUIRE(values);
            REQUIRE(values->at("interface-mode") == "left-click");
            REQUIRE(values->at("other") == "keep");
            std::ifstream in(file);
            std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            REQUIRE(contents.find("# my config") != std::string::npos);
        }

        SECTION("round-trips repeatedly")
        {
            auto file = dir.path / "rt.cfg";
            writeConfigValue(file, "interface-mode", "right-click");
            writeConfigValue(file, "interface-mode", "left-click");
            writeConfigValue(file, "interface-mode", "right-click");
            REQUIRE(readConfigFile(file)->at("interface-mode") == "right-click");
        }
    }

    TEST_CASE("config file + CLI precedence")
    {
        TempDir dir;
        auto file = dir.path / "precedence.cfg";
        writeConfigValue(file, "interface-mode", "right-click");

        // mirror main.cpp: CLI args parse first, saved config fills
        // only keys not set on the command line
        auto feed = [&](OpaqueArgs& args) {
            auto saved = readConfigFile(file);
            REQUIRE(saved);
            std::ostringstream stream;
            for (const auto& [k, v] : *saved)
            {
                stream << k << "=" << v << "\n";
            }
            auto text = stream.str();
            std::istringstream input(text);
            args.parseConfig(input);
        };

        SECTION("persisted value applies when CLI omits the key")
        {
            OpaqueArgs args;
            args.parse(0, nullptr);
            feed(args);
            REQUIRE(args.getString("interface-mode", "left-click") == "right-click");
        }

        SECTION("explicit CLI override wins over the saved value")
        {
            const char* argv[] = {"rwe", "--interface-mode", "left-click"};
            OpaqueArgs args;
            args.parse(3, const_cast<char**>(argv));
            feed(args);
            REQUIRE(args.getString("interface-mode", "right-click") == "left-click");
        }
    }
}

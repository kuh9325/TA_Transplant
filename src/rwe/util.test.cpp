#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <rwe/util.h>

namespace fs = std::filesystem;

namespace rwe
{
    namespace
    {
        struct TempDir
        {
            fs::path path;
            TempDir()
            {
                path = fs::temp_directory_path() / "rwe_util_test";
                fs::remove_all(path);
                fs::create_directories(path);
            }
            ~TempDir() { fs::remove_all(path); }
        };

        void touchFile(const fs::path& dir, const std::string& name)
        {
            std::ofstream(dir / name).put('x');
        }
    }

    TEST_CASE("isPlausibleGameDataRoot")
    {
        TempDir root;

        SECTION("missing directory is implausible")
        {
            REQUIRE(!isPlausibleGameDataRoot(root.path / "no-such-dir"));
        }

        SECTION("empty directory is implausible")
        {
            REQUIRE(!isPlausibleGameDataRoot(root.path));
        }

        SECTION("a directory of unrelated files is implausible")
        {
            touchFile(root.path, "readme.txt");
            touchFile(root.path, "setup.exe");
            REQUIRE(!isPlausibleGameDataRoot(root.path));
        }

        SECTION("a TA archive file makes it plausible")
        {
            touchFile(root.path, "totala1.hpi");
            REQUIRE(isPlausibleGameDataRoot(root.path));
        }

        SECTION("archive extension match is case-insensitive")
        {
            touchFile(root.path, "BTDATA.CCX");
            REQUIRE(isPlausibleGameDataRoot(root.path));
        }

        SECTION("ufo/gp3/gpf extensions count")
        {
            TempDir ufo;
            touchFile(ufo.path, "cdmaps.ufo");
            REQUIRE(isPlausibleGameDataRoot(ufo.path));

            TempDir gp3;
            touchFile(gp3.path, "rev31.gp3");
            REQUIRE(isPlausibleGameDataRoot(gp3.path));

            TempDir gpf;
            touchFile(gpf.path, "revision.gpf");
            REQUIRE(isPlausibleGameDataRoot(gpf.path));
        }

        SECTION("loose-file layout needs palettes + gamedata dirs")
        {
            fs::create_directories(root.path / "PALETTES");
            REQUIRE(!isPlausibleGameDataRoot(root.path));
            fs::create_directories(root.path / "Gamedata");
            REQUIRE(isPlausibleGameDataRoot(root.path));
        }

        SECTION("a regular file is implausible")
        {
            auto file = root.path / "notadir";
            touchFile(root.path, "notadir");
            REQUIRE(!isPlausibleGameDataRoot(file));
        }
    }
}

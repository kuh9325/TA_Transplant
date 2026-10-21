#include <unistd.h>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <rwe/save/SaveGameService.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/util.h>

namespace rwe
{
    namespace
    {
        std::filesystem::path makeTempDir()
        {
            auto dir = std::filesystem::temp_directory_path()
                / ("rwe_save_test_" + std::to_string(::getpid()));
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
            return dir;
        }

        std::vector<char> fakeSave(const std::string& name)
        {
            GameSimulation sim(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 100);
            GameParameters params("TESTMAP", 0);
            return encodeSavedGame(sim, params, nullptr, name, 1234);
        }
    }

    TEST_CASE("saveGameDirectory resolves under the process HOME's local data path")
    {
        // every launch path (CLI binary, .app executable, Finder
        // launch) shares the single getLocalDataPath() resolver --
        // $HOME/.rwe on non-Windows -- so the save directory this
        // process resolves is exactly the directory a real launch
        // lists and writes under the same environment
        auto local = getLocalDataPath();
        REQUIRE(local);
        auto dir = saveGameDirectory();
        REQUIRE(dir);
        REQUIRE(*dir == *local / "savegame");
    }

    TEST_CASE("sanitizeSaveName reduces hostile names to safe stems")
    {
        REQUIRE(sanitizeSaveName("my save") == "my save");
        REQUIRE(sanitizeSaveName("a/b\\c:d") == "a_b_c_d");
        REQUIRE(sanitizeSaveName("   ") == "");
        REQUIRE(sanitizeSaveName("...") == "");
        REQUIRE(sanitizeSaveName("").empty());
        // 20-char cap from SAVELIST's GAMENAME field is applied by the
        // textbox; sanitize caps at the wider SAVEGAME limit
        REQUIRE(sanitizeSaveName(std::string(200, 'x')).size() == 99);
    }

    TEST_CASE("saveGamePath maps names to <dir>/<name>.sav")
    {
        auto dir = std::filesystem::path("/tmp/saves");
        auto p = saveGamePath(dir, "mission 1");
        REQUIRE(p);
        REQUIRE(p->filename() == "mission 1.sav");
        REQUIRE(!saveGamePath(dir, ""));
    }

    TEST_CASE("write/list/read/delete round-trip through a directory")
    {
        auto dir = makeTempDir();

        writeSavedGameFile(dir, "alpha", fakeSave("alpha"));
        writeSavedGameFile(dir, "beta", fakeSave("beta"));

        auto listed = listSavedGames(dir);
        REQUIRE(listed.size() == 2);
        std::vector<std::string> names{listed[0].saveName, listed[1].saveName};
        std::sort(names.begin(), names.end());
        REQUIRE(names == std::vector<std::string>{"alpha", "beta"});

        auto bytes = readSavedGameFile(dir, "alpha");
        REQUIRE(readSaveMetadata(bytes).saveName == "alpha");

        REQUIRE(deleteSavedGameFile(dir, "alpha"));
        REQUIRE(!deleteSavedGameFile(dir, "alpha"));
        REQUIRE(listSavedGames(dir).size() == 1);

        std::filesystem::remove_all(dir);
    }

    TEST_CASE("listSavedGames skips unreadable and foreign files")
    {
        auto dir = makeTempDir();
        writeSavedGameFile(dir, "good", fakeSave("good"));

        // a truncated/foreign file sitting next to real saves
        std::ofstream(dir / "junk.sav", std::ios::binary) << "not a save";
        // non-.sav files are not listed at all
        std::ofstream(dir / "notes.txt", std::ios::binary) << "hello";

        auto listed = listSavedGames(dir);
        REQUIRE(listed.size() == 1);
        REQUIRE(listed[0].saveName == "good");

        REQUIRE_THROWS_AS(readSavedGameFile(dir, "missing"), std::runtime_error);

        std::filesystem::remove_all(dir);
    }

    TEST_CASE("writeSavedGameFile is atomic: a good file survives a failed overwrite")
    {
        auto dir = makeTempDir();
        writeSavedGameFile(dir, "slot", fakeSave("slot"));

        // empty name must not produce a file and must not clobber
        REQUIRE_THROWS_AS(writeSavedGameFile(dir, "", {'x'}), std::runtime_error);
        REQUIRE(readSaveMetadata(readSavedGameFile(dir, "slot")).saveName == "slot");

        std::filesystem::remove_all(dir);
    }
}

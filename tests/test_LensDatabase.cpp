#include "support/LensFixtures.h"
#include "support/Lensfun.h"
#include "support/TempDir.h"

#include <LensDatabase.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace arraw;

namespace {

/// @brief Gives the models of the lenses a database lists.
std::vector<std::string> modelsIn(const LensDatabase& database) {
    std::vector<std::string> models;
    for (const LensEntry& lens : database.lenses("", std::nullopt)) {
        models.push_back(lens.model);
    }
    return models;
}

/// @brief Makes a directory under a temporary one.
std::filesystem::path made(const test::TempDir& root, const char* name) {
    const auto path = root.file(name);
    std::filesystem::create_directories(path);
    return path;
}

} // namespace

TEST_CASE("A build without lensfun has a database that says so", "[lens]") {
    if (lensfunAvailable()) {
        SUCCEED("this build has lensfun");
        return;
    }
    const auto database = LensDatabase::fromDirectory(test::fixture("lensfun"));
    CHECK_FALSE(database.loaded());
    CHECK(database.problem() == LensProblem::LensfunUnavailable);
    CHECK(database.lensCount() == 0);
    CHECK_FALSE(database.findCamera("Canon", "Canon EOS 80D"));
    CHECK(database.findLenses("Canon EF 50mm f/1.8 STM").empty());
    CHECK(std::string(lensfunVersion()).empty());
}

TEST_CASE("A directory of lensfun files loads, with its counts and stamp", "[lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    REQUIRE(database.loaded());
    CHECK_FALSE(database.problem());
    CHECK(database.cameraCount() == 4);
    CHECK(database.lensCount() == 8);
    // 1700000000 in timestamp.txt.
    CHECK(database.stamp() == "2023-11-14T22:13:20Z");
    CHECK(database.directory() == test::fixture("lensfun"));
    CHECK(std::string(lensfunVersion()).starts_with("0.3"));
}

TEST_CASE("Without a timestamp file the stamp is the newest file's time", "[lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir root;
    test::writeTinyDatabase(root.path(), "Test 50mm", 1);
    std::filesystem::remove(root.file("timestamp.txt"));
    const auto database = LensDatabase::fromDirectory(root.path());
    REQUIRE(database.loaded());
    // Written just now.
    CHECK(database.stamp().starts_with("20"));
    CHECK(database.stamp().size() == std::string("2023-11-14T22:13:20Z").size());
    CHECK(database.stamp() != "1970-01-01T00:00:01Z");
}

TEST_CASE("A missing directory gives a database that says so, and does not throw", "[lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir root;
    for (const auto& path : {root.file("absent"), root.path()}) {
        CAPTURE(path);
        const auto database = LensDatabase::fromDirectory(path);
        CHECK_FALSE(database.loaded());
        CHECK(database.problem() == LensProblem::LensDatabaseMissing);
        CHECK(database.lensCount() == 0);
        CHECK(database.cameraCount() == 0);
        CHECK(database.stamp().empty());
        CHECK_FALSE(database.findCamera("Canon", "Canon EOS 80D"));
        CHECK(database.findLenses("Canon EF 50mm f/1.8 STM").empty());
        CHECK(database.lenses("", std::nullopt).empty());
    }
}

TEST_CASE("The database is looked for explicit, then bundled, then system", "[lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir root;
    const auto explicitDirectory = made(root, "explicit");
    const auto bundled = made(root, "bundled");
    const auto bundledToo = made(root, "bundled-too");
    const auto system = made(root, "system");
    test::writeTinyDatabase(explicitDirectory, "Explicit", 100);
    test::writeTinyDatabase(bundled, "Bundled", 100);
    test::writeTinyDatabase(bundledToo, "Bundled too", 100);
    test::writeTinyDatabase(system, "System", 100);

    LensDatabaseSearch search;
    search.explicitDirectory = explicitDirectory;
    search.bundledDirectories = {root.file("no-such"), bundled, bundledToo};
    search.systemDirectories = {system};

    SECTION("an explicit directory beats the rest") {
        CHECK(modelsIn(LensDatabase::fromSearch(search)) == std::vector<std::string>{"Explicit"});
    }
    SECTION("a bundled one beats the system's, the first that exists") {
        search.explicitDirectory.reset();
        const auto database = LensDatabase::fromSearch(search);
        CHECK(modelsIn(database) == std::vector<std::string>{"Bundled"});
        CHECK(database.directory() == bundled);
    }
    SECTION("the system's is used when nothing else is there") {
        search.explicitDirectory.reset();
        search.bundledDirectories = {root.file("no-such")};
        CHECK(modelsIn(LensDatabase::fromSearch(search)) == std::vector<std::string>{"System"});
    }
    SECTION("an explicit directory that is missing is not replaced by another") {
        search.explicitDirectory = root.file("absent");
        const auto database = LensDatabase::fromSearch(search);
        CHECK_FALSE(database.loaded());
        CHECK(database.problem() == LensProblem::LensDatabaseMissing);
    }
    SECTION("nothing found is a database that says so") {
        search.explicitDirectory.reset();
        search.bundledDirectories.clear();
        search.systemDirectories.clear();
        CHECK(LensDatabase::fromSearch(search).problem() == LensProblem::LensDatabaseMissing);
    }
}

TEST_CASE("A user's update replaces a bundled database that is older, and only that one",
          "[lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir root;
    const auto bundled = made(root, "bundled");
    const auto update = made(root, "update");
    test::writeTinyDatabase(bundled, "Bundled", 2000);

    LensDatabaseSearch search;
    search.bundledDirectories = {bundled};
    search.userUpdateDirectory = update;

    SECTION("newer wins") {
        test::writeTinyDatabase(update, "Updated", 3000);
        const auto database = LensDatabase::fromSearch(search);
        REQUIRE(database.loaded());
        // Loaded over the bundled copy: its lens replaces nothing by name, so both are listed.
        CHECK(database.lens("Test", "Updated"));
        CHECK(database.directory() == update);
        CHECK(database.stamp() == "1970-01-01T00:50:00Z");
    }
    SECTION("older is ignored") {
        test::writeTinyDatabase(update, "Stale", 1000);
        const auto database = LensDatabase::fromSearch(search);
        CHECK_FALSE(database.lens("Test", "Stale"));
        CHECK(database.lens("Test", "Bundled"));
        CHECK(database.directory() == bundled);
    }
    SECTION("an absent update changes nothing") {
        const auto database = LensDatabase::fromSearch(search);
        CHECK(modelsIn(database) == std::vector<std::string>{"Bundled"});
    }
}

TEST_CASE("The process's database is loaded once, however many threads ask", "[lensfun][system]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    std::vector<const LensDatabase*> seen(8, nullptr);
    {
        std::vector<std::jthread> threads;
        for (std::size_t index = 0; index < seen.size(); ++index) {
            threads.emplace_back([&seen, index] { seen[index] = &LensDatabase::system(); });
        }
    }
    CHECK(systemLensDatabaseLoads() == 1);
    CHECK(std::ranges::all_of(seen, [&](const LensDatabase* one) { return one == seen.front(); }));
    CHECK(&LensDatabase::system() == seen.front());
    CHECK(systemLensDatabaseLoads() == 1);
}

TEST_CASE("A camera is found by its whole model", "[lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();

    const auto camera = database.findCamera("Canon", "Canon EOS 80D");
    REQUIRE(camera);
    CHECK(camera->mount == "Canon EF-S");
    CHECK(camera->cropFactor == 1.6);
    CHECK(database.mountOf("Canon", "canon eos 80d") == "Canon EF-S");
    // Case and spacing do not matter; a model that is only part of another's does not find it.
    CHECK(database.findCamera("CANON", "Canon  EOS 80D"));
    CHECK_FALSE(database.findCamera("Canon", "Canon EOS 80"));
    CHECK_FALSE(database.findCamera("Canon", "Canon EOS 800D"));
    CHECK_FALSE(database.findCamera("Canon", ""));
    CHECK_FALSE(database.mountOf("Canon", "Canon EOS 800D"));
}

TEST_CASE("Lenses are listed for a picker by words and by the camera's mount", "[lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();

    CHECK(database.lenses("", std::nullopt).size() == 8);
    CHECK(database.lenses("sigma 17-50", std::nullopt).size() == 2);
    CHECK(database.lenses("50 SIGMA", std::nullopt).size() == 2);

    // EF-S fits EF-S and, by the mount's compatibility list, EF; not Nikon F.
    const auto onEfs = database.lenses("", std::string("Canon EF-S"));
    CHECK(onEfs.size() == 5);
    CHECK(std::ranges::none_of(onEfs, [](const LensEntry& lens) {
        return lens.model.find("Tamron") != std::string::npos;
    }));
    // EF does not fit an EF-S lens.
    const auto onEf = database.lenses("18-135", std::string("Canon EF"));
    CHECK(onEf.empty());
    CHECK(database.lenses("tamron", std::string("Nikon F")).size() == 1);

    const auto lens = database.lens("Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
    REQUIRE(lens);
    CHECK(lens->minFocal == 18.0);
    CHECK(lens->maxFocal == 135.0);
    CHECK(lens->widestAperture == 3.5);
    CHECK(lens->cropFactor == 1.6);
    CHECK(lens->mounts == std::vector<std::string>{"Canon EF-S"});
    CHECK_FALSE(database.lens("Canon", "Canon EF-S 18-135mm"));
}

#include "support/TempDir.h"

#include <ExportPresets.h>

#include <QLockFile>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#ifdef __unix__
#include <unistd.h>
#endif

using namespace arraw;
namespace fs = std::filesystem;

namespace {

/// @brief Reads a whole file.
std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief Writes a whole file, creating its directory.
void spit(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/// @brief Makes a preset whose quality tells it apart.
ExportPreset preset(const std::string& name, int quality = 90) {
    return {.name = name, .settings = {.quality = quality}};
}

/// @brief Runs a call and gives the problem it threw, or nothing.
template <typename F> std::optional<ExportPresetProblem> problemOf(F&& call) {
    try {
        call();
    } catch (const ExportPresetError& error) {
        return error.problem();
    }
    return std::nullopt;
}

/// @brief Finds a preset of the catalog by name.
const ExportPreset* find(const ExportPresetCatalog& catalog, const std::string& name) {
    for (const ExportPreset& candidate : catalog.presets) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

/// @brief The user's presets of a catalog: those that are not built in.
std::vector<std::string> userNames(const ExportPresetCatalog& catalog) {
    std::vector<std::string> names;
    for (const ExportPreset& candidate : catalog.presets) {
        if (!candidate.builtIn) {
            names.push_back(candidate.name);
        }
    }
    return names;
}

constexpr const char* newerFile =
    R"({"arraw": 2, "exportPresets": [{"name": "Keep", "quality": 40, "novel": true}]})";

} // namespace

TEST_CASE("A missing file reads as an empty catalog and is not created", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.file("config"));
    const ExportPresetCatalog catalog = store.read();
    CHECK(catalog.state == ExportPresetFileState::Missing);
    CHECK(userNames(catalog).empty());
    CHECK(catalog.unusable.empty());
    CHECK_FALSE(fs::exists(dir.file("config")));
    CHECK(store.file() == dir.file("config") / exportPresetsFileName);
    CHECK_FALSE(store.load("anything"));
}

TEST_CASE("Save creates the directory and the file", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path() / "a" / "b");
    store.save(preset("Mine", 70));
    CHECK(fs::is_regular_file(store.file()));
    const ExportPresetCatalog catalog = store.read();
    CHECK(catalog.state == ExportPresetFileState::Read);
    REQUIRE(find(catalog, "Mine"));
    CHECK(find(catalog, "Mine")->settings.quality == 70);
    // The lock is gone again.
    CHECK_FALSE(fs::exists(store.file().string() + ".lock"));
    CHECK_THROWS_AS(store.save({.name = "Bad", .settings = {.quality = 101}}),
                    std::invalid_argument);
}

TEST_CASE("Load finds a preset whatever the case of its name", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    store.save(preset("Web Small", 61));
    const auto found = store.load("  WEB small ");
    REQUIRE(found);
    CHECK(found->name == "Web Small");
    CHECK(found->settings.quality == 61);
    CHECK_FALSE(store.load("web"));
}

TEST_CASE("Saving a name that folds alike replaces it and takes the new spelling",
          "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    store.save(preset("Web", 10));
    store.save(preset("web", 20));
    const ExportPresetCatalog catalog = store.read();
    REQUIRE(userNames(catalog) == std::vector<std::string>{"web"});
    CHECK(find(catalog, "web")->settings.quality == 20);
    CHECK(std::ranges::count_if(catalog.presets, [](const auto& p) { return !p.builtIn; }) == 1);
}

TEST_CASE("The file is sorted by folded name", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    for (const char* name : {"zoo", "Beta", "alpha", "Charlie"}) {
        store.save(preset(name));
    }
    CHECK(userNames(store.read()) == std::vector<std::string>{"alpha", "Beta", "Charlie", "zoo"});
}

TEST_CASE("Rename refuses a name another preset holds; a change of case is allowed",
          "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    store.save(preset("One", 1));
    store.save(preset("Two", 2));
    CHECK(problemOf([&] { store.rename("One", "two"); }) == ExportPresetProblem::NameTaken);
    CHECK(problemOf([&] { store.rename("One", " "); }) == ExportPresetProblem::InvalidName);
    CHECK(problemOf([&] { store.rename("Nope", "Three"); }) == ExportPresetProblem::NotFound);
    store.rename("One", "ONE");
    store.rename("two", "Deux");
    const ExportPresetCatalog catalog = store.read();
    CHECK(userNames(catalog) == std::vector<std::string>{"Deux", "ONE"});
    CHECK(find(catalog, "ONE")->settings.quality == 1);
    CHECK(find(catalog, "Deux")->settings.quality == 2);
}

TEST_CASE("Remove of a missing preset says NotFound", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    CHECK(problemOf([&] { store.remove("Nothing"); }) == ExportPresetProblem::NotFound);
    store.save(preset("Here"));
    CHECK(problemOf([&] { store.remove("Nothing"); }) == ExportPresetProblem::NotFound);
    store.remove("here");
    CHECK(userNames(store.read()).empty());
}

TEST_CASE("Save refuses to replace an unusable preset; remove deletes it", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    spit(
        store.file(),
        R"({"arraw": 1, "exportPresets": [{"name": "Future", "format": "avif"}, {"name": "Ok"}]})");
    const ExportPresetCatalog catalog = store.read();
    REQUIRE(catalog.unusable.size() == 1);
    CHECK(catalog.unusable[0].name == "Future");
    CHECK(problemOf([&] { store.save(preset("future")); }) == ExportPresetProblem::Unusable);
    CHECK(problemOf([&] { store.rename("Future", "Past"); }) == ExportPresetProblem::Unusable);
    CHECK(problemOf([&] { (void)store.load("Future"); }) == ExportPresetProblem::Unusable);
    store.remove("Future");
    CHECK(store.read().unusable.empty());
    store.save(preset("future", 5));
    CHECK(find(store.read(), "future"));
}

TEST_CASE("A write keeps unknown keys and unusable presets of other entries", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    spit(store.file(),
         R"({"arraw": 1, "more": [1], "exportPresets": [
             {"name": "Future", "format": "avif", "note": "x"},
             {"name": "Mine", "quality": 50, "watermark": "yes"}]})");
    store.save(preset("Other", 77));
    store.save(preset("mine", 60));

    const std::string text = slurp(store.file());
    CHECK(text.find("\"more\"") != std::string::npos);
    CHECK(text.find("\"avif\"") != std::string::npos);
    CHECK(text.find("\"note\"") != std::string::npos);
    CHECK(text.find("\"watermark\"") != std::string::npos);
    const ExportPresetCatalog catalog = store.read();
    CHECK(find(catalog, "mine")->settings.quality == 60);
    CHECK(find(catalog, "Other")->settings.quality == 77);
    CHECK(catalog.unusable.size() == 1);
}

TEST_CASE("A newer file is never written: save, rename and remove leave it byte for byte",
          "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    spit(store.file(), newerFile);
    CHECK(store.read().state == ExportPresetFileState::Newer);
    CHECK(find(store.read(), "Keep"));
    CHECK(problemOf([&] { store.save(preset("New")); }) == ExportPresetProblem::NewerFile);
    CHECK(problemOf([&] { store.rename("Keep", "Kept"); }) == ExportPresetProblem::NewerFile);
    CHECK(problemOf([&] { store.remove("Keep"); }) == ExportPresetProblem::NewerFile);
    CHECK(slurp(store.file()) == newerFile);
}

TEST_CASE("An unreadable file is never written, and setAside moves it without replacing anything",
          "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    CHECK(problemOf([&] { (void)store.setAside(); }) == ExportPresetProblem::NotFound);
    spit(store.file(), "not json at all");
    const ExportPresetCatalog catalog = store.read();
    CHECK(catalog.state == ExportPresetFileState::Unreadable);
    CHECK_FALSE(catalog.problem.empty());
    CHECK(problemOf([&] { store.save(preset("New")); }) == ExportPresetProblem::UnreadableFile);
    CHECK(problemOf([&] { store.remove("New"); }) == ExportPresetProblem::UnreadableFile);
    CHECK(slurp(store.file()) == "not json at all");

    const fs::path first = store.setAside();
    CHECK(first.filename().string().starts_with("export-presets.unreadable-"));
    CHECK(first.extension() == ".json");
    CHECK(slurp(first) == "not json at all");
    CHECK_FALSE(fs::exists(store.file()));

    store.save(preset("Fresh"));
    CHECK(find(store.read(), "Fresh"));

    // A second file set aside within the same second takes another name.
    spit(store.file(), "[1]");
    const fs::path second = store.setAside();
    CHECK(second != first);
    CHECK(slurp(first) == "not json at all");
    CHECK(slurp(second) == "[1]");
}

TEST_CASE("Two stores on one directory do not lose each other's saves", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore first(dir.path());
    ExportPresetStore second(dir.path());
    first.save(preset("a1"));
    second.save(preset("b1"));
    first.save(preset("a2"));
    second.save(preset("b2"));
    CHECK(userNames(first.read()) == std::vector<std::string>{"a1", "a2", "b1", "b2"});

    // From two threads.
    std::atomic<int> failures = 0;
    const auto run = [&](char tag) {
        ExportPresetStore store(dir.path());
        for (int i = 0; i < 8; ++i) {
            try {
                store.save(preset(std::string(1, tag) + "t" + std::to_string(i), i));
            } catch (const std::exception&) {
                ++failures;
            }
        }
    };
    std::thread one(run, 'x');
    std::thread two(run, 'y');
    one.join();
    two.join();
    CHECK(failures == 0);
    CHECK(userNames(first.read()).size() == 4 + 16);
}

TEST_CASE("A held lock makes a write Busy; a stale lock is broken", "[presets][store]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    store.save(preset("Before"));
    const QString lockPath = QString::fromStdU16String(store.file().u16string()) + ".lock";
    {
        QLockFile held(lockPath);
        REQUIRE(held.tryLock(100));
        CHECK(problemOf([&] { store.save(preset("Blocked")); }) == ExportPresetProblem::Busy);
        CHECK_FALSE(find(store.read(), "Blocked"));
    }
    store.save(preset("After"));
    CHECK(find(store.read(), "After"));

#ifdef __linux__
    // A lock left by a process that is gone: the same host, a pid that does not exist, and old.
    {
        QByteArray content;
        {
            QLockFile held(lockPath);
            REQUIRE(held.tryLock(100));
            QFile file(lockPath);
            REQUIRE(file.open(QIODevice::ReadOnly));
            content = file.readAll();
        }
        REQUIRE_FALSE(fs::exists(store.file().string() + ".lock"));
        QList<QByteArray> lines = content.split('\n');
        REQUIRE(lines.size() >= 3);
        lines[0] = "2147483646";
        spit(store.file().string() + ".lock", lines.join('\n').toStdString());
        fs::last_write_time(store.file().string() + ".lock",
                            fs::file_time_type::clock::now() - std::chrono::hours(1));
        store.save(preset("Despite stale lock"));
        CHECK(find(store.read(), "Despite stale lock"));
    }
#endif
}

TEST_CASE("A symbolic link to the file stays a link after a write", "[presets][store]") {
    const test::TempDir dir;
    const fs::path real = dir.path() / "dotfiles" / "export-presets.json";
    spit(real, R"({"arraw": 1, "exportPresets": []})");
    const fs::path config = dir.path() / "config";
    fs::create_directories(config);
    std::error_code error;
    fs::create_symlink(real, config / exportPresetsFileName, error);
    if (error) {
        SKIP("symbolic links are not available: " << error.message());
    }
    ExportPresetStore store(config);
    store.save(preset("Linked", 33));
    CHECK(fs::is_symlink(store.file()));
    CHECK(slurp(real).find("Linked") != std::string::npos);
    CHECK(find(store.read(), "Linked"));
}

TEST_CASE("An unwritable directory gives WriteFailed and leaves the old file", "[presets][store]") {
#ifdef __unix__
    if (geteuid() == 0) {
        SKIP("root can write anywhere");
    }
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    store.save(preset("Old", 11));
    const std::string before = slurp(store.file());
    fs::permissions(dir.path(), fs::perms::owner_read | fs::perms::owner_exec);
    const auto problem = problemOf([&] { store.save(preset("New")); });
    fs::permissions(dir.path(), fs::perms::owner_all);
    CHECK(problem == ExportPresetProblem::WriteFailed);
    CHECK(slurp(store.file()) == before);
#else
    SKIP("permissions are checked on Unix only");
#endif
}

TEST_CASE("The built-in presets are defined in code, and fit the dialog's choices",
          "[presets][store][builtin]") {
    const auto builtIns = builtInExportPresets();
    REQUIRE(builtIns.size() == 4);
    CHECK(builtIns[0].name == "Web 2048 px");
    CHECK(builtIns[1].name == "Social 1080 px");
    CHECK(builtIns[2].name == "Full size JPEG");
    CHECK(builtIns[3].name == "Print TIFF 16-bit");
    for (const ExportPreset& preset : builtIns) {
        CHECK(preset.builtIn);
        CHECK_FALSE(preset.shadowed);
        CHECK(normalizedPresetName(preset.name) == preset.name);
        CHECK_NOTHROW(validate(preset.settings));
        CHECK(canonical(preset.settings) == preset.settings);
        CHECK(preset.settings.captureInfo);
        CHECK(preset.settings.descriptive);
        CHECK_FALSE(preset.settings.location);
        if (preset.settings.resize) {
            const auto edges = presetLongEdges();
            CHECK(std::ranges::find(edges, preset.settings.width) != edges.end());
            CHECK(preset.settings.width == preset.settings.height);
            CHECK_FALSE(preset.settings.allowEnlarging);
        }
    }
    CHECK(builtIns[0].settings.width == 2048);
    CHECK(builtIns[0].settings.quality == 85);
    CHECK(builtIns[1].settings.width == 1080);
    CHECK(builtIns[2].settings.quality == 92);
    CHECK_FALSE(builtIns[2].settings.resize);
    CHECK(builtIns[3].settings.format == ImageFileFormat::Tiff);
    CHECK(builtIns[3].settings.encoding == NamedEncoding::AdobeRgb);
    CHECK(builtIns[3].settings.sixteenBit);
}

TEST_CASE("Built-in presets come first, in their order, then the file's by name",
          "[presets][store][builtin]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    // Even with no file there are the built-ins, and nothing is created.
    ExportPresetCatalog catalog = store.read();
    CHECK(catalog.state == ExportPresetFileState::Missing);
    REQUIRE(catalog.presets.size() == 4);
    CHECK_FALSE(fs::exists(store.file()));

    store.save(preset("zzz"));
    store.save(preset("AAA"));
    catalog = store.read();
    REQUIRE(catalog.presets.size() == 6);
    const auto builtIns = builtInExportPresets();
    for (std::size_t i = 0; i < builtIns.size(); ++i) {
        CHECK(catalog.presets[i] == builtIns[i]);
    }
    CHECK(catalog.presets[4].name == "AAA");
    CHECK(catalog.presets[5].name == "zzz");
    CHECK(store.load("web 2048 PX")->builtIn);
}

TEST_CASE("A built-in cannot be saved over, renamed to or removed, whatever the case",
          "[presets][store][builtin]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    store.save(preset("Mine"));
    const std::string before = slurp(store.file());
    CHECK(problemOf([&] { store.save(preset("Web 2048 px")); }) == ExportPresetProblem::BuiltIn);
    CHECK(problemOf([&] { store.save(preset("  full SIZE jpeg ")); }) ==
          ExportPresetProblem::BuiltIn);
    CHECK(problemOf([&] { store.rename("Mine", "print tiff 16-bit"); }) ==
          ExportPresetProblem::BuiltIn);
    CHECK(problemOf([&] { store.rename("Web 2048 px", "Elsewhere"); }) ==
          ExportPresetProblem::BuiltIn);
    CHECK(problemOf([&] { store.remove("Social 1080 px"); }) == ExportPresetProblem::BuiltIn);
    CHECK(slurp(store.file()) == before);
}

TEST_CASE("Built-ins are never written to the file", "[presets][store][builtin]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    store.save(preset("Mine"));
    store.save(preset("Another"));
    store.remove("Another");
    const std::string text = slurp(store.file());
    for (const ExportPreset& builtIn : builtInExportPresets()) {
        CHECK(text.find(builtIn.name) == std::string::npos);
    }
    CHECK(text.find("Mine") != std::string::npos);
}

TEST_CASE("A preset of the file with a built-in's name is kept, shadowed, and the built-in wins",
          "[presets][store][builtin]") {
    const test::TempDir dir;
    ExportPresetStore store(dir.path());
    spit(
        store.file(),
        R"({"arraw": 1, "exportPresets": [{"name": "web 2048 px", "quality": 11}, {"name": "Mine"}]})");
    const ExportPresetCatalog catalog = store.read();
    REQUIRE(catalog.presets.size() == 6);
    CHECK(catalog.presets[4].name == "Mine");
    CHECK_FALSE(catalog.presets[4].shadowed);
    CHECK(catalog.presets[5].name == "web 2048 px");
    CHECK(catalog.presets[5].shadowed);
    CHECK_FALSE(catalog.presets[5].builtIn);

    const auto loaded = store.load("WEB 2048 px");
    REQUIRE(loaded);
    CHECK(loaded->builtIn);
    CHECK(loaded->settings.quality == 85);

    // Another write keeps it, and it can be renamed away or removed.
    store.save(preset("Other"));
    CHECK(slurp(store.file()).find("web 2048 px") != std::string::npos);
    store.rename("Web 2048 px", "Old web");
    const ExportPresetCatalog after = store.read();
    REQUIRE(find(after, "Old web"));
    CHECK(find(after, "Old web")->settings.quality == 11);
    CHECK_FALSE(find(after, "Old web")->shadowed);
}

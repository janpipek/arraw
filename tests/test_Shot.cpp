#include "support/TempDir.h"

#include <ImageImport.h>
#include <MarksFilter.h>
#include <PhotoMarks.h>
#include <Shot.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
namespace fs = std::filesystem;

namespace {

/// @brief Shorthand for a shot.
Shot shotOf(fs::path primary, std::vector<fs::path> companions = {}) {
    return {std::move(primary), std::move(companions)};
}

/// @brief The primaries of shots, for checking an order.
std::vector<std::string> primaries(const std::vector<Shot>& shots) {
    std::vector<std::string> names;
    for (const Shot& shot : shots) {
        names.push_back(shot.primary.string());
    }
    return names;
}

void touch(const fs::path& path) {
    std::ofstream(path) << "x";
}

PhotoMarks marks(int rating, std::optional<ColorLabel> label = std::nullopt) {
    return {.rating = rating, .label = label};
}

} // namespace

TEST_CASE("A RAW and its JPEG are one shot with the RAW as primary", "[shot][group]") {
    const auto shots = groupShots({"/p/IMG_1.CR2", "/p/IMG_1.JPG"});
    REQUIRE(shots == std::vector{shotOf("/p/IMG_1.CR2", {"/p/IMG_1.JPG"})});
}

TEST_CASE("The order of the files does not decide who is primary", "[shot][group]") {
    const auto shots = groupShots({"/p/IMG_1.jpg", "/p/IMG_1.png", "/p/IMG_1.arw"});
    REQUIRE(shots == std::vector{shotOf("/p/IMG_1.arw", {"/p/IMG_1.jpg", "/p/IMG_1.png"})});
}

TEST_CASE("Stems pair without regard to case", "[shot][group]") {
    const auto shots = groupShots({"/p/IMG_001.CR2", "/p/img_001.jpg"});
    REQUIRE(shots == std::vector{shotOf("/p/IMG_001.CR2", {"/p/img_001.jpg"})});
}

TEST_CASE("Files of different folders never pair", "[shot][group]") {
    const auto shots = groupShots({"/a/IMG_1.CR2", "/b/IMG_1.JPG"});
    REQUIRE(shots.size() == 2);
    REQUIRE(shots[0].companions.empty());
    REQUIRE(shots[1].companions.empty());
}

TEST_CASE("Two RAWs sharing a stem leave every file of the stem standing alone", "[shot][group]") {
    const auto shots = groupShots({"/p/IMG_1.CR2", "/p/IMG_1.NEF", "/p/IMG_1.JPG"});
    REQUIRE(shots.size() == 3);
    for (const Shot& shot : shots) {
        REQUIRE(shot.companions.empty());
    }
}

TEST_CASE("A JPEG without a RAW stands alone, and so do two standard images of one stem",
          "[shot][group]") {
    REQUIRE(groupShots({"/p/a.jpg"}) == std::vector{shotOf("/p/a.jpg")});
    const auto shots = groupShots({"/p/a.png", "/p/a.jpg"});
    REQUIRE(shots.size() == 2);
    REQUIRE(shots[0].companions.empty());
    REQUIRE(shots[1].companions.empty());
}

TEST_CASE("Unsupported files and sidecars are dropped", "[shot][group]") {
    const auto shots =
        groupShots({"/p/a.xmp", "/p/a.txt", "/p/a.arw", "/p/noextension", "/p/b.XMP"});
    REQUIRE(shots == std::vector{shotOf("/p/a.arw")});
    REQUIRE(groupShots({}).empty());
}

TEST_CASE("Shots come in natural order of the primary's name", "[shot][group]") {
    const auto shots =
        groupShots({"/p/IMG_10.ARW", "/p/img_2.arw", "/p/IMG_1.ARW", "/p/IMG_02.arw"});
    // IMG_02 is numerically 2, as img_2 is: the tie is broken by the exact name.
    REQUIRE(primaries(shots) == std::vector<std::string>{"/p/IMG_1.ARW", "/p/IMG_02.arw",
                                                         "/p/img_2.arw", "/p/IMG_10.ARW"});
}

TEST_CASE("Natural order ignores case and is the same whatever order the files arrive in",
          "[shot][group]") {
    std::vector<fs::path> paths{"/p/b.jpg", "/p/B.jpg",   "/p/a.jpg",
                                "/p/A.jpg", "/p/c10.jpg", "/p/C9.jpg"};
    const auto expected = primaries(groupShots(paths));
    std::ranges::reverse(paths);
    REQUIRE(primaries(groupShots(paths)) == expected);
    // a and A (a stem in two cases is one capture: no RAW, so each stands alone) come first.
    REQUIRE(expected.front().find_first_of("aA") != std::string::npos);
    REQUIRE(expected[4] == "/p/C9.jpg");
    REQUIRE(expected[5] == "/p/c10.jpg");
}

TEST_CASE("Companions are in natural order too", "[shot][group]") {
    const auto shots = groupShots({"/p/a.png", "/p/a.tif", "/p/a.dng", "/p/a.jpeg", "/p/a.jpg"});
    REQUIRE(shots.size() == 1);
    REQUIRE(shots[0].companions ==
            std::vector<fs::path>{"/p/a.jpeg", "/p/a.jpg", "/p/a.png", "/p/a.tif"});
}

TEST_CASE("Format labels name each format once, primary first", "[shot][label]") {
    REQUIRE(formatLabel(shotOf("/p/a.ARW")) == "ARW");
    REQUIRE(formatLabel(shotOf("/p/a.jpg")) == "JPEG");
    REQUIRE(formatLabel(shotOf("/p/a.JPEG")) == "JPEG");
    REQUIRE(formatLabel(shotOf("/p/a.tif")) == "TIFF");
    REQUIRE(formatLabel(shotOf("/p/a.tiff")) == "TIFF");
    REQUIRE(formatLabel(shotOf("/p/a.png")) == "PNG");
    REQUIRE(formatLabel(shotOf("/p/a.arw", {"/p/a.jpg"})) == "ARW+JPEG");
    REQUIRE(formatLabel(shotOf("/p/a.arw", {"/p/a.jpg", "/p/a.jpeg", "/p/a.tif"})) ==
            "ARW+JPEG+TIFF");
}

TEST_CASE("Supported images are named by extension, in any case", "[shot][supported]") {
    for (const char* name : {"a.arw", "a.CR3", "a.dng", "a.jpg", "a.JPEG", "a.png", "a.tif",
                             "a.TIFF", "/some/dir/a.Nef"}) {
        CAPTURE(name);
        REQUIRE(isSupportedImage(name));
    }
    for (const char* name : {"a.xmp", "a.txt", "a", "a.", ".jpg", "a.jpg.xmp", "a.heic"}) {
        CAPTURE(name);
        REQUIRE_FALSE(isSupportedImage(name));
    }
}

TEST_CASE("The supported extensions are lower-case, dotless and unique", "[shot][supported]") {
    const auto extensions = supportedImageExtensions();
    // Every RAW LibRaw opens (21), then the five standard formats.
    REQUIRE(extensions.size() == 26);
    for (const auto extension : extensions) {
        CAPTURE(extension);
        REQUIRE_FALSE(extension.empty());
        REQUIRE(extension.find('.') == std::string_view::npos);
        REQUIRE(std::ranges::none_of(extension, [](char c) { return c >= 'A' && c <= 'Z'; }));
        REQUIRE(isSupportedImage(fs::path("x." + std::string(extension))));
    }
    REQUIRE(std::ranges::count(extensions, "jpg") == 1);
}

TEST_CASE("A RAW the decoder opens by content is still a RAW for listing and pairing",
          "[shot][supported]") {
    // The decoder claims fewer extensions by name than LibRaw opens; folders,
    // pairing and sidecar naming go by the full list.
    REQUIRE(isSupportedImage(fs::path("DSC_1.NRW")));
    REQUIRE(isSupportedImage(fs::path("B0001.3fr")));
    const std::vector<Shot> shots = groupShots({"a/DSC_1.JPG", "a/DSC_1.NRW"});
    REQUIRE(shots.size() == 1);
    REQUIRE(shots[0].primary == fs::path("a/DSC_1.NRW"));
    REQUIRE(shots[0].companions == std::vector<fs::path>{"a/DSC_1.JPG"});
}

TEST_CASE("Listing a folder gives its shots, without hidden files or subfolders", "[shot][list]") {
    const test::TempDir directory;
    touch(directory.file("IMG_2.ARW"));
    touch(directory.file("IMG_2.JPG"));
    touch(directory.file("IMG_10.jpg"));
    touch(directory.file("IMG_2.xmp"));
    touch(directory.file("notes.txt"));
    touch(directory.file(".hidden.jpg"));
    fs::create_directory(directory.file("sub"));
    touch(directory.file("sub") / "deep.jpg");
    fs::create_directory(directory.file("folder.jpg")); // a directory named like a photograph

    const auto shots = listShots(directory.path());
    REQUIRE(shots == std::vector{shotOf(directory.file("IMG_2.ARW"), {directory.file("IMG_2.JPG")}),
                                 shotOf(directory.file("IMG_10.jpg"))});
}

TEST_CASE("Listing an empty folder gives no shots", "[shot][list]") {
    const test::TempDir directory;
    REQUIRE(listShots(directory.path()).empty());
}

TEST_CASE("Listing a folder that cannot be read throws, naming it", "[shot][list]") {
    const test::TempDir directory;
    const auto missing = directory.file("missing");
    REQUIRE_THROWS_AS(listShots(missing), std::runtime_error);
    touch(directory.file("file.jpg"));
    REQUIRE_THROWS_AS(listShots(directory.file("file.jpg")), std::runtime_error);
    try {
        (void)listShots(missing);
    } catch (const std::runtime_error& error) {
        REQUIRE(std::string(error.what()).find("missing") != std::string::npos);
    }
}

TEST_CASE("The default filter is inactive and matches everything", "[marks][filter]") {
    const MarksFilter filter;
    REQUIRE_FALSE(filter.isActive());
    REQUIRE(filter.matches(marks(-1)));
    REQUIRE(filter.matches(marks(0)));
    REQUIRE(filter.matches(marks(5, ColorLabel::Blue)));
}

TEST_CASE("A star threshold matches N and above, and not rejects or unrated", "[marks][filter]") {
    const MarksFilter filter{.minRating = 3};
    REQUIRE(filter.isActive());
    for (const int rating : {3, 4, 5}) {
        REQUIRE(filter.matches(marks(rating)));
    }
    for (const int rating : {2, 1, 0, -1}) {
        REQUIRE_FALSE(filter.matches(marks(rating)));
    }
}

TEST_CASE("Rejects only matches only rejects", "[marks][filter]") {
    const MarksFilter filter{.rejectsOnly = true};
    REQUIRE(filter.isActive());
    REQUIRE(filter.matches(marks(-1)));
    for (const int rating : {0, 3, 5}) {
        REQUIRE_FALSE(filter.matches(marks(rating)));
    }
}

TEST_CASE("The labels match by OR, and a photograph without a label matches none",
          "[marks][filter]") {
    const MarksFilter filter{.labels = {ColorLabel::Red, ColorLabel::Green}};
    REQUIRE(filter.isActive());
    REQUIRE(filter.matches(marks(0, ColorLabel::Red)));
    REQUIRE(filter.matches(marks(0, ColorLabel::Green)));
    REQUIRE_FALSE(filter.matches(marks(0, ColorLabel::Blue)));
    REQUIRE_FALSE(filter.matches(marks(0)));
}

TEST_CASE("An empty label set imposes no label constraint", "[marks][filter]") {
    const MarksFilter filter{.minRating = 1};
    REQUIRE(filter.matches(marks(2)));
    REQUIRE(filter.matches(marks(2, ColorLabel::Purple)));
}

TEST_CASE("Rating and labels combine by AND", "[marks][filter]") {
    const MarksFilter filter{.minRating = 3, .labels = {ColorLabel::Red, ColorLabel::Green}};
    REQUIRE(filter.matches(marks(4, ColorLabel::Green)));
    REQUIRE(filter.matches(marks(3, ColorLabel::Red)));
    REQUIRE_FALSE(filter.matches(marks(4, ColorLabel::Blue)));
    REQUIRE_FALSE(filter.matches(marks(2, ColorLabel::Red)));
    REQUIRE_FALSE(filter.matches(marks(2, ColorLabel::Blue)));
}

TEST_CASE("Rejects only combines with labels by AND", "[marks][filter]") {
    const MarksFilter filter{.rejectsOnly = true, .labels = {ColorLabel::Red}};
    REQUIRE(filter.matches(marks(-1, ColorLabel::Red)));
    REQUIRE_FALSE(filter.matches(marks(-1, ColorLabel::Blue)));
    REQUIRE_FALSE(filter.matches(marks(3, ColorLabel::Red)));
}

TEST_CASE("A filter that wants rejects and stars is refused", "[marks][filter]") {
    const MarksFilter both{.minRating = 2, .rejectsOnly = true};
    REQUIRE_THROWS_AS(both.validate(), std::invalid_argument);
    REQUIRE_THROWS_AS(both.matches(marks(-1)), std::invalid_argument);
    REQUIRE_THROWS_AS(MarksFilter{.minRating = 6}.validate(), std::invalid_argument);
    REQUIRE_THROWS_AS(MarksFilter{.minRating = -1}.validate(), std::invalid_argument);
    REQUIRE_NOTHROW(MarksFilter{.minRating = 5}.validate());
}

TEST_CASE("Filters compare field by field", "[marks][filter]") {
    MarksFilter a;
    MarksFilter b;
    REQUIRE(a == b);
    b.minRating = 2;
    REQUIRE_FALSE(a == b);
    a.minRating = 2;
    a.labels = {ColorLabel::Blue};
    b.labels = {ColorLabel::Blue};
    REQUIRE(a == b);
}

TEST_CASE("Listing recursively walks the subfolders folder by folder", "[shot][list][recursive]") {
    const test::TempDir directory;
    fs::create_directories(directory.file("b/deep"));
    fs::create_directories(directory.file("a"));
    fs::create_directories(directory.file("sub10"));
    fs::create_directories(directory.file("sub2"));
    touch(directory.file("IMG_5.jpg"));
    touch(directory.file("a/IMG_1.ARW"));
    touch(directory.file("a/IMG_1.JPG"));
    touch(directory.file("b/IMG_1.ARW"));
    touch(directory.file("b/deep/IMG_9.jpg"));
    touch(directory.file("sub2/x.jpg"));
    touch(directory.file("sub10/x.jpg"));

    const auto shots = listShots(directory.path(), true);
    REQUIRE(shots ==
            std::vector{
                shotOf(directory.file("IMG_5.jpg")),
                shotOf(directory.file("a/IMG_1.ARW"), {directory.file("a/IMG_1.JPG")}),
                shotOf(directory.file("b/IMG_1.ARW")), shotOf(directory.file("b/deep/IMG_9.jpg")),
                shotOf(directory.file("sub2/x.jpg")), shotOf(directory.file("sub10/x.jpg"))});
    // The same names in two folders are two shots, not one pair.
    REQUIRE(std::ranges::count_if(shots, [](const Shot& shot) {
                return shot.primary.filename() == "IMG_1.ARW";
            }) == 2);
}

TEST_CASE("Listing recursively skips hidden and junk folders and hidden files",
          "[shot][list][recursive]") {
    const test::TempDir directory;
    fs::create_directories(directory.file(".hiddendir/inner"));
    fs::create_directories(directory.file("@eaDir"));
    fs::create_directories(directory.file("keep"));
    touch(directory.file(".hiddendir/a.jpg"));
    touch(directory.file(".hiddendir/inner/b.jpg"));
    touch(directory.file("@eaDir/c.jpg"));
    touch(directory.file("keep/.d.jpg"));
    touch(directory.file("keep/e.jpg"));

    REQUIRE(listShots(directory.path(), true) == std::vector{shotOf(directory.file("keep/e.jpg"))});
}

TEST_CASE("Listing recursively does not follow symbolic links to folders",
          "[shot][list][recursive]") {
    const test::TempDir directory;
    fs::create_directories(directory.file("real"));
    touch(directory.file("real/a.jpg"));
    std::error_code error;
    fs::create_directory_symlink(directory.file("real"), directory.file("link"), error);
    if (error) {
        SKIP("symbolic links are not available");
    }
    REQUIRE(listShots(directory.path(), true) == std::vector{shotOf(directory.file("real/a.jpg"))});
}

TEST_CASE("Listing recursively still throws for a root that cannot be read",
          "[shot][list][recursive]") {
    const test::TempDir directory;
    REQUIRE_THROWS_AS(listShots(directory.file("missing"), true), std::runtime_error);
}

TEST_CASE("Listing without recursion leaves the subfolders alone", "[shot][list][recursive]") {
    const test::TempDir directory;
    fs::create_directories(directory.file("sub"));
    touch(directory.file("sub/a.jpg"));
    touch(directory.file("b.jpg"));
    REQUIRE(listShots(directory.path()) == std::vector{shotOf(directory.file("b.jpg"))});
    REQUIRE(listShots(directory.path(), false) == listShots(directory.path()));
}

TEST_CASE("Listing recursively keeps a folder's subtree next to it", "[shot][list][recursive]") {
    const test::TempDir directory;
    for (const char* name : {"Rome/raw", "Rome 2", "Trip-2", "Trip/day1"}) {
        fs::create_directories(directory.file(name));
        touch(directory.file(name) / "x.jpg");
    }
    touch(directory.file("Rome/x.jpg"));
    touch(directory.file("Trip/x.jpg"));

    std::vector<fs::path> folders;
    for (const auto& shot : listShots(directory.path(), true)) {
        folders.push_back(shot.primary.parent_path());
    }
    REQUIRE(folders == std::vector{directory.file("Rome"), directory.file("Rome/raw"),
                                   directory.file("Rome 2"), directory.file("Trip"),
                                   directory.file("Trip/day1"), directory.file("Trip-2")});
}

TEST_CASE("Listing recursively skips a folder that cannot be read and goes on",
          "[shot][list][recursive]") {
    const test::TempDir directory;
    fs::create_directories(directory.file("a"));
    fs::create_directories(directory.file("locked/inner"));
    fs::create_directories(directory.file("z"));
    touch(directory.file("a/1.jpg"));
    touch(directory.file("locked/2.jpg"));
    touch(directory.file("locked/inner/3.jpg"));
    touch(directory.file("z/4.jpg"));
    fs::permissions(directory.file("locked"), fs::perms::none);
    std::error_code error;
    const bool readable =
        fs::directory_iterator(directory.file("locked"), error) != fs::directory_iterator() ||
        !error;
    const auto shots = listShots(directory.path(), true);
    fs::permissions(directory.file("locked"), fs::perms::owner_all);
    if (readable) {
        SKIP("permissions are not enforced here (running as the superuser?)");
    }
    REQUIRE(shots ==
            std::vector{shotOf(directory.file("a/1.jpg")), shotOf(directory.file("z/4.jpg"))});
}

TEST_CASE("The listed-folder rule refuses hidden, junk and linked folders",
          "[shot][list][recursive]") {
    const test::TempDir directory;
    for (const char* name :
         {"real", ".hidden", "@eaDir", "#recycle", "$RECYCLE.BIN", "System Volume Information"}) {
        fs::create_directories(directory.file(name));
    }
    touch(directory.file("file.jpg"));
    std::error_code error;
    fs::create_directory_symlink(directory.file("real"), directory.file("link"), error);
    const bool linked = !error;
    for (const auto& entry : fs::directory_iterator(directory.path())) {
        const std::string name = entry.path().filename().string();
        INFO(name);
        CHECK(isListedFolder(entry) == (name == "real"));
    }
    CHECK(linked == fs::is_symlink(directory.file("link")));
}

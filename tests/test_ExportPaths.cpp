#include "ExportPaths.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

using namespace arraw;

TEST_CASE("A known image extension is replaced, any other is kept and extended",
          "[app][export][paths]") {
    using std::filesystem::path;
    REQUIRE(app::withSuffix("a/shot.png", ImageFileFormat::Jpeg) == path("a/shot.jpg"));
    REQUIRE(app::withSuffix("shot.TIF", ImageFileFormat::Png) == path("shot.png"));
    REQUIRE(app::withSuffix("shot.tiff", ImageFileFormat::Jpeg) == path("shot.jpg"));
    REQUIRE(app::withSuffix("shot.JPEG", ImageFileFormat::Tiff) == path("shot.tif"));
    REQUIRE(app::withSuffix("shot.v2", ImageFileFormat::Png) == path("shot.v2.png"));
    REQUIRE(app::withSuffix("shot", ImageFileFormat::Tiff) == path("shot.tif"));
}

TEST_CASE("An extension that already belongs to the format is left alone", "[app][export][paths]") {
    using std::filesystem::path;
    REQUIRE(app::withSuffix("shot.jpeg", ImageFileFormat::Jpeg) == path("shot.jpeg"));
    REQUIRE(app::withSuffix("shot.JPG", ImageFileFormat::Jpeg) == path("shot.JPG"));
    REQUIRE(app::withSuffix("shot.tiff", ImageFileFormat::Tiff) == path("shot.tiff"));
    REQUIRE(app::withSuffix("shot.PNG", ImageFileFormat::Png) == path("shot.PNG"));
}

TEST_CASE("The suggested name is the source's, in its folder", "[app][export][paths]") {
    using std::filesystem::path;
    REQUIRE(app::suggestedPath("/photos/day/IMG_001.CR2", ImageFileFormat::Jpeg) ==
            path("/photos/day/IMG_001.jpg"));
    REQUIRE(app::suggestedPath("IMG_001.dng", ImageFileFormat::Tiff) == path("IMG_001.tif"));
}

namespace {

using Paths = std::vector<std::filesystem::path>;

}

TEST_CASE("Targets are the stems in the folder with the format's suffix", "[app][export][paths]") {
    const Paths sources{"/a/one.nef", "/b/two.cr2"};
    const Paths targets = app::targetPaths(sources, "/out", ImageFileFormat::Tiff);
    CHECK(targets == Paths{"/out/one.tif", "/out/two.tif"});
    CHECK(app::targetPaths({}, "/out", ImageFileFormat::Jpeg).empty());
}

TEST_CASE("Targets of equal stems are numbered from 2, in the order given",
          "[app][export][paths]") {
    const Paths sources{"/a/x.nef", "/b/x.nef", "/c/x.cr2"};
    CHECK(app::targetPaths(sources, "/out", ImageFileFormat::Jpeg) ==
          Paths{"/out/x.jpg", "/out/x_2.jpg", "/out/x_3.jpg"});
    // Deterministic.
    CHECK(app::targetPaths(sources, "/out", ImageFileFormat::Jpeg) ==
          app::targetPaths(sources, "/out", ImageFileFormat::Jpeg));
}

TEST_CASE("Targets collide whatever the letter case of the stem", "[app][export][paths]") {
    const Paths sources{"/a/IMG_1.NEF", "/b/img_1.nef"};
    CHECK(app::targetPaths(sources, "/out", ImageFileFormat::Png) ==
          Paths{"/out/IMG_1.png", "/out/img_1_2.png"});
}

TEST_CASE("A numbered target does not take a name another stem needs", "[app][export][paths]") {
    const Paths sources{"/a/x.nef", "/b/x.nef", "/c/x_2.nef"};
    const Paths targets = app::targetPaths(sources, "/out", ImageFileFormat::Jpeg);
    CHECK(targets == Paths{"/out/x.jpg", "/out/x_2.jpg", "/out/x_2_2.jpg"});
}

TEST_CASE("A target is never one of the sources", "[app][export][paths]") {
    // A JPEG shot exported as JPEG into its own folder.
    const Paths sources{"/out/a.jpg", "/out/B.JPG", "/x/a.nef"};
    const Paths targets = app::targetPaths(sources, "/out", ImageFileFormat::Jpeg);
    CHECK(targets == Paths{"/out/a_2.jpg", "/out/B_2.jpg", "/out/a_3.jpg"});
}

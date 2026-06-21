#include "ImageGrouping.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("companion badge is empty when there are no companions", "[grouping]") {
    REQUIRE(companionBadgeText({}) == QString());
}

TEST_CASE("companion badge shows the companion's uppercased extension", "[grouping]") {
    REQUIRE(companionBadgeText({"/photos/IMG_001.jpg"}) == "JPG");
}

TEST_CASE("companion badge appends +N for extra companions", "[grouping]") {
    REQUIRE(companionBadgeText({"/photos/IMG_001.jpg", "/photos/IMG_001.png"}) == "JPG+1");
}

TEST_CASE("a lone RAW file is one group with no companions", "[grouping]") {
    const QList<ImageGroup> groups = groupImageFiles({"/photos/IMG_001.CR2"});

    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].primary == "/photos/IMG_001.CR2");
    REQUIRE(groups[0].companions.isEmpty());
}

TEST_CASE("stem matching is case-insensitive", "[grouping]") {
    const QList<ImageGroup> groups = groupImageFiles({"/photos/IMG_001.CR2", "/photos/img_001.jpg"});

    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].primary == "/photos/IMG_001.CR2");
    REQUIRE(groups[0].companions == QStringList{"/photos/img_001.jpg"});
}

TEST_CASE("same stem in different directories does not group", "[grouping]") {
    const QList<ImageGroup> groups = groupImageFiles({"/a/IMG_001.CR2", "/b/IMG_001.JPG"});

    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].companions.isEmpty());
    REQUIRE(groups[1].companions.isEmpty());
}

TEST_CASE("one RAW owns every same-stem standard image as a companion", "[grouping]") {
    const QList<ImageGroup> groups = groupImageFiles(
        {"/photos/IMG_001.CR2", "/photos/IMG_001.JPG", "/photos/IMG_001.PNG"});

    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].primary == "/photos/IMG_001.CR2");
    REQUIRE(groups[0].companions == QStringList{"/photos/IMG_001.JPG", "/photos/IMG_001.PNG"});
}

TEST_CASE("two RAWs sharing a stem are ambiguous: all same-stem files stand alone", "[grouping]") {
    const QList<ImageGroup> groups = groupImageFiles(
        {"/photos/IMG_001.CR2", "/photos/IMG_001.NEF", "/photos/IMG_001.JPG"});

    REQUIRE(groups.size() == 3);
    for (const ImageGroup& g : groups)
        REQUIRE(g.companions.isEmpty());
}

TEST_CASE("standard images with no RAW never merge, even on a shared stem", "[grouping]") {
    const QList<ImageGroup> groups = groupImageFiles({"/photos/IMG_001.JPG", "/photos/IMG_001.PNG"});

    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].primary == "/photos/IMG_001.JPG");
    REQUIRE(groups[0].companions.isEmpty());
    REQUIRE(groups[1].primary == "/photos/IMG_001.PNG");
    REQUIRE(groups[1].companions.isEmpty());
}

TEST_CASE("a RAW and its same-stem JPEG form one group, RAW primary", "[grouping]") {
    const QList<ImageGroup> groups = groupImageFiles({"/photos/IMG_001.CR2", "/photos/IMG_001.JPG"});

    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].primary == "/photos/IMG_001.CR2");
    REQUIRE(groups[0].companions == QStringList{"/photos/IMG_001.JPG"});
}

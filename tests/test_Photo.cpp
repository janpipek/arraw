#include "Photo.h"

#include "ProcessingPlan.h"

#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

using namespace arraw;

/// A photograph as a document (ADR 001): what it is and how it is developed,
/// with no partially populated state in between. Opening one reads what the
/// file declares and nothing else -- the pixels are the renderer's business.

namespace {

constexpr std::string_view neutralFixture = "linear-32x24-neutral.dng";
constexpr std::string_view noWbFixture = "linear-32x24-nowb.dng";
constexpr std::string_view testCard = "testcard-61x41-srgb8.png";

} // namespace

TEST_CASE("A photograph opens as a coherent document", "[photo]") {
    const auto path = test::fixture(neutralFixture);
    const Photo photo = openPhoto(path);

    REQUIRE(photo.path() == path);
    REQUIRE(photo.metadata() == readImageMetadata(path));
    REQUIRE(photo.settings() == DevelopSettings{});
}

TEST_CASE("A photograph that is not a RAW is a document too", "[photo]") {
    const Photo photo = openPhoto(test::fixture(testCard));

    REQUIRE(photo.metadata().size == ImageSize{61, 41});
    REQUIRE(isWorkingEncoding(photo.metadata().encoding));
}

TEST_CASE("Opening a photograph says what the file declares", "[photo][diagnostics]") {
    /// The substitution belongs to the document rather than to a render: it is
    /// true of the file, and a photographer should hear it when the photograph
    /// opens (ADR 012).
    CollectedDiagnostics log;
    const Photo photo = openPhoto(test::fixture(noWbFixture), log);

    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SubstitutedWhiteBalance);
    REQUIRE(log.entries().front().subject == photo.path());
}

TEST_CASE("Developing a photograph differently makes another document", "[photo]") {
    /// How a preset, a before-and-after and the command line's --exposure are
    /// all expressed: another snapshot of the same photograph, with the one
    /// that was read left alone.
    const Photo photo = openPhoto(test::fixture(neutralFixture));
    const Photo lifted = photo.with({.exposure = 1.5F});

    REQUIRE(lifted.path() == photo.path());
    REQUIRE(lifted.metadata() == photo.metadata());
    REQUIRE(lifted.settings().exposure == 1.5F);
    REQUIRE(photo.settings().exposure == 0.0F);
    REQUIRE_FALSE(lifted == photo);
}

TEST_CASE("Two openings of one photograph are the same document", "[photo]") {
    const auto path = test::fixture(neutralFixture);

    REQUIRE(openPhoto(path) == openPhoto(path));
}

TEST_CASE("A file that is not a photograph does not open", "[photo]") {
    const test::TempDir directory;
    const auto destination = directory.file("notes.png");
    {
        std::ofstream stream(destination, std::ios::binary);
        stream << "not an image at all";
        REQUIRE(stream.good());
    }

    REQUIRE_THROWS_AS(openPhoto(destination), std::runtime_error);
}

TEST_CASE("A plan resolves from a photograph", "[photo]") {
    /// ADR 012: what a render is planned against is the document, not a
    /// buffer somebody else loaded and a settings struct that travelled
    /// separately.
    const Photo photo = openPhoto(test::fixture(neutralFixture)).with({.exposure = -1.0F});
    const ProcessingPlan plan = planFor(photo);

    REQUIRE(plan == planFor(photo.metadata().encoding, photo.settings()));
    REQUIRE(plan.exposureGain == 0.5F);
    REQUIRE_FALSE(plan == planFor(openPhoto(test::fixture(neutralFixture))));
}

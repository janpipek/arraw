#include "ImageExport.h"
#include "ImageImport.h"

#include "support/Fixtures.h"
#include "support/ImageCompare.h"
#include "support/TempDir.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <string_view>

using namespace arraw;

/// Integration tests over the whole file-to-file path: decode a PNG that arraw
/// did not write, carry it through the linear Rec.2020 working space, encode it
/// again, and compare the two files. Deliberately a black box — neither side
/// inspects the intermediate ImageBuffer — with an oracle (QImage) that is not
/// the code under test. See ADR 004.

namespace {

/// @brief The fixture every case in this file round-trips.
constexpr std::string_view testCard = "testcard-61x41-srgb8.png";

/// @brief One named flat patch along the bottom band of the test card.
struct Patch {
    std::string_view name;
    test::Rgba8 colour;
};

/// @brief The patches written by `tests/fixtures/make_fixtures.py`, in order.
///
/// Deep shadow leads, because a linear working space is at its coarsest there,
/// and near-white follows, because that is where a transfer function that
/// overshoots would clip.
constexpr std::array patches = {
    Patch{"black", {0, 0, 0, 255}},
    Patch{"code1", {1, 1, 1, 255}},
    Patch{"code2", {2, 2, 2, 255}},
    Patch{"code3", {3, 3, 3, 255}},
    Patch{"quarterGrey", {64, 64, 64, 255}},
    Patch{"midGrey", {128, 128, 128, 255}},
    Patch{"threeQuarterGrey", {192, 192, 192, 255}},
    Patch{"nearWhite", {252, 252, 252, 255}},
    Patch{"white", {255, 255, 255, 255}},
    Patch{"red", {255, 0, 0, 255}},
    Patch{"green", {0, 255, 0, 255}},
    Patch{"blue", {0, 0, 255, 255}},
};

/// The patch band spans rows 32–40 of the card and each patch is
/// `61 / 12 == 5` columns wide, so these coordinates land in a patch's middle
/// rather than on an edge. Kept in step with the generator by hand; the
/// "patches sit where the test expects" case below fails loudly if they drift.
constexpr int patchWidth = 61 / static_cast<int>(patches.size());
constexpr int patchRow = 36;

/// @brief Column at the centre of a patch.
constexpr int patchCentre(std::size_t index) {
    return static_cast<int>(index) * patchWidth + patchWidth / 2;
}

} // namespace

TEST_CASE("The test card fixture is the image the tests assume", "[integration][roundtrip]") {
    const auto card = test::decodeAsSrgb8(test::fixture(testCard));

    REQUIRE(card.width() == 61);
    REQUIRE(card.height() == 41);

    for (std::size_t index = 0; index < patches.size(); ++index) {
        CAPTURE(patches[index].name);
        REQUIRE(test::pixelAt(card, patchCentre(index), patchRow) == patches[index].colour);
    }
}

TEST_CASE("A round trip through a lossless format costs at most one code",
          "[integration][roundtrip]") {
    const auto format = GENERATE(ImageFileFormat::Png, ImageFileFormat::Tiff);
    const auto bitDepth = GENERATE(8, 16);
    CAPTURE(format, bitDepth);

    const auto source = test::fixture(testCard);
    const test::TempDir directory;
    const auto destination = directory.file("roundtrip.bin");

    exportImage(loadImage(source), destination, {.format = format, .bitDepth = bitDepth});

    const auto original = test::decodeAsSrgb8(source);
    const auto exported = test::decodeAsSrgb8(destination);
    REQUIRE(exported.size() == original.size());

    const auto difference = test::compare(original, exported);
    INFO(difference);
    /// The round trip is 8-bit sRGB -> linear Rec.2020 U16 -> 8-bit sRGB.
    /// Rec.2020 contains sRGB, so nothing clips, and linear U16 resolves one
    /// 8-bit shadow code into roughly twenty steps, so neither the gamut nor
    /// the intermediate precision costs anything. The residue is Qt's colour
    /// transform not being exactly self-inverse.
    ///
    /// Measured (Qt 6.10, all four format/depth combinations alike): max 1,
    /// mean 0.047, 81% of pixels exact. The bounds below are those numbers
    /// rounded outwards, not a guess; if they start failing, Qt's transform
    /// changed and the new figures belong here.
    REQUIRE(difference.maxAbsDiff <= 1);
    REQUIRE(difference.meanAbsDiff < 0.1);
    REQUIRE(difference.exactFraction() > 0.8);
}

TEST_CASE("Named patches survive a round trip", "[integration][roundtrip]") {
    const test::TempDir directory;
    const auto destination = directory.file("patches.png");

    exportImage(loadImage(test::fixture(testCard)), destination, {});

    const auto exported = test::decodeAsSrgb8(destination);
    for (std::size_t index = 0; index < patches.size(); ++index) {
        const auto actual = test::pixelAt(exported, patchCentre(index), patchRow);
        CAPTURE(patches[index].name, patches[index].colour, actual);
        /// The same one code of slack the whole-image case allows. Saturated
        /// primaries are where it shows: sRGB green comes back as (1, 255, 0).
        REQUIRE(test::within(actual, patches[index].colour, 1));
    }
}

TEST_CASE("A JPEG round trip degrades within a known bound", "[integration][roundtrip]") {
    const test::TempDir directory;
    const auto destination = directory.file("roundtrip.jpg");

    exportImage(loadImage(test::fixture(testCard)), destination, {.quality = 100});

    const auto original = test::decodeAsSrgb8(test::fixture(testCard));
    const auto exported = test::decodeAsSrgb8(destination);
    REQUIRE(exported.size() == original.size());

    const auto difference = test::compare(original, exported);
    INFO(difference);
    /// A different claim from the lossless cases: not that the pipeline
    /// preserves the image, but that the codec's damage stays where we measured
    /// it. The card mixes smooth ramps with hard patch edges, and a DCT codec
    /// treats the two very differently: the maximum is set by ringing at an
    /// edge, while the ramps compress cleanly and hold the mean near zero.
    /// Both bounds matter, for that reason — the maximum alone would not notice
    /// a global shift, and the mean alone would not notice a ruined edge.
    ///
    /// Measured (Qt 6.10 over libjpeg-turbo): max 3, mean 0.169. The bounds
    /// leave room for a libjpeg version that rounds differently.
    ///
    /// Quality is pinned to 100 because that is where libjpeg stops subsampling
    /// chroma. The same round trip at the ExportOptions default of 90 measures
    /// max *194* — a saturated green patch edge decoding as (66, 184, 194) —
    /// so one tolerance shared with the lossless formats would have to be 194
    /// wide and would assert nothing at all.
    REQUIRE(difference.maxAbsDiff <= 6);
    REQUIRE(difference.meanAbsDiff < 0.5);
}

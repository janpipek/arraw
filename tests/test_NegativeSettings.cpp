#include "CheckpointState.h"
#include "ProcessingPlan.h"
#include "RenderCheckpoint.h"
#include "support/Fixtures.h"
#include "support/TestImages.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopState.h>
#include <ImageImport.h>
#include <ImagePyramid.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <SettingsJson.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace arraw;

/// The settings of the negative (ADR 049): the type, the decode that reads it, and the plan's
/// decode block.

namespace {

constexpr std::string_view detailFixture = "bayer-detail-32x24.dng";

/// @brief Returns the largest absolute difference between two sixteen-bit buffers of one shape.
int maxDifference(const ImageBuffer& left, const ImageBuffer& right) {
    const auto a = left.samples<std::uint16_t>();
    const auto b = right.samples<std::uint16_t>();
    REQUIRE(a.size() == b.size());
    int worst = 0;
    for (std::size_t index = 0; index < a.size(); ++index) {
        worst = std::max(worst, std::abs(static_cast<int>(a[index]) - static_cast<int>(b[index])));
    }
    return worst;
}

/// @brief Decodes the detail fixture the way a photograph with a demosaic is decoded.
ImageBuffer decodedWith(Demosaic demosaic, bool halfSize = false) {
    const Photo photo = openPhoto(test::fixture(detailFixture)).with(NegativeSettings{demosaic});
    return loadImage(photo.path(), discardedDiagnostics(), decodeOptionsFor(photo, halfSize));
}

} // namespace

TEST_CASE("Every demosaic round-trips through its token", "[negative]") {
    REQUIRE(demosaicTokens.size() == demosaicCount);
    for (std::size_t index = 0; index < demosaicCount; ++index) {
        const auto demosaic = static_cast<Demosaic>(index);
        INFO(demosaicTokens[index]);
        REQUIRE(tokenOf(demosaic) == demosaicTokens[index]);
        REQUIRE(parseDemosaic(tokenOf(demosaic)) == demosaic);
    }
    REQUIRE(tokenOf(static_cast<Demosaic>(demosaicCount)).empty());
}

TEST_CASE("The tokens are stable", "[negative]") {
    // Persisted by sidecars and presets: these never change.
    REQUIRE(demosaicTokens == std::array<std::string_view, demosaicCount>{
                                  "linear", "vng", "ppg", "ahd", "dcb", "dht", "aahd"});
}

TEST_CASE("An unknown or upper-case token is refused", "[negative]") {
    for (const std::string_view token : {"", "VNG", "Vng", "ahd ", "amaze", "3", "vng\n"}) {
        INFO(token);
        REQUIRE_FALSE(parseDemosaic(token).has_value());
    }
}

TEST_CASE("The default negative demosaics with AHD", "[negative]") {
    REQUIRE(NegativeSettings{}.demosaic == Demosaic::Ahd);
    REQUIRE(DecodeOptions{}.demosaic == Demosaic::Ahd);
    REQUIRE(defaultNegativeFor(readImageMetadata(test::fixture(detailFixture))) ==
            NegativeSettings{});
    REQUIRE(openPhoto(test::fixture(detailFixture)).negative() == NegativeSettings{});
}

TEST_CASE("Validation refuses a demosaic that is not an enumerator", "[negative]") {
    for (std::size_t index = 0; index < demosaicCount; ++index) {
        REQUIRE_NOTHROW(validate(NegativeSettings{static_cast<Demosaic>(index)}));
    }
    REQUIRE_THROWS_AS(validate(NegativeSettings{static_cast<Demosaic>(demosaicCount)}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(validate(NegativeSettings{static_cast<Demosaic>(-1)}), std::invalid_argument);
    const Photo photo = openPhoto(test::fixture(detailFixture));
    REQUIRE_THROWS_AS(photo.with(NegativeSettings{static_cast<Demosaic>(99)}),
                      std::invalid_argument);
}

TEST_CASE("The mosaic of a file is read from its headers", "[negative][raw]") {
    REQUIRE(readImageMetadata(test::fixture("bayer-32x24.dng")).mosaic == Mosaic::Bayer);
    REQUIRE(readImageMetadata(test::fixture(detailFixture)).mosaic == Mosaic::Bayer);
    REQUIRE(readImageMetadata(test::fixture("linear-32x24-neutral.dng")).mosaic == Mosaic::None);
    REQUIRE(readImageMetadata(test::fixture("testcard-61x41-srgb8.png")).mosaic == Mosaic::None);
}

TEST_CASE("A photograph holds its negative beside its state", "[negative][photo]") {
    const Photo photo = openPhoto(test::fixture(detailFixture));
    const Photo vng = photo.with(NegativeSettings{Demosaic::Vng});
    REQUIRE(vng.negative().demosaic == Demosaic::Vng);
    REQUIRE(vng.state() == photo.state());
    REQUIRE(vng.marks() == photo.marks());
    REQUIRE_FALSE(vng == photo);
    // The other withers keep it.
    REQUIRE(vng.with(DevelopState{.settings = {.tone = {.exposure = 1.0F}}}).negative() ==
            vng.negative());
    REQUIRE(vng.with(PhotoMarks{.rating = 3}).negative() == vng.negative());
    const Photo built(photo.path(), photo.metadata(), NegativeSettings{Demosaic::Dcb},
                      photo.state());
    REQUIRE(built.negative().demosaic == Demosaic::Dcb);
}

TEST_CASE("Two demosaics decode a patterned mosaic differently, and a half-size decode alike",
          "[negative][raw]") {
    const ImageBuffer ahd = decodedWith(Demosaic::Ahd);
    const ImageBuffer vng = decodedWith(Demosaic::Vng);
    REQUIRE(ahd.size() == vng.size());
    REQUIRE(maxDifference(ahd, vng) > 0);

    // The default is what every decode did before the setting existed.
    REQUIRE(maxDifference(ahd, loadImage(test::fixture(detailFixture))) == 0);

    // Half-size combines 2x2 blocks and never demosaics.
    REQUIRE(maxDifference(decodedWith(Demosaic::Ahd, true), decodedWith(Demosaic::Vng, true)) == 0);
}

TEST_CASE("Every demosaic decodes the patterned mosaic", "[negative][raw]") {
    for (std::size_t index = 0; index < demosaicCount; ++index) {
        INFO(demosaicTokens[index]);
        const ImageBuffer decoded = decodedWith(static_cast<Demosaic>(index));
        REQUIRE(decoded.size() == ImageSize{32, 24});
    }
}

TEST_CASE("A decode says how it was made, in canonical form", "[negative][raw]") {
    const ImageBuffer vng = decodedWith(Demosaic::Vng);
    REQUIRE(vng.decodedWith() == DecodeOptions{.halfSize = false, .demosaic = Demosaic::Vng});
    REQUIRE(vng.clone().decodedWith() == vng.decodedWith());

    // Half-size never read the demosaic.
    REQUIRE(decodedWith(Demosaic::Vng, true).decodedWith() ==
            DecodeOptions{.halfSize = true, .demosaic = Demosaic::Ahd});

    // Nor did a file with no mosaic, nor a picture Qt decoded.
    const ImageBuffer linear = loadImage(test::fixture("linear-32x24-neutral.dng"),
                                         discardedDiagnostics(), {.demosaic = Demosaic::Vng});
    REQUIRE(linear.decodedWith() == DecodeOptions{});
    const ImageBuffer png =
        loadImage(test::fixture("testcard-61x41-srgb8.png"), discardedDiagnostics(),
                  {.halfSize = true, .demosaic = Demosaic::Dcb});
    REQUIRE(png.decodedWith() == DecodeOptions{});

    // The pyramid's levels keep it, and pixels nobody decoded have none.
    REQUIRE(halved(vng).decodedWith() == vng.decodedWith());
    REQUIRE_FALSE(
        test::rainbow({4, 4}, PixelFormat::RgbaF32, workingEncoding).decodedWith().has_value());
}

TEST_CASE("The options of a photograph are canonical", "[negative][photo]") {
    const Photo raw = openPhoto(test::fixture(detailFixture)).with(NegativeSettings{Demosaic::Ppg});
    REQUIRE(decodeOptionsFor(raw, false) ==
            DecodeOptions{.halfSize = false, .demosaic = Demosaic::Ppg});
    REQUIRE(decodeOptionsFor(raw, true) == DecodeOptions{.halfSize = true});

    const Photo linear =
        openPhoto(test::fixture("linear-32x24-neutral.dng")).with(NegativeSettings{Demosaic::Ppg});
    REQUIRE(decodeOptionsFor(linear, false) == DecodeOptions{});

    const Photo png =
        openPhoto(test::fixture("testcard-61x41-srgb8.png")).with(NegativeSettings{Demosaic::Ppg});
    REQUIRE(decodeOptionsFor(png, false) == DecodeOptions{});
}

TEST_CASE("A plan names the decode its source came from", "[negative][plan]") {
    const Photo photo =
        openPhoto(test::fixture(detailFixture)).with(NegativeSettings{Demosaic::Vng});
    const ImageBuffer source =
        loadImage(photo.path(), discardedDiagnostics(), decodeOptionsFor(photo, false));

    // The plan of the document and the plan of its pixels agree, and neither is the default's.
    REQUIRE(planFor(photo).decode == decodeOptionsFor(photo, false));
    REQUIRE(planFor(source, photo.state(), LensCorrection{}) == planFor(photo));
    REQUIRE_FALSE(planFor(photo) == planFor(photo.with(NegativeSettings{})));

    // The half-size decode of a photograph is not the decode its document plans.
    const ImageBuffer reduced =
        loadImage(photo.path(), discardedDiagnostics(), decodeOptionsFor(photo, true));
    REQUIRE(planFor(reduced, photo.state(), LensCorrection{}).decode ==
            DecodeOptions{.halfSize = true});

    // Pixels nobody decoded have no block.
    const ImageBuffer built = test::rainbow({4, 4}, PixelFormat::RgbaF32, workingEncoding);
    REQUIRE_FALSE(planFor(built, {}, LensCorrection{}).decode.has_value());
}

TEST_CASE("A negative edit clears ladders, and a rung of another decode is stale by value",
          "[negative][ladder]") {
    const DevelopState state{.settings = {.tone = {.exposure = 0.4F}}};
    const RenderRequest request{.size = RenderRequest::FitInside{16, 12}};

    const Photo photoA = openPhoto(test::fixture(detailFixture));
    const Photo photoB = photoA.with(NegativeSettings{Demosaic::Vng});
    const auto bufferA = std::make_shared<const ImageBuffer>(
        loadImage(photoA.path(), discardedDiagnostics(), decodeOptionsFor(photoA, false)));
    const auto bufferB = std::make_shared<const ImageBuffer>(
        loadImage(photoB.path(), discardedDiagnostics(), decodeOptionsFor(photoB, false)));
    REQUIRE(bufferA->size() == bufferB->size());

    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelop(ladder, bufferA, state, LensCorrection{}, request));
    REQUIRE(
        resumeOrDevelop(ladder, bufferA, state, LensCorrection{}, request).resumedFrom.has_value());

    // The same size, encoding and settings, another negative.
    const LadderRender other = resumeOrDevelop(ladder, bufferB, state, LensCorrection{}, request);
    REQUIRE_FALSE(other.resumedFrom.has_value());

    // A rung of A is stale against B's plan even when one buffer is reused: the by-value rule,
    // which does not lean on the ladder's binding.
    ImageBuffer reused = bufferA->clone();
    const RenderCheckpoint rung = developUntil(reused, state, LensCorrection{}, Stage::Pointwise);
    REQUIRE(canResumeFrom(rung, reused, state, LensCorrection{}, Stage::Resize));
    reused.setDecodedWith(bufferB->decodedWith());
    REQUIRE_FALSE(canResumeFrom(rung, reused, state, LensCorrection{}, Stage::Resize));
    REQUIRE_THROWS_AS(resumeFrom(rung, reused, state, LensCorrection{}, Stage::Resize),
                      std::invalid_argument);

    const auto why =
        staleReason(stateOf(rung), planFor(reused, state, LensCorrection{}), reused.size());
    REQUIRE(why.has_value());
    REQUIRE(std::string(*why).find("decoded differently") != std::string::npos);
}

TEST_CASE("The plan prefix compares the decode block before any stage", "[negative][plan]") {
    const ProcessingPlan original;
    auto changed = original;
    changed.decode = DecodeOptions{.demosaic = Demosaic::Vng};
    for (const Stage boundary :
         {Stage::Denoise, Stage::Pointwise, Stage::Geometry, Stage::Resize, Stage::Effects}) {
        REQUIRE_FALSE(prefixMatches(original, changed, boundary));
        REQUIRE(prefixMatches(changed, changed, boundary));
    }
    REQUIRE_FALSE(original == changed);
}

TEST_CASE("The negative round-trips through its JSON object", "[negative][json]") {
    for (std::size_t index = 0; index < demosaicCount; ++index) {
        const NegativeSettings negative{static_cast<Demosaic>(index)};
        const std::string json = negativeToJson(negative);
        INFO(json);
        REQUIRE(json == "{\"demosaic\": \"" + std::string(demosaicTokens[index]) + "\"}");
        REQUIRE(applyNegativeJson(json, NegativeSettings{Demosaic::Dht}) == negative);
    }
    REQUIRE_THROWS_AS(negativeToJson(NegativeSettings{static_cast<Demosaic>(99)}),
                      std::invalid_argument);
}

TEST_CASE("An absent demosaic key leaves the base, and bad ones are warned about",
          "[negative][json]") {
    const NegativeSettings base{Demosaic::Vng};
    REQUIRE(applyNegativeJson("{}", base) == base);

    CollectedDiagnostics log;
    REQUIRE(applyNegativeJson(R"({"demosaic": "amaze"})", base, log) == base);
    REQUIRE(applyNegativeJson(R"({"demosaic": 3})", base, log) == base);
    REQUIRE(applyNegativeJson(R"({"demosaic": "AHD"})", base, log) == base);
    REQUIRE(log.entries().size() == 3);
    for (const Diagnostic& entry : log.entries()) {
        REQUIRE(entry.notice == Notice::SettingMalformed);
    }

    CollectedDiagnostics unknown;
    REQUIRE(applyNegativeJson(R"({"sharpen": true})", base, unknown) == base);
    REQUIRE(unknown.entries().size() == 1);
    REQUIRE(unknown.entries().front().notice == Notice::SettingUnknown);

    REQUIRE_THROWS_AS(applyNegativeJson("[]", base), std::invalid_argument);
    REQUIRE_THROWS_AS(applyNegativeJson("nope", base), std::invalid_argument);
}

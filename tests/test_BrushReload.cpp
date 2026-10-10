// A state with brushes after a reload: named test 3 of the brush plan (section 8.1), on the CPU.

#include "support/Fixtures.h"
#include "support/LocalAdjustmentStates.h"
#include "support/TempDir.h"

#include <Develop.h>
#include <DevelopState.h>
#include <Diagnostics.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <LocalAdjustmentEdits.h>
#include <Photo.h>
#include <SettingsJson.h>
#include <Sidecar.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <string>

using namespace arraw;

namespace {

/// A smooth scene, different at every pixel.
ImageBuffer sceneOf(ImageSize size) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double shapes =
                0.5 * std::sin(x / 37.0) * std::cos(y / 23.0) + 0.3 * std::sin((x + y) / 61.0);
            const auto base = static_cast<float>(0.18 * std::exp2(shapes));
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = base * 1.2F;
            pixel[1] = base;
            pixel[2] = base * 0.7F;
            pixel[3] = 1.0F;
        }
    }
    return image;
}

/// Whether two images hold the same bits.
bool sameBits(const ImageBuffer& a, const ImageBuffer& b) {
    return a.size() == b.size() &&
           std::ranges::equal(a.samples<float>(), b.samples<float>(), [](float x, float y) {
               return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
           });
}

} // namespace

TEST_CASE("Brushes after a reload are equal and render the same bits",
          "[brush][persistence][reload]") {
    // Three brushes (paint, erase, inverted), a disabled empty one and a radial mask.
    const DevelopState state = test::stateWithBrushes();
    const ImageBuffer source = sceneOf({256, 192});
    const ImageBuffer before = develop(source, state, {});

    SECTION("through the state JSON") {
        CollectedDiagnostics log;
        const DevelopState back = applyStateJson(stateToJson(state), DevelopState{}, log);
        CHECK(log.entries().empty());
        REQUIRE(back == state);
        // The reloaded lists are other objects: equality is by contents.
        CHECK(std::get<BrushMask>(back.localAdjustments[0].shape).strokes !=
              std::get<BrushMask>(state.localAdjustments[0].shape).strokes);
        CHECK(sameBits(before, develop(source, back, {})));
        CHECK(sameBits(sample(source, state, Tap::CurveInput, {}),
                       sample(source, back, Tap::CurveInput, {})));
    }
    SECTION("through the XMP sidecar") {
        const test::TempDir directory;
        const std::filesystem::path photo = directory.file("IMG_1.dng");
        std::filesystem::copy_file(test::fixture("linear-32x24-neutral.dng"), photo);
        writeSidecar(openPhoto(photo).with(state));
        CollectedDiagnostics log;
        const auto contents = readSidecar(photo, log);
        REQUIRE(contents);
        REQUIRE(contents->state);
        CHECK(log.entries().empty());
        REQUIRE(*contents->state == state);
        CHECK(sameBits(before, develop(source, *contents->state, {})));
        // And once more through a second write, which says nothing new.
        writeSidecar(openPhoto(photo).with(*contents->state));
        CHECK(readSidecar(photo)->state == state);
    }
    SECTION("the render does something") {
        CHECK_FALSE(sameBits(before, develop(source, DevelopState{}, {})));
    }
}

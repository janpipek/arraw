#include "core/ImageBuffer.h"
#include "pipeline/LensCorrection.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Flat width×height buffer, every pixel set to (v, v, v).
static ImageBuffer makeFlatBuffer(int width, int height, float v) {
    ImageBuffer buf;
    buf.width = width;
    buf.height = height;
    buf.data.assign(static_cast<size_t>(width * height * 3), v);
    return buf;
}

// Green channel of pixel (x, y).
static float pixel(const ImageBuffer& buf, int x, int y) {
    return buf.data[static_cast<size_t>((y * buf.width + x) * 3 + 1)];
}

// Channel c of pixel (x, y).
static float pixelC(const ImageBuffer& buf, int x, int y, int c) {
    return buf.data[static_cast<size_t>((y * buf.width + x) * 3 + c)];
}

// Horizontal ramp: every pixel (x, y) = (x / (width-1)) in all three channels, so a
// pixel's value reveals which source column it was sampled from.
static ImageBuffer makeHRampBuffer(int width, int height) {
    ImageBuffer buf;
    buf.width = width;
    buf.height = height;
    buf.data.resize(static_cast<size_t>(width * height * 3));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v = static_cast<float>(x) / static_cast<float>(width - 1);
            const size_t i = static_cast<size_t>((y * width + x) * 3);
            buf.data[i] = buf.data[i + 1] = buf.data[i + 2] = v;
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// RadialCurve (step 1 — the seam primitive)
// ---------------------------------------------------------------------------

TEST_CASE("RadialCurve::identityGain samples to 1.0 everywhere", "[lens]") {
    const RadialCurve c = RadialCurve::identityGain();
    REQUIRE_THAT(c.sample(0.0f), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(c.sample(0.5f), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(c.sample(1.0f), WithinAbs(1.0, 1e-6));
}

TEST_CASE("RadialCurve::fromFn interpolates linearly between nodes", "[lens]") {
    const RadialCurve c = RadialCurve::fromFn([](float r) { return 1.0f + r; }); // 1.0 .. 2.0
    REQUIRE_THAT(c.sample(0.0f), WithinAbs(1.0, 1e-5));
    REQUIRE_THAT(c.sample(0.5f), WithinAbs(1.5, 1e-3));
    REQUIRE_THAT(c.sample(1.0f), WithinAbs(2.0, 1e-5));
}

// ---------------------------------------------------------------------------
// applyLensCorrection — step 1: identity / gating contract
// ---------------------------------------------------------------------------

TEST_CASE("applyLensCorrection with an empty model is a no-op", "[lens]") {
    const ImageBuffer buf = makeFlatBuffer(16, 12, 0.4f);
    const LensCorrectionToggles allOn{.distortion = true, .vignetting = true, .ca = true};
    const ImageBuffer out = applyLensCorrection(buf, LensCorrectionModel{}, allOn);
    REQUIRE(out.data == buf.data);
}

TEST_CASE("applyLensCorrection skips vignetting when its toggle is off", "[lens]") {
    const ImageBuffer buf = makeFlatBuffer(16, 12, 0.4f);
    LensCorrectionModel model;
    model.vignette = RadialCurve::fromFn([](float r) { return 1.0f + r; });
    model.hasVignetting = true;
    const LensCorrectionToggles vigOff{.distortion = false, .vignetting = false, .ca = false};
    const ImageBuffer out = applyLensCorrection(buf, model, vigOff);
    REQUIRE(out.data == buf.data);
}

TEST_CASE("applyLensCorrection skips vignetting when the model lacks it", "[lens]") {
    const ImageBuffer buf = makeFlatBuffer(16, 12, 0.4f);
    LensCorrectionModel model; // hasVignetting stays false
    model.vignette = RadialCurve::fromFn([](float r) { return 1.0f + r; });
    const LensCorrectionToggles vigOn{.distortion = false, .vignetting = true, .ca = false};
    const ImageBuffer out = applyLensCorrection(buf, model, vigOn);
    REQUIRE(out.data == buf.data);
}

// ---------------------------------------------------------------------------
// applyLensCorrection — step 2: vignetting gain
// ---------------------------------------------------------------------------

TEST_CASE("vignetting applies the radial gain: centre unchanged, corners brightened", "[lens]") {
    const float v = 0.25f;
    const ImageBuffer buf = makeFlatBuffer(64, 48, v);

    LensCorrectionModel model;
    model.center = {0.5, 0.5};
    model.vignette = RadialCurve::fromFn([](float r) { return 1.0f + r; }); // gain 1.0 (centre) .. 2.0 (corner)
    model.hasVignetting = true;
    const LensCorrectionToggles vigOn{.distortion = false, .vignetting = true, .ca = false};

    const ImageBuffer out = applyLensCorrection(buf, model, vigOn);

    // Centre pixel: r ~ 0, gain ~ 1.0 → unchanged.
    REQUIRE_THAT(pixel(out, 32, 24), WithinAbs(v, 0.01));

    // Corner pixel: r ~ 1, gain ~ 2.0 → ~doubled.
    REQUIRE_THAT(pixel(out, 0, 0), WithinAbs(v * 2.0f, 0.03));

    // Monotonic falloff removal: corner brighter than mid-radius brighter than centre.
    REQUIRE(pixel(out, 0, 0) > pixel(out, 16, 12));
    REQUIRE(pixel(out, 16, 12) > pixel(out, 32, 24));
}

TEST_CASE("vignetting honours the optical centre offset", "[lens]") {
    const float v = 0.25f;
    const ImageBuffer buf = makeFlatBuffer(64, 48, v);

    LensCorrectionModel model;
    model.center = {0.0, 0.0}; // centre at top-left corner
    model.vignette = RadialCurve::fromFn([](float r) { return 1.0f + r; });
    model.hasVignetting = true;
    const LensCorrectionToggles vigOn{.distortion = false, .vignetting = true, .ca = false};

    const ImageBuffer out = applyLensCorrection(buf, model, vigOn);

    // Now the top-left is the centre (gain ~1) and the opposite corner is farthest.
    REQUIRE_THAT(pixel(out, 0, 0), WithinAbs(v, 0.02));
    REQUIRE(pixel(out, 63, 47) > pixel(out, 0, 0));
}

// ---------------------------------------------------------------------------
// applyLensCorrection — step 3: distortion warp (radial scale, bilinear resample)
// ---------------------------------------------------------------------------

TEST_CASE("distortion with an identity scale leaves the image unchanged", "[lens]") {
    const ImageBuffer buf = makeHRampBuffer(64, 48);
    LensCorrectionModel model;
    model.center = {0.5, 0.5};
    model.distortion = RadialCurve::identityGain(); // scale 1.0 everywhere
    model.hasDistortion = true;
    const LensCorrectionToggles distOn{.distortion = true, .vignetting = false, .ca = false};

    const ImageBuffer out = applyLensCorrection(buf, model, distOn);

    // Interior pixels resample from their own location (corners may touch the clamp).
    for (int x = 4; x < 60; x += 8)
        REQUIRE_THAT(pixel(out, x, 24), WithinAbs(pixel(buf, x, 24), 1e-4));
}

TEST_CASE("distortion is gated by model flag and toggle", "[lens]") {
    const ImageBuffer buf = makeHRampBuffer(64, 48);
    LensCorrectionModel model;
    model.distortion = RadialCurve::fromFn([](float) { return 0.5f; });

    // Model lacks distortion → no-op even with the toggle on.
    model.hasDistortion = false;
    REQUIRE(applyLensCorrection(buf, model, {.distortion = true}).data == buf.data);

    // Model has it but the toggle is off → no-op.
    model.hasDistortion = true;
    REQUIRE(applyLensCorrection(buf, model, {.distortion = false}).data == buf.data);
}

TEST_CASE("a uniform inward scale magnifies: edges sample from nearer the centre", "[lens]") {
    const ImageBuffer buf = makeHRampBuffer(64, 48);
    LensCorrectionModel model;
    model.center = {0.5, 0.5}; // centre at column 32
    model.distortion = RadialCurve::fromFn([](float) { return 0.5f; }); // src = centre + 0.5*(dst-centre)
    model.hasDistortion = true;
    const LensCorrectionToggles distOn{.distortion = true, .vignetting = false, .ca = false};

    const ImageBuffer out = applyLensCorrection(buf, model, distOn);

    // Output right-edge (x=63, centre row): src.x = 32 + (63.5-32)*0.5 = 47.75 → value ≈ 47.75/63.
    REQUIRE_THAT(pixel(out, 63, 24), WithinAbs(47.75f / 63.0f, 0.01));
    // Centre column unchanged (its vector from the centre is ~0).
    REQUIRE_THAT(pixel(out, 32, 24), WithinAbs(pixel(buf, 32, 24), 0.02));
    // The ramp is compressed toward the centre: output edges are pulled inward.
    REQUIRE(pixel(out, 63, 24) < pixel(buf, 63, 24));
    REQUIRE(pixel(out, 0, 24) > pixel(buf, 0, 24));
}

// ---------------------------------------------------------------------------
// autoFillZoom — step 4: auto-scale distortion to fill the frame
// ---------------------------------------------------------------------------

TEST_CASE("autoFillZoom is 1.0 when there is nothing to fill", "[lens]") {
    LensCorrectionModel model;
    // No distortion at all.
    REQUIRE_THAT(autoFillZoom(model, 64, 48), WithinAbs(1.0, 1e-6));

    // Identity distortion: every pixel samples itself, already in-bounds.
    model.distortion = RadialCurve::identityGain();
    model.hasDistortion = true;
    REQUIRE_THAT(autoFillZoom(model, 64, 48), WithinAbs(1.0, 1e-6));

    // Shrinking distortion: corners sample nearer the centre, still in-bounds.
    model.distortion = RadialCurve::fromFn([](float) { return 0.5f; });
    REQUIRE_THAT(autoFillZoom(model, 64, 48), WithinAbs(1.0, 1e-6));
}

TEST_CASE("autoFillZoom solves the binding edge for an expanding distortion", "[lens]") {
    LensCorrectionModel model;
    model.center = {0.5, 0.5}; // centre column 32 of 64
    model.distortion = RadialCurve::fromFn([](float) { return 2.0f; }); // 2x magnification
    model.hasDistortion = true;
    // Left/right edges bind: src.x = 32 +/- 63/z must stay in [0,64] → z = 63/32.
    REQUIRE_THAT(autoFillZoom(model, 64, 48), WithinAbs(63.0 / 32.0, 0.02));
}

TEST_CASE("an expanding distortion fills without clamp-smearing the edges", "[lens]") {
    const ImageBuffer buf = makeHRampBuffer(64, 48);
    LensCorrectionModel model;
    model.center = {0.5, 0.5};
    model.distortion = RadialCurve::fromFn([](float) { return 2.0f; });
    model.hasDistortion = true;
    const LensCorrectionToggles distOn{.distortion = true, .vignetting = false, .ca = false};

    const ImageBuffer out = applyLensCorrection(buf, model, distOn);

    // At output (48,24) with zoom 63/32: src.x = 32 + (16.5 / (63/32)) * 2 ≈ 48.76 → ramp value.
    // A real interior value proves the fill zoom kicked in instead of clamping to 1.0.
    REQUIRE_THAT(pixel(out, 48, 24), WithinAbs(48.76f / 63.0f, 0.02));
    REQUIRE(pixel(out, 48, 24) < 0.95f);
}

// ---------------------------------------------------------------------------
// applyLensCorrection — step 5: lateral chromatic aberration (per-channel scale)
// ---------------------------------------------------------------------------

TEST_CASE("TCA is gated by model flag and toggle", "[lens]") {
    const ImageBuffer buf = makeHRampBuffer(64, 48);
    LensCorrectionModel model;
    model.tcaR = RadialCurve::fromFn([](float) { return 0.5f; });
    model.tcaB = RadialCurve::fromFn([](float) { return 2.0f; });

    model.hasTCA = false; // model lacks TCA → no-op even with the toggle on
    REQUIRE(applyLensCorrection(buf, model, {.ca = true}).data == buf.data);

    model.hasTCA = true; // toggle off → no-op
    REQUIRE(applyLensCorrection(buf, model, {.ca = false}).data == buf.data);
}

TEST_CASE("identity TCA scales leave the image unchanged", "[lens]") {
    const ImageBuffer buf = makeHRampBuffer(64, 48);
    LensCorrectionModel model;
    model.tcaR = RadialCurve::identityGain();
    model.tcaB = RadialCurve::identityGain();
    model.hasTCA = true;
    const ImageBuffer out = applyLensCorrection(buf, model, {.ca = true});
    for (int x = 4; x < 60; x += 8)
        for (int c = 0; c < 3; ++c)
            REQUIRE_THAT(pixelC(out, x, 24, c), WithinAbs(pixelC(buf, x, 24, c), 1e-4));
}

TEST_CASE("TCA moves red and blue along the radius while green stays put", "[lens]") {
    const ImageBuffer buf = makeHRampBuffer(64, 48); // R == G == B everywhere
    LensCorrectionModel model;
    model.center = {0.5, 0.5}; // centre column 32
    model.tcaR = RadialCurve::fromFn([](float) { return 0.5f; }); // red sampled nearer centre
    model.tcaB = RadialCurve::fromFn([](float) { return 2.0f; }); // blue sampled farther out
    model.hasTCA = true;
    const LensCorrectionToggles caOn{.distortion = false, .vignetting = false, .ca = true};

    const ImageBuffer out = applyLensCorrection(buf, model, caOn);

    // Output right edge (x=63, centre row): vx = 63.5-32 = 31.5.
    //   green: src.x = 32 + 31.5      = 63.5 → ~1.0
    //   red:   src.x = 32 + 31.5*0.5  = 47.75 → ~47.75/63
    //   blue:  src.x = 32 + 31.5*2.0  = 95   → clamped to col 63 → ~1.0
    REQUIRE_THAT(pixelC(out, 63, 24, 0), WithinAbs(47.75f / 63.0f, 0.01)); // red pulled in
    REQUIRE(pixelC(out, 63, 24, 0) < pixelC(out, 63, 24, 1));              // red < green
    REQUIRE_THAT(pixelC(out, 63, 24, 1), WithinAbs(1.0f, 0.02));           // green unchanged
    REQUIRE_THAT(pixelC(out, 63, 24, 2), WithinAbs(1.0f, 0.02));           // blue clamped at edge
}

// Hidden ([.]) micro-benchmark: time one preview-sized warp in this build config.
// Run: arraw_tests "[.perf]"
TEST_CASE("benchmark: preview-sized lens warp", "[.][perf]") {
    ImageBuffer buf;
    buf.width = 3096; // half of a 6192-wide Sony frame
    buf.height = 2064;
    buf.data.assign(static_cast<size_t>(buf.width * buf.height * 3), 0.5f);

    LensCorrectionModel model;
    model.distortion = RadialCurve::fromFn([](float r) { return 1.0f + 0.03f * r * r; });
    model.tcaR = RadialCurve::fromFn([](float r) { return 1.0f + 0.001f * r; });
    model.tcaB = RadialCurve::fromFn([](float r) { return 1.0f - 0.001f * r; });
    model.vignette = RadialCurve::fromFn([](float r) { return 1.0f + 0.4f * r * r; });
    model.hasDistortion = model.hasTCA = model.hasVignetting = true;
    const LensCorrectionToggles all{.distortion = true, .vignetting = true, .ca = true};

    const auto t0 = std::chrono::steady_clock::now();
    const ImageBuffer out = applyLensCorrection(buf, model, all);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    REQUIRE(out.valid());
    WARN("preview warp (3096x2064, all corrections): " << ms << " ms");
}

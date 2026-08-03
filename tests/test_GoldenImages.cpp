#include "core/ImageBuffer.h"
#include "develop/GlobalAdjustment.h"
#include "develop/LocalAdjustment.h"
// Golden-image tests for the GLSL pipeline, via the real export path
// (offscreen::renderToImage, docs/adr/0049) — policy, thresholds, and format
// are docs/adr/0005. Regenerate goldens with:
//
//   ARRAW_UPDATE_GOLDENS=1 ./build/tests/arraw_tests "[golden]"

#include "TestApp.h"
#include "pipeline/OkLab.h"
#include "render/HeadlessRenderContext.h"
#include "render/OffscreenRender.h"
#include "render/RendererCore.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QImage>

namespace {

// ── ADR 0005 thresholds (loose per-pixel + tight mean, any GPU) ──────────────
constexpr float kMaxPixelDiff = 4.0f / 255.0f;
constexpr float kMaxChannelMean = 0.3f / 255.0f;

// ── Headless renderer (docs/adr/0049) — no widget, no event loop ─────────────

RendererCore* goldenCore() {
    testApp(); // the platform plugin must exist before GL context creation

    // ctx is declared before core so it is destroyed after it (reverse order):
    // RendererCore's GPU resources must be released while the QRhi is alive.
    static std::unique_ptr<HeadlessRenderContext> ctx = HeadlessRenderContext::create();
    static std::unique_ptr<RendererCore> core = [] {
        auto c = std::make_unique<RendererCore>();
        if (ctx)
            c->initialize(ctx->rhi());
        return c;
    }();
    return (ctx && core->ready()) ? core.get() : nullptr;
}

// ── Synthetic input: grey gradient over color bars (see ADR 0005) ────────────

ImageBuffer syntheticScene() {
    constexpr int W = 64, H = 48;
    static const float bars[8][3] = {
        {1, 0, 0},
        {0, 1, 0},
        {0, 0, 1},
        {0, 1, 1},
        {1, 0, 1},
        {1, 1, 0},
        {1, 1, 1},
        {0.02f, 0.02f, 0.02f},
    };
    ImageBuffer b;
    b.width = W;
    b.height = H;
    b.data.resize(W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = b.data.data() + (size_t(y) * W + x) * 3;
            if (y < H / 2) {
                p[0] = p[1] = p[2] = x / (W - 1.0f);
            } else {
                const float fade = 0.3f + 0.7f * (y - H / 2) / (H / 2 - 1.0f);
                const float* bar = bars[x * 8 / W];
                for (int c = 0; c < 3; ++c)
                    p[c] = bar[c] * fade;
            }
        }
    return b;
}

// ── PFM I/O (color "PF", little-endian, rows bottom-up per spec) ──────────────

bool writePfm(const QString& path, const QImage& img) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QStringLiteral("PF\n%1 %2\n-1.0\n").arg(img.width()).arg(img.height()).toLatin1());
    for (int y = img.height() - 1; y >= 0; --y) {
        const float* px = reinterpret_cast<const float*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            f.write(reinterpret_cast<const char*>(px + x * 4), 3 * sizeof(float));
    }
    return f.error() == QFileDevice::NoError;
}

QImage readPfm(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    if (f.readLine().trimmed() != "PF")
        return {};
    const QList<QByteArray> dims = f.readLine().trimmed().split(' ');
    if (dims.size() != 2)
        return {};
    const int w = dims[0].toInt(), h = dims[1].toInt();
    if (f.readLine().trimmed().toFloat() >= 0.0f)
        return {}; // big-endian unsupported
    QImage img(w, h, QImage::Format_RGBX32FPx4);
    for (int y = h - 1; y >= 0; --y) {
        float* px = reinterpret_cast<float*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            if (f.read(reinterpret_cast<char*>(px + x * 4), 3 * sizeof(float)) != 3 * sizeof(float))
                return {};
            px[x * 4 + 3] = 1.0f;
        }
    }
    return img;
}

// ── Comparison: loose per-pixel + tight per-channel mean ─────────────────────

struct DiffStats {
    float maxDiff = 0.0f;
    float meanDiff[3] = {}; // signed, per channel — catches systematic drift
};

DiffStats diff(const QImage& a, const QImage& b) {
    DiffStats s;
    double sum[3] = {};
    for (int y = 0; y < a.height(); ++y) {
        const float* pa = reinterpret_cast<const float*>(a.constScanLine(y));
        const float* pb = reinterpret_cast<const float*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            for (int c = 0; c < 3; ++c) {
                const float d = pa[x * 4 + c] - pb[x * 4 + c];
                s.maxDiff = std::max(s.maxDiff, std::abs(d));
                sum[c] += d;
            }
    }
    const double n = double(a.width()) * a.height();
    for (int c = 0; c < 3; ++c)
        s.meanDiff[c] = float(sum[c] / n);
    return s;
}

struct Scenario {
    const char* name;
    GlobalAdjustment params;
};

std::vector<Scenario> scenarios() {
    std::vector<Scenario> list;

    list.push_back({"neutral", {}});

    GlobalAdjustment p;
    p.exposure = 1.5f;
    list.push_back({"exposure_plus15ev", p});

    p = {};
    p.contrast = 60.0f;
    p.highlights = -50.0f;
    p.shadows = 40.0f;
    p.whites = 20.0f;
    p.blacks = -20.0f;
    list.push_back({"tone_regions", p});

    p = {};
    p.temperature = 3200.0f;
    p.tint = 25.0f;
    list.push_back({"white_balance", p});

    p = {};
    p.saturation = 35.0f;
    p.vibrance = 40.0f;
    p.hslHue[0] = 40.0f;  // red hue shift
    p.hslSat[5] = -50.0f; // blue desaturation
    p.hslLum[2] = 30.0f;  // yellow luminance
    list.push_back({"color_hsl", p});

    p = {};
    p.curveLuma.points = {{0.0, 0.0}, {0.25, 0.15}, {0.75, 0.85}, {1.0, 1.0}};
    p.curveR.points = {{0.0, 0.1}, {1.0, 0.9}};
    list.push_back({"tone_curve", p});

    p = {};
    p.cropRect = QRectF(0.25, 0.125, 0.5, 0.5);
    p.rotation = 10.0f;
    list.push_back({"crop_rotate", p});

    // Filmic Highlights (docs/adr/0040): push exposure to generate headroom and
    // saturated highlights, then exercise the shoulder + path to white.
    p = {};
    p.exposure = 1.5f;
    p.saturation = 30.0f;
    p.filmicHighlights = 80.0f;
    list.push_back({"filmic_highlights", p});

    // Black & White (docs/adr/0048): the hue mixer lightens the red bar and
    // darkens the blue one, and every output pixel must be neutral grey. Mirrors
    // colour::applyBlackAndWhite, so this enforces CPU/GPU parity for the mixer.
    p = {};
    p.convertToGrayscale = true;
    p.bwMix = {60.0f, 0.0f, 0.0f, 0.0f, 0.0f, -80.0f, 0.0f, 0.0f};
    list.push_back({"black_and_white", p});

    // Colour Grading (docs/adr/0052): a live split tone — warm Shadows against cool
    // Highlights, off-centre Balance and non-default Blending, so every term of the
    // zone blend is exercised. Mirrors colour::applyColourGrading, so this is what
    // enforces CPU/GPU parity for the grade (and that the vec4[2] uniform packing
    // reaches the right shader components — a default grade is the identity and
    // would render identically however the block were packed).
    p = {};
    p.colorGradeHue = {40.0f, 0.0f, 250.0f};
    p.colorGradeSat = {70.0f, 0.0f, 55.0f};
    p.colorGradeBalance = -30.0f;
    p.colorGradeBlending = 25.0f;
    list.push_back({"colour_grading", p});

    // The same grade on the Black & White treatment — the feature's reason to
    // exist (ADR 0048's deferred toning). Grading runs after the branch merges, so
    // unlike HSL/Saturation it must still tint the neutral grey.
    p.convertToGrayscale = true;
    list.push_back({"colour_grading_bw", p});

    return list;
}

} // namespace

TEST_CASE("shader pipeline matches golden renders", "[gpu][golden]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    const ImageBuffer scene = syntheticScene();
    const bool update = qEnvironmentVariableIsSet("ARRAW_UPDATE_GOLDENS");
    const QString goldenDir = QStringLiteral(ARRAW_FIXTURE_DIR "/golden");
    if (update)
        QDir().mkpath(goldenDir);

    for (const auto& sc : scenarios()) {
        DYNAMIC_SECTION(sc.name) {
            const int outW = int(std::lround(sc.params.cropRect.width() * scene.width));
            const int outH = int(std::lround(sc.params.cropRect.height() * scene.height));

            // Same sequence as MainWindow's export: offscreen::renderToImage
            // sets the curve LUT from sc.params before the offscreen render.
            const QImage got = offscreen::renderToImage(*core, scene, sc.params, outW, outH);
            REQUIRE_FALSE(got.isNull());
            REQUIRE(got.format() == QImage::Format_RGBX32FPx4);

            const QString path = goldenDir + "/" + sc.name + ".pfm";
            if (update) {
                REQUIRE(writePfm(path, got));
                SUCCEED("golden updated: " << sc.name);
                continue;
            }

            const QImage want = readPfm(path);
            INFO("golden: " << path.toStdString() << " (regenerate with ARRAW_UPDATE_GOLDENS=1)");
            REQUIRE_FALSE(want.isNull());
            REQUIRE(want.size() == got.size());

            const DiffStats d = diff(got, want);
            INFO(
                "maxDiff=" << d.maxDiff * 255.0f << "/255, meanDiff=(" << d.meanDiff[0] * 255.0f
                           << ", " << d.meanDiff[1] * 255.0f << ", " << d.meanDiff[2] * 255.0f
                           << ")/255");
            CHECK(d.maxDiff <= kMaxPixelDiff);
            for (int c = 0; c < 3; ++c)
                CHECK(std::abs(d.meanDiff[c]) <= kMaxChannelMean);
        }
    }
}

// White balance is a multiplicative gain (docs/adr/0025): a black pixel can
// never acquire colour, however extreme the temperature/tint. The additive
// model this replaced turned black into saturated red at 12000 K. This drives
// the whole GPU chain (std140 packing, fillUbuf, the shader multiply).
TEST_CASE("black stays black through the white-balance gain", "[gpu][whitebalance]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    const ImageBuffer scene = syntheticScene(); // x==0, top half is pure black

    for (const float tint : {-100.0f, 0.0f, 100.0f}) {
        for (const float kelvin : {2000.0f, 12000.0f}) {
            DYNAMIC_SECTION("K=" << kelvin << " tint=" << tint) {
                GlobalAdjustment p;
                p.temperature = kelvin;
                p.tint = tint;
                const QImage got = offscreen::renderToImage(*core, scene, p, scene.width, scene.height);
                REQUIRE_FALSE(got.isNull());

                // Black column (x == 0) in the top (grey-ramp) half stays black.
                const float* px = reinterpret_cast<const float*>(got.constScanLine(5));
                INFO("black pixel -> (" << px[0] << ", " << px[1] << ", " << px[2] << ")");
                CHECK(px[0] <= 1e-4f);
                CHECK(px[1] <= 1e-4f);
                CHECK(px[2] <= 1e-4f);
            }
        }
    }
}

// Local adjustments (docs/adr/0010): a behavioural check of the whole GPU chain
// — std140 packing, fillUbuf, the shader loop, and the GLSL maskWeight port.
// A +1 EV local exposure on a Linear mask must brighten only the masked region.
TEST_CASE("a local exposure mask brightens only the masked region", "[gpu][localadj]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    // Flat mid-grey scene, so any brightness difference is the mask's doing.
    ImageBuffer scene;
    scene.width = 64;
    scene.height = 48;
    scene.data.assign(size_t(scene.width) * scene.height * 3, 0.3f);

    GlobalAdjustment p;
    LocalAdjustment la;
    // Vertical gradient line near centre: left of x=0.4 → weight 0,
    // right of x=0.6 → weight 1.
    la.mask = LinearMask{{0.4, 0.5}, {0.6, 0.5}};
    la.exposure = 1.0f; // +1 EV on the masked side
    p.localAdjustments.push_back(la);

    const QImage got = offscreen::renderToImage(*core, scene, p, scene.width, scene.height);
    REQUIRE_FALSE(got.isNull());
    REQUIRE(got.format() == QImage::Format_RGBX32FPx4);

    auto value = [&](float fx, int y) {
        const float* px = reinterpret_cast<const float*>(got.constScanLine(y));
        return px[int(fx * scene.width) * 4]; // grey scene → R channel suffices
    };
    const int y = scene.height / 2;
    const float unmasked = value(0.1f, y); // weight ~0, untouched
    const float masked = value(0.9f, y);   // weight ~1, +1 EV

    INFO("unmasked=" << unmasked << " masked=" << masked);
    CHECK(masked > unmasked + 0.1f); // +1 EV gives a substantial perceptual lift
}

// Colour Grading (docs/adr/0052): a behavioural check of the whole GPU chain —
// the colorGrade vec4[2] std140 packing, fillUbuf's element order, and the
// shader's mirror of colour::applyColourGrading. Worth having on top of the
// golden renders: a *default* grade is the exact identity, so the shader linking
// and rendering proves nothing about the packing. Scrambling hue with sat, or
// balance with blending, still renders every existing golden byte-for-byte —
// but breaks the zone selectivity below.
TEST_CASE("colour grading tints the right tonal zones on the GPU", "[gpu][grade]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    // Flat neutral scenes: default WB is a unit gain (5500K/tint 0), so any chroma
    // in the output is the grade's doing and nothing else's.
    auto flat = [](float level) {
        ImageBuffer b;
        b.width = 16;
        b.height = 16;
        b.data.assign(size_t(b.width) * b.height * 3, level);
        return b;
    };
    const ImageBuffer dark = flat(0.02f);
    const ImageBuffer mid = flat(0.18f);
    const ImageBuffer bright = flat(0.7f);

    // Oklab chroma of a centre pixel of the render.
    auto chromaOf = [&](const ImageBuffer& scene, const GlobalAdjustment& p) {
        const QImage got = offscreen::renderToImage(*core, scene, p, scene.width, scene.height);
        REQUIRE_FALSE(got.isNull());
        const auto* px = reinterpret_cast<const float*>(got.constScanLine(scene.height / 2));
        const colour::Lab lab = colour::toOklab({px[0], px[1], px[2]});
        return std::hypot(lab.a, lab.b);
    };

    const GlobalAdjustment ungraded;
    CHECK(chromaOf(dark, ungraded) < 1e-3f); // neutral in, neutral out
    CHECK(chromaOf(bright, ungraded) < 1e-3f);

    SECTION("a Shadows-only grade tints dark tones more than bright ones") {
        GlobalAdjustment p;
        p.colorGradeHue = {40.0f, 0.0f, 0.0f};
        p.colorGradeSat = {80.0f, 0.0f, 0.0f};
        CHECK(chromaOf(dark, p) > chromaOf(bright, p));
        CHECK(chromaOf(dark, p) > 0.01f);
    }

    SECTION("a Highlights-only grade tints bright tones more than dark ones") {
        GlobalAdjustment p;
        p.colorGradeHue = {0.0f, 0.0f, 250.0f};
        p.colorGradeSat = {0.0f, 0.0f, 80.0f};
        CHECK(chromaOf(bright, p) > chromaOf(dark, p));
        CHECK(chromaOf(bright, p) > 0.01f);
    }

    // Balance and Blending live in the same vec4 as the Highlights zone, so these
    // two are what catch a swap between them.
    SECTION("Balance hands the tonal range to Shadows when negative") {
        GlobalAdjustment p;
        p.colorGradeSat = {80.0f, 0.0f, 0.0f};
        p.colorGradeHue = {40.0f, 0.0f, 0.0f};
        const float centred = chromaOf(mid, p);
        p.colorGradeBalance = -100.0f;
        CHECK(chromaOf(mid, p) > centred);
        p.colorGradeBalance = 100.0f;
        CHECK(chromaOf(mid, p) < centred);
    }

    SECTION("Blending widens a Shadows zone into the midtones") {
        GlobalAdjustment p;
        p.colorGradeSat = {80.0f, 0.0f, 0.0f};
        p.colorGradeHue = {40.0f, 0.0f, 0.0f};
        p.colorGradeBlending = 0.0f;
        const float narrow = chromaOf(mid, p);
        p.colorGradeBlending = 100.0f;
        CHECK(chromaOf(mid, p) > narrow);
    }

    // The feature's reason to exist (docs/adr/0048's deferred toning): grading runs
    // after the Colour/B&W branch merges, so unlike HSL/Saturation it must still
    // reach the neutral grey the mixer produced.
    SECTION("it tints the Black & White treatment's neutral grey") {
        GlobalAdjustment mono;
        mono.convertToGrayscale = true;
        CHECK(chromaOf(mid, mono) < 1e-3f); // B&W alone is neutral

        GlobalAdjustment toned = mono;
        toned.colorGradeHue = {40.0f, 40.0f, 40.0f};
        toned.colorGradeSat = {70.0f, 70.0f, 70.0f};
        CHECK(chromaOf(mid, toned) > 0.01f); // sepia on the grey
    }
}

TEST_CASE("a later Local Adjustment can recover global white headroom", "[gpu][tone]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    ImageBuffer scene;
    scene.width = 16;
    scene.height = 16;
    scene.data.assign(size_t(scene.width) * scene.height * 3, 0.9f);

    GlobalAdjustment globalOnly;
    globalOnly.whites = 100.0f;
    // Isolate the headroom/recovery mechanism from the end-of-chain shoulder:
    // Filmic Highlights defaults to 25 (docs/adr/0040) and would compress the
    // over-white value back below 1.0, masking the headroom this test checks.
    globalOnly.filmicHighlights = 0.0f;
    const QImage over = offscreen::renderToImage(*core, scene, globalOnly, scene.width, scene.height);
    REQUIRE_FALSE(over.isNull());
    const float overWhite = reinterpret_cast<const float*>(over.constScanLine(8))[8 * 4];
    REQUIRE(overWhite > 1.0f);

    GlobalAdjustment recovered = globalOnly;
    LocalAdjustment local;
    local.mask = LinearMask{{-2.0, 0.5}, {-1.0, 0.5}}; // weight 1 over the whole frame
    local.whites = -100.0f;
    recovered.localAdjustments.push_back(local);
    const QImage under = offscreen::renderToImage(*core, scene, recovered, scene.width, scene.height);
    REQUIRE_FALSE(under.isNull());
    const float recoveredWhite = reinterpret_cast<const float*>(under.constScanLine(8))[8 * 4];

    CHECK(recoveredWhite < 1.0f);
    CHECK(recoveredWhite < overWhite);
}

TEST_CASE("a local radial mask brightens only inside the oval", "[gpu][localadj]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    ImageBuffer scene;
    scene.width = 64;
    scene.height = 48;
    scene.data.assign(size_t(scene.width) * scene.height * 3, 0.3f);

    GlobalAdjustment p;
    LocalAdjustment la;
    la.mask = RadialMask{
        .center = {0.5, 0.5},
        .radiusX = 0.3,
        .radiusY = 0.3,
        .angle = 0.0,
        .feather = 0.3,
        .invert = false};
    la.exposure = 1.0f;
    p.localAdjustments.push_back(la);

    const QImage got = offscreen::renderToImage(*core, scene, p, scene.width, scene.height);
    REQUIRE_FALSE(got.isNull());

    auto value = [&](float fx, float fy) {
        const float* px = reinterpret_cast<const float*>(got.constScanLine(int(fy * scene.height)));
        return px[int(fx * scene.width) * 4];
    };
    const float inside = value(0.5f, 0.5f);    // centre of the oval, +1 EV
    const float outside = value(0.05f, 0.05f); // corner, well outside

    INFO("inside=" << inside << " outside=" << outside);
    CHECK(inside > outside + 0.1f);
}

// Regression: masks are pinned to the post-lens-correction image, not to the
// current crop. A radial mask centred at full-image (0.5,0.5) must remain on
// that subject when a later crop keeps only the bottom-right quadrant.
TEST_CASE("a radial mask follows the subject through crop changes", "[gpu][localadj]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    ImageBuffer scene;
    scene.width = 64;
    scene.height = 48;
    scene.data.assign(size_t(scene.width) * scene.height * 3, 0.3f);

    GlobalAdjustment p;
    p.cropRect = QRectF(0.5, 0.5, 0.5, 0.5); // bottom-right quadrant of the source
    LocalAdjustment la;
    la.mask = RadialMask{
        .center = {0.5, 0.5}, // centre of the full corrected image
        .radiusX = 0.2,
        .radiusY = 0.2,
        .angle = 0.0,
        .feather = 0.2,
        .invert = false};
    la.exposure = 1.0f;
    p.localAdjustments.push_back(la);

    const int outW = int(std::lround(p.cropRect.width() * scene.width));
    const int outH = int(std::lround(p.cropRect.height() * scene.height));
    const QImage got = offscreen::renderToImage(*core, scene, p, outW, outH);
    REQUIRE_FALSE(got.isNull());

    auto value = [&](float fx, float fy) {
        const float* px = reinterpret_cast<const float*>(got.constScanLine(int(fy * outH)));
        return px[int(fx * outW) * 4];
    };
    const float center = value(0.5f, 0.5f);   // full-image 0.75,0.75: outside the oval
    const float corner = value(0.05f, 0.05f); // near full-image 0.5,0.5: inside the oval

    // Crop-frame masks brighten the crop centre instead. Subject-space masks
    // leave the brightened region attached to the full-image centre.
    INFO("center=" << center << " corner=" << corner);
    CHECK(corner > center + 0.1f);
}

TEST_CASE("post-crop Vignette darkens crop corners without moving its centre", "[gpu][effects]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    ImageBuffer scene;
    scene.width = 80;
    scene.height = 60;
    scene.data.assign(size_t(scene.width) * scene.height * 3, 0.5f);

    GlobalAdjustment p;
    p.cropRect = {0.2, 0.1, 0.6, 0.7};
    p.postCropVignetteAmount = -100.0f;
    p.postCropVignetteMidpoint = 50.0f;
    p.postCropVignetteFeather = 50.0f;
    const int outW = int(p.cropRect.width() * scene.width);
    const int outH = int(p.cropRect.height() * scene.height);

    const QImage got = offscreen::renderToImage(*core, scene, p, outW, outH);
    REQUIRE_FALSE(got.isNull());

    auto value = [&](int x, int y) {
        return reinterpret_cast<const float*>(got.constScanLine(y))[x * 4];
    };
    const float centre = value(got.width() / 2, got.height() / 2);
    const float corner = value(0, 0);
    INFO("centre=" << centre << " corner=" << corner);
    CHECK(centre > 0.45f);
    CHECK(corner < centre * 0.4f);
}

TEST_CASE("Grain is deterministic per seed and monochromatic", "[gpu][effects]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    ImageBuffer scene;
    scene.width = 64;
    scene.height = 48;
    scene.data.assign(size_t(scene.width) * scene.height * 3, 0.4f);

    GlobalAdjustment p;
    p.grainAmount = 60.0f;
    p.grainSize = 50.0f;
    p.grainRoughness = 75.0f;
    p.grainSeed = 123456U;
    const QImage first = offscreen::renderToImage(*core, scene, p, scene.width, scene.height);
    const QImage second = offscreen::renderToImage(*core, scene, p, scene.width, scene.height);
    REQUIRE_FALSE(first.isNull());
    CHECK(first == second);

    const float* px = reinterpret_cast<const float*>(first.constScanLine(17));
    CHECK(std::abs(px[23 * 4 + 0] - px[23 * 4 + 1]) < 1e-6f);
    CHECK(std::abs(px[23 * 4 + 1] - px[23 * 4 + 2]) < 1e-6f);

    p.grainSeed = 654321U;
    const QImage other = offscreen::renderToImage(*core, scene, p, scene.width, scene.height);
    REQUIRE_FALSE(other.isNull());
    CHECK_FALSE(first == other);
}

// Clipping overlay (docs/adr/0009): rendered through the display path (sRGB
// encode) with both overlays on. The synthetic scene spans pure white, near
// black, and saturated single-channel bars, so this one golden locks the
// any-channel rule, the red/blue colours, and highlight-wins-over-shadow ties.
TEST_CASE("clipping overlay matches golden render", "[gpu][golden]") {
    RendererCore* core = goldenCore();
    if (!core)
        SKIP("no OpenGL context available on this machine");

    const ImageBuffer scene = syntheticScene();
    const bool update = qEnvironmentVariableIsSet("ARRAW_UPDATE_GOLDENS");
    const QString goldenDir = QStringLiteral(ARRAW_FIXTURE_DIR "/golden");
    if (update)
        QDir().mkpath(goldenDir);

    GlobalAdjustment p; // neutral: clipping reflects the scene itself
    const QImage got = offscreen::renderClipSample(
        *core,
        scene,
        p,
        /*highlights=*/true,
        /*shadows=*/true);
    REQUIRE_FALSE(got.isNull());
    REQUIRE(got.format() == QImage::Format_RGBX32FPx4);

    const QString path = goldenDir + "/clipping_both.pfm";
    if (update) {
        REQUIRE(writePfm(path, got));
        SUCCEED("golden updated: clipping_both");
        return;
    }

    const QImage want = readPfm(path);
    INFO("golden: " << path.toStdString() << " (regenerate with ARRAW_UPDATE_GOLDENS=1)");
    REQUIRE_FALSE(want.isNull());
    REQUIRE(want.size() == got.size());

    const DiffStats d = diff(got, want);
    INFO(
        "maxDiff=" << d.maxDiff * 255.0f << "/255, meanDiff=(" << d.meanDiff[0] * 255.0f << ", "
                   << d.meanDiff[1] * 255.0f << ", " << d.meanDiff[2] * 255.0f << ")/255");
    CHECK(d.maxDiff <= kMaxPixelDiff);
    for (int c = 0; c < 3; ++c)
        CHECK(std::abs(d.meanDiff[c]) <= kMaxChannelMean);
}

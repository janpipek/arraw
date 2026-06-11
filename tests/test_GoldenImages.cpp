// Golden-image tests for the GLSL pipeline, via the real export path
// (ImageViewport::renderToImage) — policy, thresholds, and format are
// docs/adr/0005. Regenerate goldens with:
//
//   ARRAW_UPDATE_GOLDENS=1 ./build/tests/arraw_tests "[golden]"

#include "ImageViewport.h"
#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <cmath>

namespace {

// ── ADR 0005 thresholds (loose per-pixel + tight mean, any GPU) ──────────────
constexpr float kMaxPixelDiff   = 4.0f / 255.0f;
constexpr float kMaxChannelMean = 0.3f / 255.0f;

// ── Realized viewport (QApplication + shown widget = live GL context) ────────

ImageViewport* goldenViewport() {
    static int argc = 1;
    static char arg0[] = "arraw_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);

    // Declared after `app` so it is destroyed first — tearing down a live
    // QOpenGLWidget after QApplication segfaults in the platform plugin.
    static std::unique_ptr<ImageViewport> vp = [] {
        auto v = std::make_unique<ImageViewport>();
        v->resize(128, 96);
        v->show();
        for (int i = 0; i < 200 && !v->context(); ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return v;
    }();
    return vp->context() ? vp.get() : nullptr;
}

// ── Synthetic input: grey gradient over color bars (see ADR 0005) ────────────

ImageBuffer syntheticScene() {
    constexpr int W = 64, H = 48;
    static const float bars[8][3] = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1},
        {1, 0, 1}, {1, 1, 0}, {1, 1, 1}, {0.02f, 0.02f, 0.02f},
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
                for (int c = 0; c < 3; ++c) p[c] = bar[c] * fade;
            }
        }
    return b;
}

// ── PFM I/O (color "PF", little-endian, rows bottom-up per spec) ──────────────

bool writePfm(const QString& path, const QImage& img) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QStringLiteral("PF\n%1 %2\n-1.0\n")
                .arg(img.width()).arg(img.height()).toLatin1());
    for (int y = img.height() - 1; y >= 0; --y) {
        const float* px = reinterpret_cast<const float*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            f.write(reinterpret_cast<const char*>(px + x * 4), 3 * sizeof(float));
    }
    return f.error() == QFileDevice::NoError;
}

QImage readPfm(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (f.readLine().trimmed() != "PF") return {};
    const QList<QByteArray> dims = f.readLine().trimmed().split(' ');
    if (dims.size() != 2) return {};
    const int w = dims[0].toInt(), h = dims[1].toInt();
    if (f.readLine().trimmed().toFloat() >= 0.0f) return {};  // big-endian unsupported
    QImage img(w, h, QImage::Format_RGBX32FPx4);
    for (int y = h - 1; y >= 0; --y) {
        float* px = reinterpret_cast<float*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            if (f.read(reinterpret_cast<char*>(px + x * 4), 3 * sizeof(float))
                    != 3 * sizeof(float))
                return {};
            px[x * 4 + 3] = 1.0f;
        }
    }
    return img;
}

// ── Comparison: loose per-pixel + tight per-channel mean ─────────────────────

struct DiffStats {
    float maxDiff = 0.0f;
    float meanDiff[3] = {};   // signed, per channel — catches systematic drift
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
    for (int c = 0; c < 3; ++c) s.meanDiff[c] = float(sum[c] / n);
    return s;
}

struct Scenario {
    const char* name;
    AdjustmentParams params;
};

std::vector<Scenario> scenarios() {
    std::vector<Scenario> list;

    list.push_back({"neutral", {}});

    AdjustmentParams p;
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
    p.hslHue[0] = 40.0f;   // red hue shift
    p.hslSat[5] = -50.0f;  // blue desaturation
    p.hslLum[2] = 30.0f;   // yellow luminance
    list.push_back({"color_hsl", p});

    p = {};
    p.curveLuma.points = {{0.0, 0.0}, {0.25, 0.15}, {0.75, 0.85}, {1.0, 1.0}};
    p.curveR.points    = {{0.0, 0.1}, {1.0, 0.9}};
    list.push_back({"tone_curve", p});

    p = {};
    p.cropRect = QRectF(0.25, 0.125, 0.5, 0.5);
    p.rotation = 10.0f;
    list.push_back({"crop_rotate", p});

    return list;
}

}  // namespace

TEST_CASE("shader pipeline matches golden renders", "[gpu][golden]") {
    ImageViewport* vp = goldenViewport();
    if (!vp)
        SKIP("no OpenGL context available on this machine");

    const ImageBuffer scene = syntheticScene();
    const bool update = qEnvironmentVariableIsSet("ARRAW_UPDATE_GOLDENS");
    const QString goldenDir = QStringLiteral(ARRAW_FIXTURE_DIR "/golden");
    if (update)
        QDir().mkpath(goldenDir);

    for (const auto& sc : scenarios()) {
        DYNAMIC_SECTION(sc.name) {
            const int outW = int(std::lround(sc.params.cropRect.width()  * scene.width));
            const int outH = int(std::lround(sc.params.cropRect.height() * scene.height));

            // Same sequence as MainWindow's export: params are applied to the
            // viewport (curve LUT) before the offscreen render.
            vp->setAdjustments(sc.params);
            const QImage got = vp->renderToImage(scene, sc.params, outW, outH);
            REQUIRE_FALSE(got.isNull());
            REQUIRE(got.format() == QImage::Format_RGBX32FPx4);

            const QString path = goldenDir + "/" + sc.name + ".pfm";
            if (update) {
                REQUIRE(writePfm(path, got));
                SUCCEED("golden updated: " << sc.name);
                continue;
            }

            const QImage want = readPfm(path);
            INFO("golden: " << path.toStdString()
                 << " (regenerate with ARRAW_UPDATE_GOLDENS=1)");
            REQUIRE_FALSE(want.isNull());
            REQUIRE(want.size() == got.size());

            const DiffStats d = diff(got, want);
            INFO("maxDiff=" << d.maxDiff * 255.0f << "/255, meanDiff=("
                 << d.meanDiff[0] * 255.0f << ", " << d.meanDiff[1] * 255.0f
                 << ", " << d.meanDiff[2] * 255.0f << ")/255");
            CHECK(d.maxDiff <= kMaxPixelDiff);
            for (int c = 0; c < 3; ++c)
                CHECK(std::abs(d.meanDiff[c]) <= kMaxChannelMean);
        }
    }
}

#include "LensfunSource.h"

#ifndef ARRAW_HAS_LENSFUN

// No lensfun at build time: lens correction degrades to "no profile available".
bool lensfunAvailable() {
    return false;
}

std::optional<LensCorrectionModel> resolveLensfunModel(const QString&, const LensQuery&) {
    return std::nullopt;
}

#else

#include <lensfun.h>

#include <QCoreApplication>
#include <QDir>

#include <cmath>

bool lensfunAvailable() {
    return true;
}

namespace {

constexpr int kLut = RadialCurve::N;

float radius(float x, float y, float cx, float cy) {
    return std::hypot(x - cx, y - cy);
}

// A lens database bundled next to the executable, so distributed builds (AppImage,
// portable Windows tree) resolve lenses without a system lensfun install — the
// AppImage is self-contained and Windows has no system path at all (issue #53).
// Returns the first existing candidate directory that holds DB XML, or an empty
// string to fall back to lensfun's own system/user discovery (dev builds and the
// Fedora RPM, which depends on the system `lensfun` package for the DB).
QString bundledLensfunDbDir() {
    const QString appDir = QCoreApplication::applicationDirPath();
    if (appDir.isEmpty())
        return QString(); // no QCoreApplication (e.g. unit tests) — use system DB
    const QStringList candidates = {
        appDir + QStringLiteral("/../share/lensfun/db"), // AppImage: usr/bin → usr/share
        appDir + QStringLiteral("/lensfun/db"),          // portable tree beside the exe
    };
    for (const QString& candidate : candidates) {
        const QDir dir(candidate);
        if (dir.exists() && !dir.entryList({QStringLiteral("*.xml")}, QDir::Files).isEmpty())
            return dir.absolutePath();
    }
    return QString();
}

} // namespace

std::optional<LensCorrectionModel> resolveLensfunModel(const QString& dbPath, const LensQuery& query) {
    if (query.width <= 0 || query.height <= 0 || query.focal <= 0.0f)
        return std::nullopt;

    lfDatabase db;
    // An explicit dbPath (deterministic tests) wins; otherwise prefer a DB bundled
    // next to the executable, and only then fall back to lensfun's system discovery.
    QString dir = dbPath;
    if (dir.isEmpty())
        dir = bundledLensfunDbDir();
    if (dir.isEmpty()) {
        if (db.Load() != LF_NO_ERROR)
            return std::nullopt; // no system database available
    } else if (!db.LoadDirectory(dir.toUtf8().constData())) {
        return std::nullopt;
    }

    const lfCamera** cams = db.FindCameras(query.cameraMaker.toUtf8().constData(),
                                           query.cameraModel.toUtf8().constData());
    const lfCamera* cam = (cams && cams[0]) ? cams[0] : nullptr;

    const lfLens** lenses = db.FindLenses(cam, nullptr, query.lensModel.toUtf8().constData());
    const lfLens* lens = (lenses && lenses[0]) ? lenses[0] : nullptr;
    if (!lens) {
        lf_free(lenses);
        lf_free(const_cast<lfCamera**>(cams));
        return std::nullopt;
    }

    const int W = query.width;
    const int H = query.height;
    const float cx = static_cast<float>(W) * 0.5f;
    const float cy = static_cast<float>(H) * 0.5f;
    const float aperture = query.aperture > 0.0f ? query.aperture : 8.0f;
    const float distance = query.distance > 0.0f ? query.distance : 1000.0f;
    const float crop = cam && cam->CropFactor > 0.0f ? cam->CropFactor : lens->CropFactor;

    LensCorrectionModel model;
    model.center = {0.5, 0.5};
    model.lensName = QString::fromUtf8(lens->Model);
    model.source = LensCorrectionModel::Source::Lensfun;

    // Sample lensfun's resolved correction along the diagonal toward the far corner,
    // where the output pixel at parameter t (i / (kLut-1)) sits exactly at normalised
    // radius t — matching applyLensCorrection's r = dist/maxR convention.
    const auto outPixel = [&](int i, float& ox, float& oy) {
        const float t = static_cast<float>(i) / static_cast<float>(kLut - 1);
        ox = cx + t * cx;
        oy = cy + t * cy;
    };

    // Distortion → radial scale LUT.
    {
        lfModifier mod(lens, crop, W, H);
        const int eff = mod.Initialize(lens, LF_PF_F32, query.focal, aperture, distance, 1.0f,
                                       lens->Type, LF_MODIFY_DISTORTION, false);
        if (eff & LF_MODIFY_DISTORTION) {
            model.hasDistortion = true;
            for (int i = 0; i < kLut; ++i) {
                float ox = 0.0f, oy = 0.0f;
                outPixel(i, ox, oy);
                float res[2] = {ox, oy};
                const float outd = radius(ox, oy, cx, cy);
                float s = 1.0f;
                if (outd > 0.0f && mod.ApplyGeometryDistortion(ox, oy, 1, 1, res))
                    s = radius(res[0], res[1], cx, cy) / outd;
                model.distortion.lut[i] = s;
            }
        }
    }

    // Lateral TCA → per-channel radial scale LUTs (red/blue relative to green).
    {
        lfModifier mod(lens, crop, W, H);
        const int eff = mod.Initialize(lens, LF_PF_F32, query.focal, aperture, distance, 1.0f,
                                       lens->Type, LF_MODIFY_TCA, false);
        if (eff & LF_MODIFY_TCA) {
            model.hasTCA = true;
            for (int i = 0; i < kLut; ++i) {
                float ox = 0.0f, oy = 0.0f;
                outPixel(i, ox, oy);
                float res[6] = {ox, oy, ox, oy, ox, oy};
                const float outd = radius(ox, oy, cx, cy);
                float sr = 1.0f, sb = 1.0f;
                if (outd > 0.0f && mod.ApplySubpixelDistortion(ox, oy, 1, 1, res)) {
                    sr = radius(res[0], res[1], cx, cy) / outd;
                    sb = radius(res[4], res[5], cx, cy) / outd;
                }
                model.tcaR.lut[i] = sr;
                model.tcaB.lut[i] = sb;
            }
        }
    }

    // Vignetting → radial gain LUT: correcting a unit pixel yields the gain directly.
    {
        lfModifier mod(lens, crop, W, H);
        const int eff = mod.Initialize(lens, LF_PF_F32, query.focal, aperture, distance, 1.0f,
                                       lens->Type, LF_MODIFY_VIGNETTING, false);
        if (eff & LF_MODIFY_VIGNETTING) {
            model.hasVignetting = true;
            for (int i = 0; i < kLut; ++i) {
                float ox = 0.0f, oy = 0.0f;
                outPixel(i, ox, oy);
                float px[3] = {1.0f, 1.0f, 1.0f};
                float g = 1.0f;
                if (mod.ApplyColorModification(px, ox, oy, 1, 1, LF_CR_3(RED, GREEN, BLUE),
                                               3 * static_cast<int>(sizeof(float))))
                    g = px[1];
                model.vignette.lut[i] = g;
            }
        }
    }

    lf_free(lenses);
    lf_free(const_cast<lfCamera**>(cams));

    if (!model.hasDistortion && !model.hasTCA && !model.hasVignetting)
        return std::nullopt;
    return model;
}

#endif

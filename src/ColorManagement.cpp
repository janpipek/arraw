#include "ColorManagement.h"
#include <QColorSpace>
#include <lcms2.h>
#include <memory>

// All lcms handles are created per call rather than cached: profile/transform
// construction is microseconds against a once-per-image conversion, and
// separate handles keep concurrent loads (worker thread + main thread)
// trivially safe without locking.

namespace {

using ProfilePtr   = std::unique_ptr<std::remove_pointer_t<cmsHPROFILE>,   decltype(&cmsCloseProfile)>;
using TransformPtr = std::unique_ptr<std::remove_pointer_t<cmsHTRANSFORM>, decltype(&cmsDeleteTransform)>;

constexpr cmsCIExyY kD65 = {0.3127, 0.3290, 1.0};

ProfilePtr makeProfile(cmsHPROFILE p) { return {p, &cmsCloseProfile}; }

// Linear-light Rec.2020 — the working color space (docs/adr/0001).
ProfilePtr workingProfile() {
    cmsCIExyYTRIPLE primaries = {{0.708, 0.292, 1.0},
                                 {0.170, 0.797, 1.0},
                                 {0.131, 0.046, 1.0}};
    cmsToneCurve* linear = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve* curves[3] = {linear, linear, linear};
    auto p = makeProfile(cmsCreateRGBProfile(&kD65, &primaries, curves));
    cmsFreeToneCurve(linear);
    return p;
}

ProfilePtr outputProfile(OutputProfile profile) {
    switch (profile) {
    case OutputProfile::SRgb:
        return makeProfile(cmsCreate_sRGBProfile());
    case OutputProfile::DisplayP3: {
        cmsCIExyYTRIPLE primaries = {{0.680, 0.320, 1.0},
                                     {0.265, 0.690, 1.0},
                                     {0.150, 0.060, 1.0}};
        // sRGB transfer function (parametric type 4: Y = (aX+b)^g above d, cX below)
        cmsFloat64Number params[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
        cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, params);
        cmsToneCurve* curves[3] = {curve, curve, curve};
        auto p = makeProfile(cmsCreateRGBProfile(&kD65, &primaries, curves));
        cmsFreeToneCurve(curve);
        return p;
    }
    case OutputProfile::AdobeRgb: {
        cmsCIExyYTRIPLE primaries = {{0.640, 0.330, 1.0},
                                     {0.210, 0.710, 1.0},
                                     {0.150, 0.060, 1.0}};
        cmsToneCurve* curve = cmsBuildGamma(nullptr, 563.0 / 256.0);  // 2.19921875
        cmsToneCurve* curves[3] = {curve, curve, curve};
        auto p = makeProfile(cmsCreateRGBProfile(&kD65, &primaries, curves));
        cmsFreeToneCurve(curve);
        return p;
    }
    }
    return makeProfile(cmsCreate_sRGBProfile());
}

QColorSpace outputColorSpace(OutputProfile profile) {
    switch (profile) {
    case OutputProfile::SRgb:      return QColorSpace(QColorSpace::SRgb);
    case OutputProfile::DisplayP3: return QColorSpace(QColorSpace::DisplayP3);
    case OutputProfile::AdobeRgb:  return QColorSpace(QColorSpace::AdobeRgb);
    }
    return QColorSpace(QColorSpace::SRgb);
}

}  // namespace

ImageBuffer toWorkingSpaceBuffer(const QImage& img) {
    if (img.isNull()) return {};

    ProfilePtr src = makeProfile(nullptr);
    if (const QByteArray icc = img.colorSpace().iccProfile(); !icc.isEmpty())
        src = makeProfile(cmsOpenProfileFromMem(icc.constData(), cmsUInt32Number(icc.size())));
    if (!src)
        src = makeProfile(cmsCreate_sRGBProfile());

    const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    const ProfilePtr work = workingProfile();
    const TransformPtr t{cmsCreateTransform(src.get(), TYPE_RGB_8,
                                            work.get(), TYPE_RGB_FLT,
                                            INTENT_RELATIVE_COLORIMETRIC, 0),
                         &cmsDeleteTransform};
    if (!t) return {};

    const int w = rgb.width(), h = rgb.height();
    ImageBuffer buf;
    buf.width  = w;
    buf.height = h;
    buf.data.resize(size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        cmsDoTransform(t.get(), rgb.constScanLine(y), buf.data.data() + size_t(y) * w * 3, w);
    return buf;
}

QImage toOutputImage(const QImage& linearWorking, OutputProfile profile, bool sixteenBit) {
    if (linearWorking.isNull() || linearWorking.format() != QImage::Format_RGBX32FPx4)
        return {};

    const ProfilePtr work = workingProfile();
    const ProfilePtr out  = outputProfile(profile);
    const int w = linearWorking.width(), h = linearWorking.height();

    QImage dst(w, h, sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGB888);
    const TransformPtr t{cmsCreateTransform(work.get(), TYPE_RGBA_FLT,
                                            out.get(), sixteenBit ? TYPE_RGBA_16 : TYPE_RGB_8,
                                            INTENT_RELATIVE_COLORIMETRIC, 0),
                         &cmsDeleteTransform};
    if (!t) return {};

    for (int y = 0; y < h; ++y)
        cmsDoTransform(t.get(), linearWorking.constScanLine(y), dst.scanLine(y), w);

    // lcms skips the extra (alpha) channel in TYPE_RGBA_16 output — force opaque.
    if (sixteenBit) {
        for (int y = 0; y < h; ++y) {
            auto* px = reinterpret_cast<quint16*>(dst.scanLine(y));
            for (int x = 0; x < w; ++x)
                px[x * 4 + 3] = 0xFFFF;
        }
    }

    dst.setColorSpace(outputColorSpace(profile));
    return dst;
}

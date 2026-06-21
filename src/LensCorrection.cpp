#include "LensCorrection.h"

#include "ImagePipeline.h"

#include <algorithm>
#include <cmath>

float RadialCurve::sample(float r) const {
    r = std::clamp(r, 0.0f, 1.0f);
    const float pos = r * (N - 1);
    const int i = static_cast<int>(pos);
    if (i >= N - 1)
        return lut[N - 1];
    const float frac = pos - static_cast<float>(i);
    return lut[i] * (1.0f - frac) + lut[i + 1] * frac;
}

RadialCurve RadialCurve::fromFn(const std::function<float(float)>& fn) {
    RadialCurve c;
    for (int i = 0; i < N; ++i)
        c.lut[i] = fn(static_cast<float>(i) / static_cast<float>(N - 1));
    return c;
}

RadialCurve RadialCurve::identityGain() {
    RadialCurve c;
    c.lut.fill(1.0f);
    return c;
}

// Multiply every pixel by the radial gain curve, with r = 0 at the optical centre
// and r = 1 at the image corner farthest from it (so the curve covers the whole
// frame regardless of where the centre sits).
static void applyVignetting(ImageBuffer& buf, const RadialCurve& gain, QPointF center) {
    const float cx = static_cast<float>(center.x()) * static_cast<float>(buf.width);
    const float cy = static_cast<float>(center.y()) * static_cast<float>(buf.height);
    const auto dist = [&](float x, float y) { return std::hypot(x - cx, y - cy); };
    const float maxR = std::max({dist(0.0f, 0.0f), dist(static_cast<float>(buf.width), 0.0f),
                                 dist(0.0f, static_cast<float>(buf.height)),
                                 dist(static_cast<float>(buf.width), static_cast<float>(buf.height))});
    if (maxR <= 0.0f)
        return;

    for (int y = 0; y < buf.height; ++y) {
        for (int x = 0; x < buf.width; ++x) {
            const float r = dist(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) / maxR;
            const float g = gain.sample(r);
            const size_t i = static_cast<size_t>((y * buf.width + x) * 3);
            buf.data[i] *= g;
            buf.data[i + 1] *= g;
            buf.data[i + 2] *= g;
        }
    }
}

// Bilinear sample of one channel `c` of `src` at pixel position (px, py), where a
// pixel's centre is at integer + 0.5. Out-of-range positions clamp to the edge.
static float sampleChannel(const ImageBuffer& src, float px, float py, int c) {
    const float fx = px - 0.5f;
    const float fy = py - 0.5f;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = std::clamp(x0, 0, src.width - 1);
    x1 = std::clamp(x1, 0, src.width - 1);
    y0 = std::clamp(y0, 0, src.height - 1);
    y1 = std::clamp(y1, 0, src.height - 1);

    const auto at = [&](int x, int y) {
        return src.data[static_cast<size_t>((y * src.width + x) * 3 + c)];
    };
    const auto lerp = [](float a, float b, float t) { return a * (1.0f - t) + b * t; };
    const float top = lerp(at(x0, y0), at(x1, y0), tx);
    const float bot = lerp(at(x0, y1), at(x1, y1), tx);
    return lerp(top, bot, ty);
}

// The radial frame for a warp: optical centre in pixels and the distance to the
// farthest corner (the normalisation for r). Shared by the warp and the fill-zoom so
// the geometry is defined once.
struct WarpFrame {
    float cx = 0.0f;
    float cy = 0.0f;
    float maxR = 0.0f;
};

static WarpFrame warpFrame(const LensCorrectionModel& model, int width, int height) {
    WarpFrame f;
    f.cx = static_cast<float>(model.center.x()) * static_cast<float>(width);
    f.cy = static_cast<float>(model.center.y()) * static_cast<float>(height);
    const auto dist = [&](float x, float y) { return std::hypot(x - f.cx, y - f.cy); };
    f.maxR = std::max({dist(0.0f, 0.0f), dist(static_cast<float>(width), 0.0f),
                       dist(0.0f, static_cast<float>(height)),
                       dist(static_cast<float>(width), static_cast<float>(height))});
    return f;
}

// Source position a corrected output pixel at vector (vx, vy) from the centre samples
// from: the vector is divided by the fill zoom, then scaled by the distortion curve.
static void warpSource(const WarpFrame& f, const RadialCurve& distortion, bool doDistortion,
                       float zoom, float vx, float vy, float& sx, float& sy, float& scale) {
    const float zx = vx / zoom;
    const float zy = vy / zoom;
    const float r = std::hypot(zx, zy) / f.maxR;
    scale = doDistortion ? distortion.sample(r) : 1.0f;
    sx = f.cx + zx * scale;
    sy = f.cy + zy * scale;
}

// Geometric warp: for each output pixel, scale its vector from the optical centre and
// resample the source there. Green carries the distortion scale; red and blue add
// the per-channel TCA scale on top, so distortion and lateral CA share one resample.
static ImageBuffer applyWarp(const ImageBuffer& src, const LensCorrectionModel& model,
                             bool doDistortion, bool doTCA, float zoom) {
    const WarpFrame f = warpFrame(model, src.width, src.height);
    ImageBuffer dst = src;
    if (f.maxR <= 0.0f)
        return dst;

    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            const float vx = static_cast<float>(x) + 0.5f - f.cx;
            const float vy = static_cast<float>(y) + 0.5f - f.cy;
            float gx = 0.0f, gy = 0.0f, base = 1.0f;
            warpSource(f, model.distortion, doDistortion, zoom, vx, vy, gx, gy, base);
            const size_t i = static_cast<size_t>((y * src.width + x) * 3);
            dst.data[i + 1] = sampleChannel(src, gx, gy, 1); // green carries geometry
            if (doTCA) {
                const float zx = vx / zoom;
                const float zy = vy / zoom;
                const float r = std::hypot(zx, zy) / f.maxR;
                const float sR = base * model.tcaR.sample(r);
                const float sB = base * model.tcaB.sample(r);
                dst.data[i] = sampleChannel(src, f.cx + zx * sR, f.cy + zy * sR, 0);
                dst.data[i + 2] = sampleChannel(src, f.cx + zx * sB, f.cy + zy * sB, 2);
            } else {
                dst.data[i] = sampleChannel(src, gx, gy, 0);
                dst.data[i + 2] = sampleChannel(src, gx, gy, 2);
            }
        }
    }
    return dst;
}

float autoFillZoom(const LensCorrectionModel& model, int width, int height) {
    if (!model.hasDistortion || width <= 0 || height <= 0)
        return 1.0f;
    const WarpFrame f = warpFrame(model, width, height);
    if (f.maxR <= 0.0f)
        return 1.0f;

    // A zoom is valid when every boundary output pixel samples within the source
    // (interior pixels never sample farther out than the boundary for a monotone
    // radial scale). Larger zoom pulls samples inward, so validity is monotone —
    // binary-search the smallest valid zoom.
    const auto edgeOk = [&](float vx, float vy, float zoom) {
        float sx = 0.0f, sy = 0.0f, scale = 0.0f;
        warpSource(f, model.distortion, true, zoom, vx, vy, sx, sy, scale);
        return sx >= 0.0f && sx <= static_cast<float>(width) && sy >= 0.0f
               && sy <= static_cast<float>(height);
    };
    const auto valid = [&](float zoom) {
        for (int x = 0; x < width; ++x) {
            const float vx = static_cast<float>(x) + 0.5f - f.cx;
            if (!edgeOk(vx, 0.5f - f.cy, zoom)
                || !edgeOk(vx, static_cast<float>(height) - 0.5f - f.cy, zoom))
                return false;
        }
        for (int y = 0; y < height; ++y) {
            const float vy = static_cast<float>(y) + 0.5f - f.cy;
            if (!edgeOk(0.5f - f.cx, vy, zoom)
                || !edgeOk(static_cast<float>(width) - 0.5f - f.cx, vy, zoom))
                return false;
        }
        return true;
    };

    if (valid(1.0f))
        return 1.0f;
    float lo = 1.0f;
    float hi = 16.0f;
    if (!valid(hi))
        return hi; // extreme distortion: clamp rather than loop forever
    for (int iter = 0; iter < 24; ++iter) {
        const float mid = 0.5f * (lo + hi);
        if (valid(mid))
            hi = mid;
        else
            lo = mid;
    }
    return hi;
}

ImageBuffer applyLensCorrection(const ImageBuffer& buf, const LensCorrectionModel& model,
                                const LensCorrectionToggles& toggles) {
    ImageBuffer out = buf;
    if (!out.valid())
        return out;

    // Vignetting is a per-pixel gain in the captured (source) geometry, applied
    // before the geometric warp resamples the de-vignetted image.
    if (model.hasVignetting && toggles.vignetting)
        applyVignetting(out, model.vignette, model.center);

    // Distortion and lateral TCA are both radial scales, resampled together: the warp
    // runs when either is enabled (green carries distortion, red/blue add TCA).
    const bool doDistortion = model.hasDistortion && toggles.distortion;
    const bool doTCA = model.hasTCA && toggles.ca;
    if (doDistortion || doTCA) {
        const float zoom = doDistortion ? autoFillZoom(model, out.width, out.height) : 1.0f;
        out = applyWarp(out, model, doDistortion, doTCA, zoom);
    }

    return out;
}

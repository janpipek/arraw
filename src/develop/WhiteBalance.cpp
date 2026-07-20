#include "develop/WhiteBalance.h"
#include <algorithm>
#include <cmath>

// ── White balance (docs/adr/0025) ───────────────────────────────────────────
// A per-channel multiplicative gain in linear Rec.2020, so a black pixel
// (c == 0) stays black for any setting. The gain is blackbody-derived: a target
// Kelvin maps through its Planckian-locus chromaticity to a linear Rec.2020
// colour, and the gain is the reference (5500 K) colour over the target's,
// normalised so green == 1 (temperature moves R/B about the green anchor).
// Tint is the orthogonal green↔magenta axis, applied as a green gain.
namespace {

// Correlated-colour-temperature → CIE 1931 xy on the Planckian locus
// (Kim et al. cubic-spline approximation, valid ~1667..25000 K).
std::array<double, 2> planckianXY(float kelvin) {
    const double T = std::clamp(double(kelvin), 1667.0, 25000.0);
    const double T2 = T * T;
    const double T3 = T2 * T;

    const double x = (T < 4000.0)
                         ? -0.2661239e9 / T3 - 0.2343589e6 / T2 + 0.8776956e3 / T + 0.179910
                         : -3.0258469e9 / T3 + 2.1070379e6 / T2 + 0.2226347e3 / T + 0.240390;

    const double x2 = x * x;
    const double x3 = x2 * x;
    double y;
    if (T < 2222.0)
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    else if (T < 4000.0)
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;

    return {x, y};
}

// CIE xy (luminance Y = 1) → linear Rec.2020 RGB (XYZ→Rec.2020, D65).
std::array<double, 3> xyToLinearRec2020(double x, double y) {
    const double Y = 1.0;
    const double X = (y > 1e-9) ? x * Y / y : 0.0;
    const double Z = (y > 1e-9) ? (1.0 - x - y) * Y / y : 0.0;
    return {
        1.7166511880 * X - 0.3556707838 * Y - 0.2533662814 * Z,
        -0.6666843518 * X + 1.6164812366 * Y + 0.0157685458 * Z,
        0.0176398574 * X - 0.0427706133 * Y + 0.9421031212 * Z,
    };
}

// Tint strength: full ±100 slider is a ±2^(0.2) ≈ ±15% green gain.
constexpr double kTintGainExp = 0.2;

std::array<double, 3> illuminantRec2020(float kelvin) {
    const auto [x, y] = planckianXY(kelvin);
    return xyToLinearRec2020(x, y);
}

} // namespace

std::array<float, 3> WhiteBalance::gain() const {
    static const std::array<double, 3> ref = illuminantRec2020(kNeutralKelvin);
    const std::array<double, 3> w = illuminantRec2020(kelvin);

    // von Kries gain = reference illuminant / target illuminant, per channel.
    auto safe = [](double v) { return std::max(v, 1e-4); };
    std::array<double, 3> g = {
        safe(ref[0]) / safe(w[0]),
        safe(ref[1]) / safe(w[1]),
        safe(ref[2]) / safe(w[2]),
    };
    // Normalise so green == 1: temperature moves R/B about the green anchor.
    const double gn = g[1];
    g[0] /= gn;
    g[1] = 1.0;
    g[2] /= gn;

    // Tint: orthogonal green↔magenta gain (+ = greener, matching the prior model).
    g[1] *= std::pow(2.0, kTintGainExp * double(tint) / 100.0);

    return {float(g[0]), float(g[1]), float(g[2])};
}

WhiteBalance WhiteBalance::fromNeutral(double r, double g, double b) {
    // Find the Kelvin whose gain neutralises the R/B balance (r*kr == b*kb), i.e.
    // kr(K)/kb(K) == b/r. The ratio is monotonic in K — bisect over the range.
    const double wantRatio = (r > 1e-6) ? b / r : 1.0;
    double lo = 2000.0, hi = 12000.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        const auto gm = WhiteBalance{float(mid), 0.0f}.gain();
        const double ratio = double(gm[0]) / double(gm[2]); // kr/kb
        if (ratio < wantRatio)
            lo = mid;
        else
            hi = mid;
    }
    const float kelvin = float(std::clamp(0.5 * (lo + hi), 2000.0, 12000.0));

    // Tint brings green onto the (now equal) R/B level: g * tintGain == r*kr.
    const auto gk = WhiteBalance{kelvin, 0.0f}.gain();
    const double m = 0.5 * (r * gk[0] + b * gk[2]); // balanced R/B level
    const double tintGain = (g > 1e-6) ? m / g : 1.0;
    const float tint = float(
        std::clamp(100.0 * std::log2(std::max(tintGain, 1e-6)) / kTintGainExp, -100.0, 100.0));

    return {kelvin, tint};
}

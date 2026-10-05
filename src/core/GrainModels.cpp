#include "GrainModels.h"

#include "ColorAdjustments.h"
#include "Effects.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace arraw;

namespace {

/// @brief Lattice cells per grain of the value-noise model's three lattices, finest first
/// (`main`'s).
constexpr std::array<double, valueNoiseLatticeCount> valueNoiseScales{1.0, 0.53, 0.23};

/// @brief What each lattice's seed is the plan's seed exclusive-ored with: `main`'s three, then
/// the substitute's.
constexpr std::array<std::uint32_t, grainLayerCount> valueNoiseSalts{0U, 0x9e3779b9U, 0x85ebca6bU,
                                                                     0xc2b2ae35U};

/// @brief Shares of the three lattices at full roughness, before scaling to unit variance
/// (`main`'s).
constexpr std::array<double, valueNoiseLatticeCount> roughShares{0.6, 0.3, 0.1};

/// @brief Share of a lattice's variance smoothstep interpolation keeps, per axis: the mean of
/// `s^2 + (1 - s)^2` over a cell.
constexpr double interpolatedShare = 26.0 / 35.0;

/// @brief What turns one lattice's interpolated values into unit standard deviation.
///
/// The lattice values are uniform on (-0.5, 0.5), a variance of 1/12; smoothstep
/// interpolation keeps on average `26/35` of it per axis (the mean of
/// `s^2 + (1 - s)^2` over the cell), so the field's deviation is
/// `(26/35) / sqrt(12)`. `main` used 4.9, about five per cent more.
const double valueNoiseUnitScale = std::sqrt(12.0) / interpolatedShare;

/// @brief Eases from zero at @p first to one at @p last, in double, for the host's placement.
double smoothstepDouble(double first, double last, double value) {
    const double t = std::clamp((value - first) / (last - first), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// @brief Integrates a lattice's interpolation kernel, the smoothstep hat `1 - s(|t|)`, from zero
/// to @p offset cells.
double hatIntegral(double offset) {
    const double a = std::min(std::abs(offset), 1.0);
    const double value = a - a * a * a + 0.5 * a * a * a * a;
    return offset < 0.0 ? -value : value;
}

/// @brief Gives the share of a lattice's variance, per axis, left after a box filter @p width cells
/// wide, relative to the values' own.
///
/// A lattice is its values convolved with the smoothstep hat; boxed, with the
/// hat convolved with the box. What remains is the integral of that kernel's
/// square: ::interpolatedShare for no box, about `1 / width` for a wide one.
/// Integrated by Simpson's rule over the kernel's support; it is piecewise
/// polynomial and smooth, so this is good to far better than the statistics
/// need.
double boxedShare(double width) {
    if (width < 1e-6) {
        return interpolatedShare;
    }
    const double half = 0.5 * width;
    const double end = 1.0 + half;
    constexpr int intervals = 512;
    const double h = end / intervals;
    double sum = 0.0;
    for (int index = 0; index <= intervals; ++index) {
        const double t = index * h;
        const double kernel = (hatIntegral(t + half) - hatIntegral(t - half)) / width;
        const double simpson =
            index == 0 || index == intervals ? 1.0 : (index % 2 != 0 ? 4.0 : 2.0);
        sum += simpson * kernel * kernel;
    }
    // The kernel is even: twice its half.
    return 2.0 * sum * h / 3.0;
}

/// @brief Gives the share of a lattice's values' variance that pixels sampling it at phases @p
/// phase and @p phase + 1/2 of a cell find, per axis: the substitute's two phases.
double twoPhaseShare(double phase) {
    const auto share = [](double t) {
        const double s = t * t * (3.0 - 2.0 * t);
        return s * s + (1.0 - s) * (1.0 - s);
    };
    const double other = phase + 0.5;
    return 0.5 * (share(phase) + share(other - std::floor(other)));
}

/// @brief A lattice value from -0.5 to 0.5, symmetric about zero, exact in float.
///
/// The top 24 bits of the hash, centred, plus a half so that the values
/// pair up around zero; every step is exact, so both backends agree.
float latticeValue(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    const auto centred = static_cast<std::int32_t>(grainHash(x, y, seed) >> 8U) - 0x800000;
    return (static_cast<float>(centred) + 0.5F) * 0x1p-24F;
}

/// @brief Value of one lattice at an output pixel.
///
/// Every operation is a single correctly rounded float operation, in the
/// order the shader uses; nothing may be fused.
float layerValue(const GrainLayer& layer, std::uint32_t column, std::uint32_t row) {
    const float px = layer.fraction[0] + static_cast<float>(column) * layer.delta[0];
    const float py = layer.fraction[1] + static_cast<float>(row) * layer.delta[1];
    const float fx = std::floor(px);
    const float fy = std::floor(py);
    const std::uint32_t cx = static_cast<std::uint32_t>(layer.cell[0]) +
                             static_cast<std::uint32_t>(static_cast<std::int32_t>(fx));
    const std::uint32_t cy = static_cast<std::uint32_t>(layer.cell[1]) +
                             static_cast<std::uint32_t>(static_cast<std::int32_t>(fy));
    const float tx = px - fx;
    const float ty = py - fy;
    const float sx = tx * tx * (3.0F - 2.0F * tx);
    const float sy = ty * ty * (3.0F - 2.0F * ty);
    const float a = latticeValue(cx, cy, layer.seed);
    const float b = latticeValue(cx + 1U, cy, layer.seed);
    const float c = latticeValue(cx, cy + 1U, layer.seed);
    const float d = latticeValue(cx + 1U, cy + 1U, layer.seed);
    const float top = a + (b - a) * sx;
    const float bottom = c + (d - c) * sx;
    return top + (bottom - top) * sy;
}

} // namespace

GrainPlan arraw::grainPlanFor(const GrainSettings& settings) {
    const float amount =
        clampedSetting(settings.amount, minimumGrainControl, maximumGrainControl, "grain amount");
    const float size =
        clampedSetting(settings.size, minimumGrainControl, maximumGrainControl, "grain size");
    const float roughness = clampedSetting(settings.roughness, minimumGrainControl,
                                           maximumGrainControl, "grain roughness");
    if (std::ranges::none_of(grainModelNames,
                             [&](const auto& entry) { return entry.first == settings.model; })) {
        throw std::invalid_argument("Unknown grain model");
    }
    if (amount == 0.0F) {
        return {};
    }
    GrainPlan plan;
    plan.active = true;
    plan.model = settings.model;
    plan.deviation = amount / maximumGrainControl * strongestGrainDeviation;
    const double t = static_cast<double>(size) / maximumGrainControl;
    plan.size = static_cast<float>(
        std::exp2(std::lerp(std::log2(finestGrain), std::log2(coarsestGrain), t)));
    plan.roughness = roughness / maximumGrainControl;
    plan.seed = settings.seed != 0 ? settings.seed : unseededGrainSeed;
    return plan;
}

GrainPlacement arraw::grainPlacementOf(const GrainPlan& plan, const FrameMapping& mapping) {
    switch (plan.model) {
    case GrainModel::ValueNoise:
        return valueNoisePlacement(plan, mapping);
    }
    return {};
}

float arraw::grainAt(const GrainPlan& plan, const GrainPlacement& placement, std::uint32_t column,
                     std::uint32_t row) {
    switch (plan.model) {
    case GrainModel::ValueNoise:
        return valueNoiseGrain(placement, column, row);
    }
    return 0.0F;
}

GrainPlacement arraw::valueNoisePlacement(const GrainPlan& plan, const FrameMapping& mapping) {
    // The frame's width and height in long edges, so that a grain is the same
    // size along both axes and relative to the long edge.
    const std::array<double, 2> extent = mapping.aspect >= 1.0
                                             ? std::array{1.0, 1.0 / mapping.aspect}
                                             : std::array{mapping.aspect, 1.0};
    const double r = plan.roughness;
    const double roughScale =
        1.0 / std::sqrt(roughShares[0] * roughShares[0] + roughShares[1] * roughShares[1] +
                        roughShares[2] * roughShares[2]);
    std::array<double, valueNoiseLatticeCount> shares{};
    for (std::size_t layer = 0; layer < valueNoiseLatticeCount; ++layer) {
        shares[layer] = r * roughShares[layer] * roughScale;
    }
    shares[0] += 1.0 - r;
    const double total =
        std::sqrt(shares[0] * shares[0] + shares[1] * shares[1] + shares[2] * shares[2]);

    GrainPlacement placement;
    // Variance the fade leaves out that a box filter of the output pixel would keep.
    double lost = 0.0;
    for (std::size_t index = 0; index < valueNoiseLatticeCount; ++index) {
        GrainLayer& layer = placement.layers[index];
        double widest = 0.0;
        double boxed = 1.0;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const double cellsPerFrame = extent[axis] / plan.size * valueNoiseScales[index];
            const double first = (mapping.origin[axis] + 0.5 * mapping.step[axis]) * cellsPerFrame;
            const double delta = mapping.step[axis] * cellsPerFrame;
            const double cell = std::floor(first);
            layer.cell[axis] = static_cast<std::int32_t>(cell);
            layer.fraction[axis] = static_cast<float>(first - cell);
            layer.delta[axis] = static_cast<float>(delta);
            widest = std::max(widest, delta);
            boxed *= boxedShare(delta) / interpolatedShare;
        }
        const double fade = smoothstepDouble(grainFadeStart, grainFadeEnd, 1.0 / widest);
        const double part = plan.deviation * shares[index] / total;
        layer.weight = static_cast<float>(part * valueNoiseUnitScale * fade);
        layer.seed = plan.seed ^ valueNoiseSalts[index];
        lost += std::max(0.0, boxed - fade * fade) * part * part;
    }

    // The substitute: cells of two output pixels on the crop frame, so that
    // every pixel samples its cell at one of two phases a half apart, and
    // never aliases. Its weight gives it the lost variance at those phases.
    GrainLayer& substitute = placement.layers[valueNoiseSubstituteLayer];
    substitute.seed = plan.seed ^ valueNoiseSalts[valueNoiseSubstituteLayer];
    double sampled = 1.0 / 12.0;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double cellsPerFrame = 1.0 / (grainSubstituteCell * mapping.step[axis]);
        const double first = (mapping.origin[axis] + 0.5 * mapping.step[axis]) * cellsPerFrame;
        const double cell = std::floor(first);
        substitute.cell[axis] = static_cast<std::int32_t>(cell);
        substitute.fraction[axis] = static_cast<float>(first - cell);
        substitute.delta[axis] = static_cast<float>(1.0 / grainSubstituteCell);
        sampled *= twoPhaseShare(substitute.fraction[axis]);
    }
    substitute.weight = lost > 0.0 ? static_cast<float>(std::sqrt(lost / sampled)) : 0.0F;
    return placement;
}

float arraw::valueNoiseGrain(const GrainPlacement& placement, std::uint32_t column,
                             std::uint32_t row) {
    float grain = 0.0F;
    for (const GrainLayer& layer : placement.layers) {
        if (layer.weight != 0.0F) {
            grain = grain + layer.weight * layerValue(layer, column, row);
        }
    }
    return grain;
}

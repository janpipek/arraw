#include "ResampleWeights.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace arraw;

namespace {

/// @brief Evaluates a kernel at a distance in kernel units.
double kernel(ResizeFilter filter, double x) {
    const double distance = std::abs(x);
    if (filter == ResizeFilter::Bilinear) {
        return distance < 1.0 ? 1.0 - distance : 0.0;
    }
    if (distance >= 3.0) {
        return 0.0;
    }
    if (distance < 1e-12) {
        return 1.0;
    }
    const double pix = std::numbers::pi * distance;
    return 3.0 * std::sin(pix) * std::sin(pix / 3.0) / (pix * pix);
}

} // namespace

AxisWeights arraw::axisWeights(std::uint32_t in, std::uint32_t out, ResizeFilter filter) {
    const double scale = static_cast<double>(out) / in;
    const double stretch = std::max(1.0, 1.0 / scale);
    const double radius = (filter == ResizeFilter::Bilinear ? 1.0 : 3.0) * stretch;

    AxisWeights result;
    result.taps.reserve(out);
    if (in == out) {
        /// Lanczos at scale 1 is only nearly an identity, so make it exact.
        result.weights.assign(out, 1.0);
        for (std::uint32_t x = 0; x < out; ++x) {
            result.taps.push_back({x, x, 1});
        }
        return result;
    }
    for (std::uint32_t x = 0; x < out; ++x) {
        const double centre = (x + 0.5) / scale - 0.5;
        const auto first = static_cast<std::int64_t>(std::ceil(centre - radius));
        const auto last = static_cast<std::int64_t>(std::floor(centre + radius));
        const std::size_t offset = result.weights.size();
        double sum = 0.0;
        for (std::int64_t i = first; i <= last; ++i) {
            const double weight = kernel(filter, (static_cast<double>(i) - centre) / stretch);
            result.weights.push_back(weight);
            sum += weight;
        }
        for (std::size_t k = offset; k < result.weights.size(); ++k) {
            result.weights[k] /= sum;
        }
        result.taps.push_back({first, offset, result.weights.size() - offset});
    }
    return result;
}

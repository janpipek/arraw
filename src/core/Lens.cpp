#include "Lens.h"

#include "ProgressScope.h"
#include "RowBands.h"
#include "TimingTrace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>

using namespace arraw;
using arraw::detail::LensResampler;

namespace {

/// @brief Weights of a kernel's taps at a fractional offset, summing to 1.
///
/// The taps lie at whole pixels from `floor(position) - Taps / 2 + 1` up, and @p fraction is the
/// position's distance past the first of the two between which it falls.
template <LensResampler Which> struct Kernel;

template <> struct Kernel<LensResampler::Bilinear> {
    static constexpr int taps = 2;

    static void weights(double t, double* w) noexcept {
        w[0] = 1.0 - t;
        w[1] = t;
    }
};

template <> struct Kernel<LensResampler::CatmullRom> {
    static constexpr int taps = 4;

    static void weights(double t, double* w) noexcept {
        const double t2 = t * t;
        const double t3 = t2 * t;
        w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
        w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
        w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
        w[3] = 0.5 * (t3 - t2);
    }
};

template <> struct Kernel<LensResampler::Lanczos3> {
    static constexpr int taps = 6;

    static double lanczos(double x) noexcept {
        if (x == 0.0) {
            return 1.0;
        }
        if (std::abs(x) >= 3.0) {
            return 0.0;
        }
        const double a = std::numbers::pi * x;
        return 3.0 * std::sin(a) * std::sin(a / 3.0) / (a * a);
    }

    static void weights(double t, double* w) noexcept {
        double sum = 0.0;
        for (int tap = 0; tap < taps; ++tap) {
            w[tap] = lanczos(t - (tap - 2));
            sum += w[tap];
        }
        for (int tap = 0; tap < taps; ++tap) {
            w[tap] /= sum;
        }
    }
};

/// @brief Snaps a position that rounding left a hair from a whole pixel onto it.
///
/// So that a position that maps to itself reads its pixel exactly, however the arithmetic that
/// found it rounded.
double snapped(double position) noexcept {
    const double whole = std::nearbyint(position);
    return std::abs(position - whole) < 1.0e-9 ? whole : position;
}

/// @brief Where a channel is read along one axis: its taps' pixels, clamped, and their weights.
template <LensResampler K> struct AxisTaps {
    std::array<int, Kernel<K>::taps> index{};
    std::array<double, Kernel<K>::taps> weight{};
    bool whole = false; ///< Whether the position falls on a pixel, so that the pixel is the answer.
    int pixel = 0;      ///< That pixel, when whole.

    void set(double position, int size) noexcept {
        position = snapped(position);
        const double lower = std::floor(position);
        const int base = static_cast<int>(lower);
        whole = position == lower;
        pixel = std::clamp(base, 0, size - 1);
        if (whole) {
            return;
        }
        Kernel<K>::weights(position - lower, weight.data());
        for (int tap = 0; tap < Kernel<K>::taps; ++tap) {
            index[tap] = std::clamp(base - (Kernel<K>::taps / 2 - 1) + tap, 0, size - 1);
        }
    }
};

/// @brief Everything the pass reads, resolved once for the whole image.
struct Pass {
    const LensCorrection& correction;
    const float* source;
    float* target;
    int width;
    int height;
    std::size_t stride; ///< Floats per row.

    double aspect;
    double fill;
    double centerX; ///< Optical centre, in heights.
    double centerY;
    double reach; ///< Centre to the farthest corner, in heights.

    bool distorts;
    bool colorSplit;
    bool dims;
};

/// @brief Reads some channels at the taps, clamping the ringing of a kernel with negative lobes.
///
/// @tparam Count Number of channels read.
/// @param first Index of the first channel; the channels are consecutive.
/// @param out Receives the @p Count values.
template <LensResampler K, int Count>
void read(const Pass& pass, const AxisTaps<K>& across, const AxisTaps<K>& down, int first,
          float* out) noexcept {
    if (across.whole && down.whole) {
        const float* pixel = pass.source + down.pixel * pass.stride +
                             static_cast<std::size_t>(across.pixel) * 4 +
                             static_cast<std::size_t>(first);
        for (int channel = 0; channel < Count; ++channel) {
            out[channel] = pixel[channel];
        }
        return;
    }
    std::array<double, Count> total{};
    std::array<bool, Count> negative{};
    // A whole position on one axis reads its line, or its column, alone.
    const int rows = down.whole ? 1 : Kernel<K>::taps;
    const int columns = across.whole ? 1 : Kernel<K>::taps;
    for (int row = 0; row < rows; ++row) {
        const int line = down.whole ? down.pixel : down.index[row];
        const double rowWeight = down.whole ? 1.0 : down.weight[row];
        const float* sourceRow = pass.source + line * pass.stride + first;
        std::array<double, Count> sum{};
        for (int tap = 0; tap < columns; ++tap) {
            const int column = across.whole ? across.pixel : across.index[tap];
            const double weight = across.whole ? 1.0 : across.weight[tap];
            const float* pixel = sourceRow + static_cast<std::size_t>(column) * 4;
            for (int channel = 0; channel < Count; ++channel) {
                negative[channel] = negative[channel] || pixel[channel] < 0.0F;
                sum[channel] += weight * pixel[channel];
            }
        }
        for (int channel = 0; channel < Count; ++channel) {
            total[channel] += rowWeight * sum[channel];
        }
    }
    // Ringing below zero is the kernel's, not the picture's: a channel that saw nothing
    // negative stays non-negative, while a camera matrix's genuine negatives survive.
    for (int channel = 0; channel < Count; ++channel) {
        out[channel] =
            static_cast<float>(total[channel] < 0.0 && !negative[channel] ? 0.0 : total[channel]);
    }
}

template <LensResampler K> void band(const Pass& pass, std::uint32_t first, std::uint32_t last) {
    const LensCorrection& c = pass.correction;
    AxisTaps<K> greenAcross;
    AxisTaps<K> greenDown;
    AxisTaps<K> redAcross;
    AxisTaps<K> redDown;
    AxisTaps<K> blueAcross;
    AxisTaps<K> blueDown;
    for (std::uint32_t row = first; row < last; ++row) {
        float* out = pass.target + row * pass.stride;
        const double v = (row + 0.5) / pass.height;
        for (int column = 0; column < pass.width; ++column, out += 4) {
            const double u = (column + 0.5) / pass.width;
            // The sensor position, in heights; the corrected one without a distortion.
            double x = u * pass.aspect;
            double y = v;
            if (pass.distorts) {
                const double idealX = pass.centerX + (x - pass.centerX) / pass.fill;
                const double idealY = pass.centerY + (y - pass.centerY) / pass.fill;
                const double radius = std::sqrt((idealX - pass.centerX) * (idealX - pass.centerX) +
                                                (idealY - pass.centerY) * (idealY - pass.centerY)) /
                                      pass.reach;
                const double scale = tableValue(c.distortion, radius);
                x = pass.centerX + (idealX - pass.centerX) * scale;
                y = pass.centerY + (idealY - pass.centerY) * scale;
            }
            const double sensorRadius = pass.colorSplit || pass.dims
                                            ? std::sqrt((x - pass.centerX) * (x - pass.centerX) +
                                                        (y - pass.centerY) * (y - pass.centerY)) /
                                                  pass.reach
                                            : 0.0;
            // Pixel-centre index space: a position of u in 0 to 1 is at u * size - 0.5.
            const auto indexX = [&](double heights) {
                return heights / pass.aspect * pass.width - 0.5;
            };
            const auto indexY = [&](double heights) { return heights * pass.height - 0.5; };

            greenAcross.set(indexX(x), pass.width);
            greenDown.set(indexY(y), pass.height);
            float red;
            float green;
            float blue;
            float alpha;
            if (pass.colorSplit) {
                const double redScale = tableValue(c.redScale, sensorRadius);
                const double blueScale = tableValue(c.blueScale, sensorRadius);
                redAcross.set(indexX(pass.centerX + (x - pass.centerX) * redScale), pass.width);
                redDown.set(indexY(pass.centerY + (y - pass.centerY) * redScale), pass.height);
                blueAcross.set(indexX(pass.centerX + (x - pass.centerX) * blueScale), pass.width);
                blueDown.set(indexY(pass.centerY + (y - pass.centerY) * blueScale), pass.height);
                read<K, 1>(pass, redAcross, redDown, 0, &red);
                read<K, 1>(pass, blueAcross, blueDown, 2, &blue);
                read<K, 1>(pass, greenAcross, greenDown, 1, &green);
                read<K, 1>(pass, greenAcross, greenDown, 3, &alpha);
            } else {
                float all[4];
                read<K, 4>(pass, greenAcross, greenDown, 0, all);
                red = all[0];
                green = all[1];
                blue = all[2];
                alpha = all[3];
            }
            if (pass.dims) {
                const auto gain = static_cast<float>(tableValue(c.vignetting, sensorRadius));
                red *= gain;
                green *= gain;
                blue *= gain;
            }
            out[0] = red;
            out[1] = green;
            out[2] = blue;
            out[3] = alpha;
        }
    }
}

template <LensResampler K> void run(const Pass& pass) {
    detail::forEachRowBand(
        static_cast<std::uint32_t>(pass.height), static_cast<std::uint32_t>(pass.width),
        [&pass](std::uint32_t first, std::uint32_t last) { band<K>(pass, first, last); });
}

} // namespace

ImageBuffer arraw::detail::applyLens(const ImageBuffer& source, const LensPlan& plan,
                                     LensResampler resampler) {
    if (source.format() != workingFormat) {
        throw std::invalid_argument("the lens pass needs pixels in the working format");
    }
    if (!plan.active() || source.size().empty()) {
        return source.clone();
    }
    const detail::TimingSpan timing("cpu.lens");
    // Opened before the result is made, whose zeroing is part of the pass.
    const detail::ProgressSpan progress(ProgressStep::Lens);
    const LensCorrection& c = plan.correction;
    ImageBuffer target(source.size(), workingFormat, source.encoding(), source.orientation());
    target.setPixelScale(source.pixelScale());

    const double aspect = c.aspect > 0.0 ? c.aspect : 1.0;
    Pass pass{c,
              source.samples<float>().data(),
              target.samples<float>().data(),
              static_cast<int>(source.size().width),
              static_cast<int>(source.size().height),
              source.rowStride() / sizeof(float),
              aspect,
              c.fill > 0.0 ? c.fill : 1.0,
              c.centerU * aspect,
              c.centerV,
              0.0,
              !c.distortion.empty(),
              c.hasLateralCa(),
              !c.vignetting.empty()};
    pass.reach = std::hypot(std::max(pass.centerX, aspect - pass.centerX),
                            std::max(pass.centerY, 1.0 - pass.centerY));
    switch (resampler) {
    case LensResampler::Bilinear:
        run<LensResampler::Bilinear>(pass);
        break;
    case LensResampler::CatmullRom:
        run<LensResampler::CatmullRom>(pass);
        break;
    case LensResampler::Lanczos3:
        run<LensResampler::Lanczos3>(pass);
        break;
    }
    return target;
}

ImageBuffer arraw::applyLens(const ImageBuffer& source, const LensPlan& plan) {
    return detail::applyLens(source, plan, LensResampler::CatmullRom);
}

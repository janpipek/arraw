#include "Denoise.h"

#include "ColorAdjustments.h"
#include "ColorSpaces.h"
#include "RowBands.h"
#include "SampleConversion.h"
#include "TimingTrace.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

/// @brief Gives the tap radius that covers three sigmas, within the shaders' bound.
std::uint32_t radiusFor(float sigma) {
    const float taps = std::ceil(3.0F * sigma);
    return static_cast<std::uint32_t>(
        std::clamp(taps, 1.0F, static_cast<float>(maximumDenoiseRadius)));
}

/// @brief Gives the source's channels to working luminance, as shot.
///
/// The camera's own matrix, never the plan's `toWorking`, which carries the
/// white balance the photographer chose (ADR 007, ADR 039).
Colour lumaRowOf(const ColorEncoding& encoding) {
    if (const auto* camera = std::get_if<CameraNative>(&encoding)) {
        Colour row{};
        for (std::size_t column = 0; column < 3; ++column) {
            row[column] = colorspaces::workingLuminance[0] * camera->toWorking.at(0, column) +
                          colorspaces::workingLuminance[1] * camera->toWorking.at(1, column) +
                          colorspaces::workingLuminance[2] * camera->toWorking.at(2, column);
        }
        return row;
    }
    if (isWorkingEncoding(encoding)) {
        return colorspaces::workingLuminance;
    }
    throw std::invalid_argument(
        "Noise reduction starts from the working encoding or a camera's own primaries");
}

/// @brief Luminance of a colour through a row, in a fixed order the shaders repeat.
float lumaOf(const Colour& row, float red, float green, float blue) {
    return row[0] * red + row[1] * green + row[2] * blue;
}

/// @brief Gives the index of a sample of a plane, the coordinates clamped to its edges.
std::size_t clampedIndex(std::uint32_t width, std::uint32_t height, std::int64_t x,
                         std::int64_t y) {
    const auto column = static_cast<std::size_t>(std::clamp<std::int64_t>(x, 0, width - 1));
    const auto row = static_cast<std::size_t>(std::clamp<std::int64_t>(y, 0, height - 1));
    return row * width + column;
}

/// @brief Neighbours of a pixel along one axis, the coordinates clamped to the plane.
///
/// For the pixels within a radius of an edge.
struct ClampedNeighbours {
    std::uint32_t width;  ///< Samples per row.
    std::uint32_t height; ///< Rows.
    std::uint32_t x;      ///< Column of the centre.
    std::uint32_t y;      ///< Row of the centre.
    bool across;          ///< Whether the axis is horizontal.

    /// @brief Gives the index of the neighbour @p tap samples away, after (+) or before (-).
    [[nodiscard]] std::size_t at(std::uint32_t tap, bool after) const {
        const std::int64_t step = after ? std::int64_t{tap} : -std::int64_t{tap};
        return across ? clampedIndex(width, height, x + step, y)
                      : clampedIndex(width, height, x, y + step);
    }
};

/// @brief Neighbours of a pixel along one axis, all inside the plane.
///
/// The interior's fast path: the same samples ::ClampedNeighbours gives
/// there, without the clamping.
struct InteriorNeighbours {
    std::size_t index;  ///< Index of the centre.
    std::size_t stride; ///< Distance between neighbours: 1 across, the width down.

    /// @brief Gives the index of the neighbour @p tap samples away, after (+) or before (-).
    [[nodiscard]] std::size_t at(std::uint32_t tap, bool after) const {
        const std::size_t offset = tap * stride;
        return after ? index + offset : index - offset;
    }
};

/// @brief The bilateral at one pixel, along one axis.
///
/// Mirrors `bilateral` in `src/gpu/shaders/denoise_filter.frag`: the centre
/// weighs one, each tap the spatial weight times `exp(-d^2 * rangeFactor)` for
/// its perceptual difference from the centre. The same arithmetic in the same
/// order whichever neighbours it is given, so the interior and the border agree
/// with each other and with the shader.
template <typename Neighbours>
float bilateralAt(std::span<const float> luma, std::span<const float> encoded, std::size_t index,
                  const DenoiseWeights& weights, std::uint32_t radius, float rangeFactor,
                  const Neighbours& neighbours) {
    const float centre = encoded[index];
    float sum = luma[index];
    float total = 1.0F;
    for (std::uint32_t tap = 1; tap <= radius; ++tap) {
        const std::size_t afterIndex = neighbours.at(tap, true);
        const std::size_t beforeIndex = neighbours.at(tap, false);
        const float after = luma[afterIndex];
        const float before = luma[beforeIndex];
        const float afterDifference = encoded[afterIndex] - centre;
        const float beforeDifference = encoded[beforeIndex] - centre;
        const float afterWeight =
            weights[tap] * std::exp(-afterDifference * afterDifference * rangeFactor);
        const float beforeWeight =
            weights[tap] * std::exp(-beforeDifference * beforeDifference * rangeFactor);
        sum += afterWeight * after + beforeWeight * before;
        total += afterWeight + beforeWeight;
    }
    return sum / total;
}

/// @brief Tells whether a coordinate is at least a radius from both ends of an axis.
bool interior(std::uint32_t coordinate, std::uint32_t radius, std::uint32_t length) {
    return coordinate >= radius && static_cast<std::uint64_t>(coordinate) + radius < length;
}

/// @brief One pass of the separable bilateral along one axis.
///
/// Rows are split across threads (::arraw::detail::forEachRowBand); each pixel
/// is computed by ::bilateralAt from planes no thread writes, so the result is
/// the single-threaded one bit for bit.
/// @param luma Luminance to filter.
/// @param encoded Its perceptual values, ::arraw::perceptualLuma of each.
std::vector<float> bilateralPass(const std::vector<float>& luma, const std::vector<float>& encoded,
                                 std::uint32_t width, std::uint32_t height,
                                 const DenoiseWeights& weights, std::uint32_t radius,
                                 float rangeFactor, bool across) {
    std::vector<float> result(luma.size());
    const std::span<const float> in(luma);
    const std::span<const float> perceptual(encoded);
    detail::forEachRowBand(height, width, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t y = first; y < last; ++y) {
            const bool rowInside = across || interior(y, radius, height);
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                if (rowInside && (!across || interior(x, radius, width))) {
                    const InteriorNeighbours neighbours{index, across ? 1 : std::size_t{width}};
                    result[index] = bilateralAt(in, perceptual, index, weights, radius, rangeFactor,
                                                neighbours);
                } else {
                    const ClampedNeighbours neighbours{width, height, x, y, across};
                    result[index] = bilateralAt(in, perceptual, index, weights, radius, rangeFactor,
                                                neighbours);
                }
            }
        }
    });
    return result;
}

/// @brief Encodes every luminance of a plane into the perceptual coordinate.
std::vector<float> perceptualOf(const std::vector<float>& luma, std::uint32_t width,
                                std::uint32_t height) {
    std::vector<float> encoded(luma.size());
    detail::forEachRowBand(height, width, [&](std::uint32_t first, std::uint32_t last) {
        const std::size_t begin = static_cast<std::size_t>(first) * width;
        const std::size_t end = static_cast<std::size_t>(last) * width;
        for (std::size_t index = begin; index < end; ++index) {
            encoded[index] = perceptualLuma(luma[index]);
        }
    });
    return encoded;
}

/// @brief The separable bilateral: across, then down on what across made.
std::vector<float> bilateral(const std::vector<float>& luma, std::uint32_t width,
                             std::uint32_t height, const DenoisePlan& plan) {
    const DenoiseWeights weights = denoiseWeights(plan.spatialSigma, plan.spatialRadius);
    const float rangeFactor = rangeFactorOf(plan);
    const std::vector<float> across =
        bilateralPass(luma, perceptualOf(luma, width, height), width, height, weights,
                      plan.spatialRadius, rangeFactor, true);
    return bilateralPass(across, perceptualOf(across, width, height), width, height, weights,
                         plan.spatialRadius, rangeFactor, false);
}

/// @brief The luminance filter seam: luminance in, filtered luminance out.
///
/// Every filter reads and writes linear luminance; how it measures edges is
/// its own business. Decomposition and recombination stay outside, shared.
std::vector<float> filterLuminance(const std::vector<float>& luma, std::uint32_t width,
                                   std::uint32_t height, const DenoisePlan& plan) {
    switch (plan.filter) {
    case LuminanceNoiseFilter::Bilateral:
        return bilateral(luma, width, height, plan);
    }
    throw std::invalid_argument("Unknown luminance noise filter");
}

/// @brief The colour ratio on its grid, blurred.
struct RatioGrid {
    std::uint32_t width = 0;   ///< Cells across.
    std::uint32_t height = 0;  ///< Cells down.
    std::vector<Colour> cells; ///< Unit-luma ratios, row by row.
};

/// @brief The colour blur at one cell, along one axis.
///
/// Mirrors `blurRatio` in `denoise_filter.frag`: a normalised Gaussian. The same
/// arithmetic whichever neighbours it is given, as ::bilateralAt.
template <typename Neighbours>
Colour blurAt(std::span<const Colour> cells, std::size_t index, const DenoiseWeights& weights,
              std::uint32_t radius, const Neighbours& neighbours) {
    Colour sum = cells[index];
    float total = 1.0F;
    for (std::uint32_t tap = 1; tap <= radius; ++tap) {
        const Colour& after = cells[neighbours.at(tap, true)];
        const Colour& before = cells[neighbours.at(tap, false)];
        for (std::size_t channel = 0; channel < 3; ++channel) {
            sum[channel] += weights[tap] * after[channel] + weights[tap] * before[channel];
        }
        total += 2.0F * weights[tap];
    }
    Colour result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result[channel] = sum[channel] / total;
    }
    return result;
}

/// @brief One pass of the colour blur along one axis of the grid, edges clamped.
///
/// Rows split across threads as in ::bilateralPass, with the same result.
std::vector<Colour> blurPass(const std::vector<Colour>& cells, std::uint32_t width,
                             std::uint32_t height, const DenoiseWeights& weights,
                             std::uint32_t radius, bool across) {
    std::vector<Colour> result(cells.size());
    const std::span<const Colour> in(cells);
    detail::forEachRowBand(height, width, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t y = first; y < last; ++y) {
            const bool rowInside = across || interior(y, radius, height);
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                if (rowInside && (!across || interior(x, radius, width))) {
                    const InteriorNeighbours neighbours{index, across ? 1 : std::size_t{width}};
                    result[index] = blurAt(in, index, weights, radius, neighbours);
                } else {
                    const ClampedNeighbours neighbours{width, height, x, y, across};
                    result[index] = blurAt(in, index, weights, radius, neighbours);
                }
            }
        }
    });
    return result;
}

/// @brief Reduces the source to the grid, takes each cell's ratio and blurs it.
///
/// A cell is the plain mean of the `reduction x reduction` block of source
/// colours it covers; the last row and column average the pixels they have.
/// The ratio is then that of the mean colour, so dark pixels weigh less, as
/// their luminance does. Mirrors the `reduce`, `blurAcross` and `blurDown`
/// steps of `denoise_filter.frag`.
RatioGrid blurredRatios(std::span<const float> rgba, std::uint32_t width, std::uint32_t height,
                        const DenoisePlan& plan) {
    const std::uint32_t reduction = plan.gridReduction;
    RatioGrid grid;
    grid.width = (width + reduction - 1) / reduction;
    grid.height = (height + reduction - 1) / reduction;
    grid.cells.resize(static_cast<std::size_t>(grid.width) * grid.height);
    // Banded by grid rows; the work is that of the source pixels they cover.
    detail::forEachRowBand(
        grid.height, grid.width * reduction * reduction,
        [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t cy = first; cy < last; ++cy) {
                for (std::uint32_t cx = 0; cx < grid.width; ++cx) {
                    Colour sum{0.0F, 0.0F, 0.0F};
                    float count = 0.0F;
                    for (std::uint32_t dy = 0; dy < reduction; ++dy) {
                        const std::uint32_t y = cy * reduction + dy;
                        if (y >= height) {
                            break;
                        }
                        for (std::uint32_t dx = 0; dx < reduction; ++dx) {
                            const std::uint32_t x = cx * reduction + dx;
                            if (x >= width) {
                                break;
                            }
                            const float* pixel =
                                &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                            sum[0] += pixel[0];
                            sum[1] += pixel[1];
                            sum[2] += pixel[2];
                            count += 1.0F;
                        }
                    }
                    const Colour mean{sum[0] / count, sum[1] / count, sum[2] / count};
                    grid.cells[static_cast<std::size_t>(cy) * grid.width + cx] =
                        decompose(plan, mean).ratio;
                }
            }
        });
    const DenoiseWeights weights = denoiseWeights(plan.colorSigma, plan.colorRadius);
    grid.cells =
        blurPass(grid.cells, grid.width, grid.height, weights, plan.colorRadius, /*across=*/true);
    grid.cells =
        blurPass(grid.cells, grid.width, grid.height, weights, plan.colorRadius, /*across=*/false);
    return grid;
}

/// @brief Where a source pixel's centre falls on the grid along one axis, for bilinear reading.
struct GridTap {
    std::uint32_t first = 0;  ///< Cell at or before the position.
    std::uint32_t second = 0; ///< The next cell, or the same one at the edge.
    float fraction = 0.0F;    ///< Weight of the second cell.
};

/// @brief Places a source coordinate on the grid: `u = (x + 0.5) / reduction - 0.5`, clamped.
///
/// Mirrors `gridTap` in `denoise_combine.frag`. Exact in float: the reduction
/// is a power of two.
GridTap gridTap(std::uint32_t coordinate, std::uint32_t reduction, std::uint32_t cells) {
    const float position =
        (static_cast<float>(coordinate) + 0.5F) / static_cast<float>(reduction) - 0.5F;
    const float clamped = std::clamp(position, 0.0F, static_cast<float>(cells - 1));
    const float below = std::floor(clamped);
    GridTap tap;
    tap.first = static_cast<std::uint32_t>(below);
    tap.second = std::min(tap.first + 1, cells - 1);
    tap.fraction = clamped - below;
    return tap;
}

/// @brief Reads the grid bilinearly at a source pixel.
Colour upsampled(const RatioGrid& grid, const GridTap& across, const GridTap& down) {
    const auto at = [&grid](std::uint32_t x, std::uint32_t y) -> const Colour& {
        return grid.cells[static_cast<std::size_t>(y) * grid.width + x];
    };
    Colour result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float topLeft = at(across.first, down.first)[channel];
        const float topRight = at(across.second, down.first)[channel];
        const float bottomLeft = at(across.first, down.second)[channel];
        const float bottomRight = at(across.second, down.second)[channel];
        const float top = topLeft + across.fraction * (topRight - topLeft);
        const float bottom = bottomLeft + across.fraction * (bottomRight - bottomLeft);
        result[channel] = top + down.fraction * (bottom - top);
    }
    return result;
}

} // namespace

DenoisePlan arraw::denoisePlanFor(const NoiseReductionSettings& settings,
                                  const ColorEncoding& encoding, double pixelScale) {
    if (!std::isfinite(pixelScale) || !(pixelScale > 0.0)) {
        throw std::invalid_argument("A source's pixel scale must be finite and above zero");
    }
    const float luminance = clampedSetting(settings.luminance, minimumNoiseReduction,
                                           maximumNoiseReduction, "luminance noise reduction");
    const float detail = clampedSetting(settings.luminanceDetail, minimumNoiseReduction,
                                        maximumNoiseReduction, "luminance noise detail");
    const float color = clampedSetting(settings.color, minimumNoiseReduction, maximumNoiseReduction,
                                       "colour noise reduction");
    const float smoothness = clampedSetting(settings.colorSmoothness, minimumNoiseReduction,
                                            maximumNoiseReduction, "colour noise smoothness");
    if (std::ranges::none_of(luminanceNoiseFilterNames, [&settings](const auto& entry) {
            return entry.first == settings.luminanceFilter;
        })) {
        throw std::invalid_argument("Unknown luminance noise filter");
    }

    DenoisePlan plan;
    plan.luminance = reducesLuminanceNoise(settings);
    plan.color = reducesColorNoise(settings);
    if (!plan.active()) {
        return plan;
    }
    plan.lumaRow = lumaRowOf(encoding);
    const float rowSum = plan.lumaRow[0] + plan.lumaRow[1] + plan.lumaRow[2];
    if (!(rowSum > 0.0F) || !std::isfinite(rowSum)) {
        throw std::invalid_argument("The source's luminance row has no positive neutral");
    }
    plan.neutral = {1.0F / rowSum, 1.0F / rowSum, 1.0F / rowSum};

    // Exact for the powers of two a pyramid and a half-size decode give.
    const auto reach = static_cast<float>(1.0 / pixelScale);
    if (plan.luminance) {
        plan.filter = settings.luminanceFilter;
        plan.luminanceMix = luminance / maximumNoiseReduction;
        plan.rangeSigma = loosestLuminanceNoiseRange +
                          (tightestLuminanceNoiseRange - loosestLuminanceNoiseRange) *
                              (detail / maximumNoiseReduction);
        plan.spatialSigma = luminanceNoiseSpatialSigma * reach;
        plan.spatialRadius = radiusFor(plan.spatialSigma);
    }
    if (plan.color) {
        plan.colorMix = color / maximumNoiseReduction;
        // Whole source pixels per cell, at most the full-resolution four: an
        // enlarged source keeps the cell and widens the blur instead. Rounded
        // down to a power of two, so that gridTap's division is exact in float
        // on both backends whatever the scale.
        const double cells = std::clamp(std::floor(colorNoiseGridReduction / pixelScale), 1.0,
                                        static_cast<double>(colorNoiseGridReduction));
        plan.gridReduction = std::bit_floor(static_cast<std::uint32_t>(cells));
        plan.colorSigma = smoothness * colorNoiseSigmaPerSmoothness * reach /
                          static_cast<float>(plan.gridReduction);
        plan.colorRadius = radiusFor(plan.colorSigma);
    }
    return plan;
}

DenoiseWeights arraw::denoiseWeights(float sigma, std::uint32_t radius) {
    DenoiseWeights weights{};
    weights[0] = 1.0F;
    const float factor = 1.0F / (2.0F * sigma * sigma);
    const std::uint32_t last = std::min(radius, maximumDenoiseRadius);
    for (std::uint32_t tap = 1; tap <= last; ++tap) {
        const auto distance = static_cast<float>(tap);
        weights[tap] = std::exp(-distance * distance * factor);
    }
    return weights;
}

float arraw::rangeFactorOf(const DenoisePlan& plan) {
    return 1.0F / (2.0F * plan.rangeSigma * plan.rangeSigma);
}

float arraw::perceptualLuma(float luminance) {
    return luminance > 0.0F ? std::pow(luminance, 1.0F / 2.2F) : 0.0F;
}

LumaRatio arraw::decompose(const DenoisePlan& plan, Colour colour) {
    const float luminance = lumaOf(plan.lumaRow, colour[0], colour[1], colour[2]);
    const float scale = std::max(luminance, 0.0F) + denoiseRatioFloor;
    const float offset = scale - luminance;
    return {luminance,
            {(colour[0] + offset * plan.neutral[0]) / scale,
             (colour[1] + offset * plan.neutral[1]) / scale,
             (colour[2] + offset * plan.neutral[2]) / scale}};
}

Colour arraw::recompose(const DenoisePlan& plan, float luminance, Colour ratio) {
    const float scale = std::max(luminance, 0.0F) + denoiseRatioFloor;
    const float offset = scale - luminance;
    return {scale * ratio[0] - offset * plan.neutral[0],
            scale * ratio[1] - offset * plan.neutral[1],
            scale * ratio[2] - offset * plan.neutral[2]};
}

std::uint32_t arraw::denoiseReach(const DenoisePlan& plan) {
    std::uint32_t reach = 0;
    if (plan.luminance) {
        reach = plan.spatialRadius;
    }
    if (plan.color) {
        // The blur's reach on the grid, a cell for the bilinear read, and the
        // cell's own block.
        reach = std::max(reach, (plan.colorRadius + 2) * plan.gridReduction);
    }
    return reach;
}

ImageBuffer arraw::applyDenoise(const ImageBuffer& source, const DenoisePlan& plan) {
    const detail::TimingSpan timing("cpu.denoise");
    std::optional<ImageBuffer> converted;
    if (source.format() != PixelFormat::RgbaF32) {
        converted = toRgbaF32(source);
    }
    const ImageBuffer& input = converted ? *converted : source;
    const ImageSize size = input.size();
    const auto pixels = static_cast<std::size_t>(size.pixelCount());
    const std::span<const float> rgba = input.samples<float>();

    std::vector<float> luma(pixels);
    detail::forEachRowBand(size.height, size.width, [&](std::uint32_t first, std::uint32_t last) {
        const std::size_t begin = static_cast<std::size_t>(first) * size.width;
        const std::size_t end = static_cast<std::size_t>(last) * size.width;
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const float* in = &rgba[pixel * 4];
            luma[pixel] = lumaOf(plan.lumaRow, in[0], in[1], in[2]);
        }
    });
    std::vector<float> filtered;
    if (plan.luminance) {
        filtered = filterLuminance(luma, size.width, size.height, plan);
    }
    RatioGrid grid;
    std::vector<GridTap> columns;
    if (plan.color) {
        grid = blurredRatios(rgba, size.width, size.height, plan);
        columns.reserve(size.width);
        for (std::uint32_t x = 0; x < size.width; ++x) {
            columns.push_back(gridTap(x, plan.gridReduction, grid.width));
        }
    }

    ImageBuffer result(size, PixelFormat::RgbaF32, input.encoding(), input.orientation());
    result.setPixelScale(input.pixelScale());
    const auto out = result.samples<float>();
    detail::forEachRowBand(size.height, size.width, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t y = first; y < last; ++y) {
            const GridTap down =
                plan.color ? gridTap(y, plan.gridReduction, grid.height) : GridTap{};
            for (std::uint32_t x = 0; x < size.width; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * size.width + x;
                const float* in = &rgba[pixel * 4];
                const LumaRatio split = decompose(plan, {in[0], in[1], in[2]});
                float luminance = split.luminance;
                if (plan.luminance) {
                    luminance += plan.luminanceMix * (filtered[pixel] - luminance);
                }
                Colour ratio = split.ratio;
                if (plan.color) {
                    const Colour blurred = upsampled(grid, columns[x], down);
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        ratio[channel] += plan.colorMix * (blurred[channel] - ratio[channel]);
                    }
                }
                const Colour colour = recompose(plan, luminance, ratio);
                float* target = &out[pixel * 4];
                target[0] = colour[0];
                target[1] = colour[1];
                target[2] = colour[2];
                target[3] = in[3];
            }
        }
    });
    return result;
}

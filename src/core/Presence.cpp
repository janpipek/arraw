#include "Presence.h"

#include "ColorAdjustments.h"
#include "Denoise.h"
#include "ProcessingPlan.h"
#include "ProgressScope.h"
#include "RowBands.h"
#include "SampleConversion.h"
#include "TimingTrace.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

/// @brief Resolves one base from its sigma and cell, both in sensor pixels.
///
/// The cell in source pixels is the sensor cell divided by the pixel scale,
/// rounded down to a power of two and at least one, so a pyramid level of a
/// source covers the same sensor pixels per cell as the source does for as long
/// as it can (ADR 041). The sigma follows in cells, at least
/// ::arraw::minimumPresenceSigma and at most what the radius bound covers.
PresenceBase baseFor(float sigmaSensor, std::uint32_t cellSensor, double pixelScale) {
    const double cells =
        std::clamp(std::floor(cellSensor / pixelScale), 1.0, static_cast<double>(cellSensor));
    PresenceBase base;
    base.reduction = std::bit_floor(static_cast<std::uint32_t>(cells));
    const double sigma = sigmaSensor / pixelScale / base.reduction;
    base.sigma = static_cast<float>(std::clamp(sigma, static_cast<double>(minimumPresenceSigma),
                                               static_cast<double>(maximumPresenceRadius) / 3.0));
    base.radius = static_cast<std::uint32_t>(
        std::clamp(std::ceil(3.0F * base.sigma), 1.0F, static_cast<float>(maximumPresenceRadius)));
    return base;
}

/// @brief Resolves a window radius, in sensor pixels, to cells of a base.
///
/// As the sigma: the same sensor pixels while the cell can keep its size, at
/// least one cell and at most the shader's bound.
std::uint32_t windowFor(float windowSensor, double pixelScale, std::uint32_t reduction) {
    const double cells = std::round(windowSensor / pixelScale / reduction);
    return static_cast<std::uint32_t>(
        std::clamp(cells, 1.0, static_cast<double>(maximumPresenceRadius)));
}

/// @brief Luminance of a colour through a row, in a fixed order the shaders repeat.
float lumaOf(const Colour& row, float red, float green, float blue) {
    return row[0] * red + row[1] * green + row[2] * blue;
}

/// @brief Reduces the input to one base's grid of log2 mean luminance.
///
/// Mirrors the `reduce` step of `presence_filter.frag`: each cell sums the
/// bounded luminance of the pixels it covers, rows then columns, and takes the
/// logarithm of the mean, which the bounds keep finite.
template <typename Sample>
PresenceGrid reducedGrid(const ImageBuffer& input, const Colour& row, std::uint32_t reduction) {
    const ImageSize size = input.size();
    const auto samples = input.samples<Sample>();
    const std::size_t channels = channelCount(input.format());
    PresenceGrid grid;
    grid.reduction = reduction;
    grid.width = gridCells(size.width, reduction);
    grid.height = gridCells(size.height, reduction);
    grid.cells.resize(static_cast<std::size_t>(grid.width) * grid.height);
    detail::forEachRowBand(
        grid.height, grid.width * reduction * reduction,
        [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t cy = first; cy < last; ++cy) {
                for (std::uint32_t cx = 0; cx < grid.width; ++cx) {
                    float sum = 0.0F;
                    float count = 0.0F;
                    for (std::uint32_t dy = 0; dy < reduction; ++dy) {
                        const std::uint32_t y = cy * reduction + dy;
                        if (y >= size.height) {
                            break;
                        }
                        for (std::uint32_t dx = 0; dx < reduction; ++dx) {
                            const std::uint32_t x = cx * reduction + dx;
                            if (x >= size.width) {
                                break;
                            }
                            const auto* pixel =
                                &samples[(static_cast<std::size_t>(y) * size.width + x) * channels];
                            sum += boundedLuminance(
                                lumaOf(row, toUnit(pixel[0]), toUnit(pixel[1]), toUnit(pixel[2])));
                            count += 1.0F;
                        }
                    }
                    grid.cells[static_cast<std::size_t>(cy) * grid.width + cx] =
                        std::log2(sum / count);
                }
            }
        });
    return grid;
}

/// @brief Gives the index of a cell, the coordinates clamped to the grid.
std::size_t clampedCell(const PresenceGrid& grid, std::int64_t x, std::int64_t y) {
    const auto column = static_cast<std::size_t>(std::clamp<std::int64_t>(x, 0, grid.width - 1));
    const auto line = static_cast<std::size_t>(std::clamp<std::int64_t>(y, 0, grid.height - 1));
    return line * grid.width + column;
}

/// @brief One pass of a base's blur along one axis, edges clamped.
///
/// Mirrors `blur` in `presence_filter.frag`: a normalised Gaussian whose centre
/// weighs one, the taps summed in pairs outward. Rows split across threads, as
/// in the Denoise pass, with the single-threaded result.
std::vector<float> blurPass(const PresenceGrid& grid, const DenoiseWeights& weights,
                            std::uint32_t radius, bool across) {
    std::vector<float> result(grid.cells.size());
    const std::span<const float> in(grid.cells);
    detail::forEachRowBand(grid.height, grid.width, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t y = first; y < last; ++y) {
            for (std::uint32_t x = 0; x < grid.width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * grid.width + x;
                float sum = in[index];
                float total = 1.0F;
                for (std::uint32_t tap = 1; tap <= radius; ++tap) {
                    const std::int64_t step = tap;
                    const float after = across ? in[clampedCell(grid, x + step, y)]
                                               : in[clampedCell(grid, x, y + step)];
                    const float before = across ? in[clampedCell(grid, x - step, y)]
                                                : in[clampedCell(grid, x, y - step)];
                    sum += weights[tap] * after + weights[tap] * before;
                    total += 2.0F * weights[tap];
                }
                result[index] = sum / total;
            }
        }
    });
    return result;
}

/// @brief A direction a pass of the opening steps along, in cells.
struct GridAxis {
    std::int64_t x = 0; ///< Cells across a step.
    std::int64_t y = 0; ///< Cells down a step.
};

/// @brief The four directions of the octagon's passes, in their order: across, down, and the
/// diagonal and the antidiagonal.
constexpr std::array<GridAxis, 4> octagonAxes{GridAxis{1, 0}, GridAxis{0, 1}, GridAxis{1, 1},
                                              GridAxis{1, -1}};

/// @brief One pass of a window's minimum, or maximum, along one direction, each coordinate
/// clamped to the grid.
///
/// Mirrors `extremum` in `presence_filter.frag`; exact on both, as a minimum is.
std::vector<float> extremumPass(const PresenceGrid& grid, std::uint32_t window, GridAxis axis,
                                bool maximum) {
    std::vector<float> result(grid.cells.size());
    const std::span<const float> in(grid.cells);
    const auto pick = [maximum](float a, float b) {
        return maximum ? std::max(a, b) : std::min(a, b);
    };
    detail::forEachRowBand(
        grid.height, grid.width * (2 * window + 1), [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t y = first; y < last; ++y) {
                for (std::uint32_t x = 0; x < grid.width; ++x) {
                    float extreme = in[static_cast<std::size_t>(y) * grid.width + x];
                    for (std::uint32_t tap = 1; tap <= window; ++tap) {
                        const std::int64_t step = tap;
                        const float after =
                            in[clampedCell(grid, x + step * axis.x, y + step * axis.y)];
                        const float before =
                            in[clampedCell(grid, x - step * axis.x, y - step * axis.y)];
                        extreme = pick(extreme, pick(after, before));
                    }
                    result[static_cast<std::size_t>(y) * grid.width + x] = extreme;
                }
            }
        });
    return result;
}

/// @brief One step of the opening's reconstruction: the maximum over a cell's 3x3
/// neighbourhood, edges clamped, then the minimum of that and the cell unopened.
///
/// Mirrors `reconstruct` in `presence_filter.frag`; exact on both, as a minimum
/// and a maximum are.
std::vector<float> reconstructionPass(const PresenceGrid& grid, std::span<const float> cells) {
    std::vector<float> result(grid.cells.size());
    const std::span<const float> in(grid.cells);
    detail::forEachRowBand(
        grid.height, grid.width * 9, [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t y = first; y < last; ++y) {
                for (std::uint32_t x = 0; x < grid.width; ++x) {
                    const std::size_t index = static_cast<std::size_t>(y) * grid.width + x;
                    float extreme = in[index];
                    for (std::int64_t dy = -1; dy <= 1; ++dy) {
                        for (std::int64_t dx = -1; dx <= 1; ++dx) {
                            extreme = std::max(extreme, in[clampedCell(grid, x + dx, y + dy)]);
                        }
                    }
                    result[index] = std::min(extreme, cells[index]);
                }
            }
        });
    return result;
}

/// @brief Reduces the input to a grid of log2 mean luminance, in the input's sample type.
PresenceGrid cellsOf(const ImageBuffer& input, const Colour& row, std::uint32_t reduction) {
    switch (input.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        return reducedGrid<std::uint8_t>(input, row, reduction);
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        return reducedGrid<std::uint16_t>(input, row, reduction);
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        break;
    }
    return reducedGrid<float>(input, row, reduction);
}

/// @brief Computes one base from its cells: the opening by the window, if any, then the blur.
///
/// The opening is by a regular octagon (::arraw::octagonOf), the minimum along
/// its four directions and then the maximum, and is reconstructed by the
/// base's few steps, so that a bright area holding the window keeps its own
/// floor up to its edges whatever their shape; with a window the blur never
/// lowers a cell below the opening, so that a dark neighbour's floor does not
/// spread across an edge into a bright area (ADR 041).
PresenceGrid baseOf(PresenceGrid grid, const PresenceBase& base) {
    std::vector<float> opened;
    if (base.window != 0) {
        const std::vector<float> cells = grid.cells;
        const OctagonWindow octagon = octagonOf(base.window);
        for (const bool maximum : {false, true}) {
            for (std::size_t index = 0; index < octagonAxes.size(); ++index) {
                const std::uint32_t radius = index < 2 ? octagon.across : octagon.diagonal;
                grid.cells = extremumPass(grid, radius, octagonAxes[index], maximum);
            }
        }
        for (std::uint32_t step = 0; step < base.reconstruction; ++step) {
            grid.cells = reconstructionPass(grid, cells);
        }
        opened = grid.cells;
    }
    const DenoiseWeights weights = denoiseWeights(base.sigma, base.radius);
    grid.cells = blurPass(grid, weights, base.radius, /*across=*/true);
    grid.cells = blurPass(grid, weights, base.radius, /*across=*/false);
    if (!opened.empty()) {
        std::ranges::transform(grid.cells, opened, grid.cells.begin(),
                               [](float blurred, float open) { return std::max(blurred, open); });
    }
    return grid;
}

/// @brief Reads a grid bilinearly between four cells, rows first.
float upsampled(const PresenceGrid& grid, const GridTap& across, const GridTap& down) {
    const auto at = [&grid](std::uint32_t x, std::uint32_t y) {
        return grid.cells[static_cast<std::size_t>(y) * grid.width + x];
    };
    const float topLeft = at(across.first, down.first);
    const float topRight = at(across.second, down.first);
    const float bottomLeft = at(across.first, down.second);
    const float bottomRight = at(across.second, down.second);
    const float top = topLeft + across.fraction * (topRight - topLeft);
    const float bottom = bottomLeft + across.fraction * (bottomRight - bottomLeft);
    return top + down.fraction * (bottom - top);
}

/// @brief Weight of Clarity at a perceptual luminance: full in the midtones, none at the ends.
float midtoneWeight(float value) {
    return smoothstep(0.0F, 0.8F, 1.0F - std::abs(2.0F * value - 1.0F));
}

} // namespace

PresencePlan arraw::presencePlanFor(const PresenceSettings& settings, const ColorEncoding& encoding,
                                    double pixelScale, ImageSize sourceSize) {
    if (!std::isfinite(pixelScale) || !(pixelScale > 0.0)) {
        throw std::invalid_argument("A source's pixel scale must be finite and above zero");
    }
    const float texture =
        clampedSetting(settings.texture, weakestPresence, strongestPresence, "texture");
    const float clarity =
        clampedSetting(settings.clarity, weakestPresence, strongestPresence, "clarity");
    const float dehaze =
        clampedSetting(settings.dehaze, weakestPresence, strongestPresence, "dehaze");
    PresencePlan plan;
    if (texture == 0.0F && clarity == 0.0F && dehaze == 0.0F) {
        return plan;
    }
    plan.texture = texture / strongestPresence;
    plan.clarity = clarity / strongestPresence;
    plan.dehaze = dehaze / strongestPresence;
    plan.lumaRow = asShotLuminanceRow(encoding);
    if (plan.texture != 0.0F) {
        plan.fine = baseFor(textureSigmaSensorPixels, textureCellSensorPixels, pixelScale);
    }
    if (plan.clarity != 0.0F || plan.dehaze != 0.0F) {
        // The long edge in sensor pixels, so a reduced copy of a source resolves
        // the same sigma and cell in sensor pixels as the source itself.
        const double longEdge =
            static_cast<double>(std::max(sourceSize.width, sourceSize.height)) * pixelScale;
        const double sigma = claritySigmaFraction * longEdge;
        const double cell = std::max(1.0, std::floor(sigma / clarityCellsPerSigma));
        const std::uint32_t cellSensor = std::bit_floor(
            static_cast<std::uint32_t>(std::min(cell, static_cast<double>(1U << 16))));
        if (plan.clarity != 0.0F) {
            plan.coarse = baseFor(static_cast<float>(sigma), cellSensor, pixelScale);
        }
        if (plan.dehaze != 0.0F) {
            // The same cells, so that the two share one reduction.
            const float sigmaFraction =
                plan.dehaze > 0.0F ? hazeFloorSigmaFraction : hazeMeanSigmaFraction;
            plan.haze =
                baseFor(static_cast<float>(sigmaFraction * longEdge), cellSensor, pixelScale);
            if (plan.dehaze > 0.0F) {
                plan.haze.window = windowFor(static_cast<float>(hazeWindowFraction * longEdge),
                                             pixelScale, plan.haze.reduction);
                plan.haze.reconstruction = hazeReconstructionSteps;
            }
        }
    }
    return plan;
}

std::uint32_t arraw::presenceReach(const PresencePlan& plan) {
    std::uint32_t reach = 0;
    for (const PresenceBase* base : {&plan.fine, &plan.coarse, &plan.haze}) {
        if (base->active()) {
            // The opening's minimum and maximum, a window each (the octagon's
            // axis inradius, its widest reach along a row or a column), then
            // its reconstruction, a cell a step.
            reach = std::max(reach, (2 * base->window + base->reconstruction + base->radius + 2) *
                                        base->reduction);
        }
    }
    return reach;
}

ImageSize arraw::presenceGridSize(const PresenceBase& base, ImageSize source) {
    const std::uint32_t reduction = std::max<std::uint32_t>(base.reduction, 1U);
    return {gridCells(source.width, reduction), gridCells(source.height, reduction)};
}

namespace {

/// @brief Appends the cost of each loop ::baseOf runs for a base of a source's size, in order.
void appendBaseLoops(std::vector<double>& weights, const PresenceBase& base, ImageSize size) {
    // Nanoseconds of wall time per cell and tap, as in presenceLoopWeights.
    constexpr double tapCost = 0.4;
    const ImageSize grid = presenceGridSize(base, size);
    const double cells = static_cast<double>(grid.width) * grid.height;
    if (base.window != 0) {
        const OctagonWindow octagon = octagonOf(base.window);
        for (int extremum = 0; extremum < 2; ++extremum) {
            for (std::size_t index = 0; index < octagonAxes.size(); ++index) {
                const std::uint32_t radius = index < 2 ? octagon.across : octagon.diagonal;
                weights.push_back(tapCost * (2.0 * radius + 1.0) * cells);
            }
        }
        for (std::uint32_t step = 0; step < base.reconstruction; ++step) {
            weights.push_back(tapCost * 9.0 * cells);
        }
    }
    const double blur = tapCost * (2.0 * base.radius + 1.0) * cells;
    weights.insert(weights.end(), {blur, blur});
}

} // namespace

std::vector<double> arraw::presenceLoopWeights(const PresencePlan& plan, ImageSize size) {
    // Nanoseconds of wall time per input pixel of a reduction to cells, on
    // eight threads of a release build at 24 MP (ADR 042).
    constexpr double reduceCost = 1.1;
    const double reduce = reduceCost * static_cast<double>(size.pixelCount());
    std::vector<double> weights;
    if (plan.fine.active()) {
        weights.push_back(reduce);
        appendBaseLoops(weights, plan.fine, size);
    }
    if (plan.coarse.active() || plan.haze.active()) {
        weights.push_back(reduce);
        if (plan.haze.active()) {
            appendBaseLoops(weights, plan.haze, size);
        }
        if (plan.coarse.active()) {
            appendBaseLoops(weights, plan.coarse, size);
        }
    }
    return weights;
}

PresenceContext arraw::presenceContextOf(const ImageBuffer& input, const PresencePlan& plan) {
    const detail::TimingSpan timing("cpu.presence-context");
    // One unit a loop, in the order below and in baseOf.
    const std::vector<double> loops = presenceLoopWeights(plan, input.size());
    const detail::ProgressSpan progress(ProgressStep::Context, loops);
    PresenceContext context;
    if (plan.fine.active()) {
        context.fine = baseOf(cellsOf(input, plan.lumaRow, plan.fine.reduction), plan.fine);
    }
    if (plan.coarse.active() || plan.haze.active()) {
        // One reduction for both: the plan gives them the same cell.
        PresenceGrid cells = cellsOf(input, plan.lumaRow, plan.coarseReduction());
        if (plan.haze.active()) {
            context.haze = baseOf(cells, plan.haze);
        }
        if (plan.coarse.active()) {
            context.coarse = baseOf(cells, plan.coarse);
        }
        // Clarity's band and positive Dehaze's share both read the cells unblurred.
        if (plan.coarse.active() || plan.haze.window != 0) {
            context.coarseCells = std::move(cells);
        }
    }
    return context;
}

PresenceSampler::PresenceSampler(const PresenceContext& context, ImageSize size)
    : context_(context) {
    const auto columns = [&size](const PresenceGrid& grid, std::vector<GridTap>& taps) {
        if (grid.cells.empty()) {
            return;
        }
        taps.reserve(size.width);
        for (std::uint32_t x = 0; x < size.width; ++x) {
            taps.push_back(gridTap(x, grid.reduction, grid.width));
        }
    };
    columns(context.fine, fineColumns_);
    // Clarity's grids and Dehaze's share the coarse cells, and so their taps.
    columns(context.coarse.cells.empty() ? context.haze : context.coarse, coarseColumns_);
}

void PresenceSampler::setRow(std::uint32_t y) {
    if (!context_.fine.cells.empty()) {
        fineRow_ = gridTap(y, context_.fine.reduction, context_.fine.height);
    }
    const PresenceGrid& coarse = context_.coarse.cells.empty() ? context_.haze : context_.coarse;
    if (!coarse.cells.empty()) {
        coarseRow_ = gridTap(y, coarse.reduction, coarse.height);
    }
}

PixelContext PresenceSampler::at(std::uint32_t x) const {
    PixelContext pixel;
    if (!fineColumns_.empty()) {
        pixel.fineBase = upsampled(context_.fine, fineColumns_[x], fineRow_);
    }
    if (!context_.coarse.cells.empty()) {
        pixel.coarseBase = upsampled(context_.coarse, coarseColumns_[x], coarseRow_);
    }
    if (!context_.coarseCells.cells.empty()) {
        pixel.coarseCell = upsampled(context_.coarseCells, coarseColumns_[x], coarseRow_);
    }
    if (!context_.haze.cells.empty()) {
        pixel.hazeBase = upsampled(context_.haze, coarseColumns_[x], coarseRow_);
    }
    return pixel;
}

Colour arraw::applyPresence(const PresencePlan& plan, Colour colour, float logLuminance,
                            const PixelContext& context) {
    if (!plan.active()) {
        return colour;
    }
    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    // Black, and NaN, have no ratio to scale by.
    if (!(luminance > liftedBlackThreshold)) {
        return colour;
    }

    float stops = 0.0F;
    if (plan.fine.active()) {
        stops += plan.texture * softLimit(logLuminance - context.fineBase, textureLimitStops);
    }
    if (plan.coarse.active()) {
        // Clarity's band: the unblurred coarse grid against its blur, so the
        // detail finer than a cell, Texture's and the noise, stays out of it.
        const float limited = softLimit(context.coarseCell - context.coarseBase, clarityLimitStops);
        const float perceptual = toPerceptual(std::clamp(luminance, 0.0F, 1.0F));
        stops += plan.clarity * midtoneWeight(perceptual) * limited;
    }
    const float gain = std::exp2(stops);
    colour = {colour[0] * gain, colour[1] * gain, colour[2] * gain};
    if (!plan.haze.active()) {
        return colour;
    }

    // Nearly white is not hazy, whatever its surroundings.
    const float toned = luminance * gain;
    const float open = 1.0F - smoothstep(0.75F, 1.25F, toned);
    float hazy = 0.0F;
    if (plan.dehaze > 0.0F) {
        // The floor's share of the cell around the pixel: one at or below the
        // floor, less above it. Scaling by 1 - k * share subtracts k times the
        // floor from a cell above it, raising contrast at every scale above a
        // cell, and leaves one at it 1 - k of itself, never crossing black.
        // Measured against the smooth cell rather than the pixel, so detail
        // finer than a cell, and noise, is scaled smoothly, not expanded.
        hazy = std::exp2(std::min(context.hazeBase - context.coarseCell, 0.0F)) * open;
        const float keep = 1.0F - plan.dehaze * dehazeStrength * hazy;
        colour = {colour[0] * keep, colour[1] * keep, colour[2] * keep};
    } else {
        // The surroundings' mean in the colour's own scale.
        const float mean =
            toned * std::exp2(std::min(context.hazeBase - logLuminance, dehazeMeanLimitStops));
        // Nothing for what is nearly white, nor so for an infinite pixel,
        // whose mean is infinite too.
        const float veil = open > 0.0F ? -plan.dehaze * dehazeVeil * open * mean : 0.0F;
        hazy = veil > 0.0F ? veil / (toned + veil) : 0.0F;
        colour = {colour[0] + veil, colour[1] + veil, colour[2] + veil};
    }
    const float chroma = plan.dehaze * dehazeChroma * hazy;
    return chroma == 0.0F ? colour : applySaturation(colour, chroma);
}

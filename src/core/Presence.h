#pragma once

#include "ColorSpaces.h"
#include "ReducedGrid.h"

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <PresenceSettings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <vector>

namespace arraw {

/// @brief Luminance below which the Presence controls measure every value as this one.
///
/// 2^-14, the floor of a 14-bit raw file, as ::arraw::denoiseRatioFloor: the
/// logarithm of anything darker, zero, negative or NaN is -14 (ADR 041).
inline constexpr float presenceLuminanceFloor = 0x1p-14F;

/// @brief Luminance above which the Presence controls measure every value as this one.
///
/// 2^16, far above any scene value a development produces: it keeps an
/// infinite pixel from turning the context and the detail infinite (ADR 041).
inline constexpr float presenceLuminanceCeiling = 0x1p16F;

/// @brief Bounds a luminance to what the Presence controls measure: NaN and below the floor
/// to the floor, above the ceiling (and infinity) to the ceiling.
///
/// Every luminance the context sums or the chain takes the logarithm of goes
/// through this, so that a non-finite pixel moves its cell by a bounded
/// amount rather than poisoning its neighbourhood.
/// Mirrored by `boundedLuminance` in `src/gpu/shaders/common/presence_bounds.glsl`,
/// which `presence_filter.frag` and `develop.frag` include.
[[nodiscard]] inline float boundedLuminance(float luminance) {
    if (!(luminance > presenceLuminanceFloor)) {
        return presenceLuminanceFloor;
    }
    return luminance < presenceLuminanceCeiling ? luminance : presenceLuminanceCeiling;
}

/// @brief Sigma of Texture's base, in sensor pixels.
///
/// Texture is a detail control: the scale of skin, bark or fabric, which has
/// the size of a few sensor pixels whatever the frame (ADR 041).
inline constexpr float textureSigmaSensorPixels = 4.0F;

/// @brief Sensor pixels per side of a cell of Texture's grid, at full resolution.
inline constexpr std::uint32_t textureCellSensorPixels = 2;

/// @brief Sigma of Clarity's base, as a fraction of the long edge.
///
/// A composition-scale control: one hundredth of the uncropped long edge, 60
/// sensor pixels on a 6000-pixel frame, so a preview level shows the same
/// local contrast an export does (ADR 041).
inline constexpr float claritySigmaFraction = 0.01F;

/// @brief Cells of the coarse grid one Clarity sigma spans, at least.
///
/// The coarse cell, shared by Clarity's and Dehaze's bases, is the largest
/// power of two of sensor pixels that leaves this many cells a sigma: four to
/// eight, so the Gaussian is sampled finely enough for its bilinear read to be
/// smooth.
inline constexpr float clarityCellsPerSigma = 4.0F;

/// @brief Smallest sigma of either base, in cells.
///
/// On a deep preview level Texture's sensor-pixel sigma falls below one pixel
/// of the level; it is held at one, so the preview shows texture at the finest
/// scale it has rather than none.
inline constexpr float minimumPresenceSigma = 1.0F;

/// @brief Largest tap radius of either base's blur, the shader's compile-time bound.
inline constexpr std::uint32_t maximumPresenceRadius = 64;

/// @brief Stops at which Texture's detail is soft-limited: no pixel moves by more.
inline constexpr float textureLimitStops = 0.5F;

/// @brief Stops at which Clarity's detail is soft-limited: no pixel moves by more.
///
/// The bound a halo at a step edge cannot exceed, at Clarity 100 (ADR 041).
inline constexpr float clarityLimitStops = 1.0F;

/// @brief Radius of the window of Dehaze's floor, as a fraction of the long edge.
///
/// 3%, 180 sensor pixels on a 6000-pixel frame: the inradius of the regular
/// octagon the floor is opened by. The floor is the opening of the coarse
/// cells by this window, their minimum over it and then the maximum of that,
/// shortly reconstructed towards the cells: wide enough that a window almost
/// always holds something dark, the dark-channel assumption, so what it finds
/// is the veil over it; and a region that holds the window keeps its own floor
/// up to its edges, whatever their shape, so a broad flat area such as a sky is
/// not left with a rim (ADR 041).
inline constexpr float hazeWindowFraction = 0.03F;

/// @brief Steps of the reconstruction of Dehaze's opened floor.
///
/// The octagon reaches a curved edge to within about 0.08 of its inradius
/// (1.9 cells at 24 MP); two 3x3 steps close that for every disk that holds
/// the octagon, measured on the cells for windows of 3 to 23 cells (the window
/// is at most 24 at full resolution and on power-of-two pyramid levels), and
/// climb hardly further into a textured neighbour of a bright area. They do
/// not close the tip of a right-angled corner, which the octagon cuts by a
/// triangle with legs of twice its diagonal (ADR 041).
inline constexpr std::uint32_t hazeReconstructionSteps = 2;

/// @brief The two half-widths of a regular octagon made of four 1-D passes.
///
/// A square of half-width @ref across (a pass across, a pass down) widened by
/// the two diagonal segments of @ref diagonal cells each: `across + 2 *
/// diagonal` cells out along the axes, `sqrt(2) * (across + diagonal)` along
/// the diagonals.
struct OctagonWindow {
    std::uint32_t across = 0;   ///< Half-width of the passes across and down, in cells.
    std::uint32_t diagonal = 0; ///< Half-length of the diagonal passes, in diagonal steps.

    friend bool operator==(const OctagonWindow&, const OctagonWindow&) = default;
};

/// @brief Splits an octagon's inradius into its passes' half-widths.
///
/// A regular octagon needs `across = sqrt(2) * diagonal`, so `diagonal =
/// round(r * (1 - 1 / sqrt(2)))`, about 0.29 r, and `across = r - 2 *
/// diagonal`, about 0.41 r: the axis inradius is exactly @p inradius and the
/// diagonal one within a cell of it (22.6 for 23, 18.4 for 18). A diagonal
/// pass reaches only the cells of one parity, `x + y` even or odd about the
/// centre; @ref OctagonWindow::across is kept at least one, so the passes
/// across and down mix both (tested) and a radius of one or two is a square.
[[nodiscard]] constexpr OctagonWindow octagonOf(std::uint32_t inradius) noexcept {
    if (inradius == 0) {
        return {};
    }
    // 1 - 1/sqrt(2), rounded half up in integers.
    constexpr double share = 0.29289321881345248;
    const auto rounded = static_cast<std::uint32_t>(inradius * share + 0.5);
    const std::uint32_t diagonal = std::min(rounded, (inradius - 1) / 2);
    return {.across = inradius - 2 * diagonal, .diagonal = diagonal};
}

/// @brief Sigma of the blur that smooths Dehaze's floor, as a fraction of the long edge.
///
/// A quarter of a percent, 15 sensor pixels on a 6000-pixel frame: about two
/// cells, enough to hide the cells' steps. The blur never lowers a cell below
/// its opening, so a dark area's floor does not spread across an edge into a
/// bright one (ADR 041).
inline constexpr float hazeFloorSigmaFraction = 0.0025F;

/// @brief Sigma of the blur that gives negative Dehaze its mean, as a fraction of the long edge.
///
/// 2%, twice Clarity's: the veil follows the light of a broad neighbourhood.
inline constexpr float hazeMeanSigmaFraction = 0.02F;

/// @brief Share of the veil positive Dehaze takes off at full strength.
///
/// Less than one, so that a cell at its surroundings' floor, and every pixel in
/// it, keeps 40% of itself: nothing crosses black, and the darkest tones keep
/// their order.
inline constexpr float dehazeStrength = 0.6F;

/// @brief Veil negative Dehaze adds at full strength, as a fraction of the surroundings' mean.
inline constexpr float dehazeVeil = 0.4F;

/// @brief Saturation Dehaze adds (or takes away) where the veil is full, at full strength.
inline constexpr float dehazeChroma = 0.16F;

/// @brief Stops above the pixel at which the mean negative Dehaze measures its veil is held.
///
/// A pixel far darker than its surroundings, a deep shadow or one at the
/// luminance floor, gets the veil of a pixel this many stops below them.
inline constexpr float dehazeMeanLimitStops = 6.0F;

/// @brief One blurred base of the Presence context: a box-reduced grid of log luminance.
///
/// The grid, then optionally its opening by an octagonal window (the minimum
/// over it, then the maximum of that, then a short reconstruction), then a
/// Gaussian blur, which with a window never lowers a cell below its opening.
struct PresenceBase {
    /// @brief Source pixels per side of a cell; a power of two, or zero when the base is not used.
    std::uint32_t reduction = 0;

    /// @brief Sigma of the Gaussian blur, in cells.
    float sigma = 0.0F;

    /// @brief Tap radius of the blur, in cells.
    std::uint32_t radius = 0;

    /// @brief Inradius in cells of the regular octagon the grid is opened by before the blur
    /// (::arraw::octagonOf); zero for none.
    std::uint32_t window = 0;

    /// @brief Steps of the opening's reconstruction, each a 3x3 maximum bounded by the cells; zero
    /// for none.
    ///
    /// Each step brings an opened bright area back by one cell towards its own
    /// outline, where the octagon does not reach, such as along a curved edge
    /// (::arraw::hazeReconstructionSteps). A fixed count, so the device renders
    /// as many steps every time.
    std::uint32_t reconstruction = 0;

    /// @brief Whether the base is computed and read.
    [[nodiscard]] bool active() const noexcept {
        return reduction != 0;
    }

    friend bool operator==(const PresenceBase&, const PresenceBase&) = default;
};

/// @brief The three Presence controls, resolved: the values a pixel applies.
///
/// Kept apart from the parameters of the context so that the chain can take
/// them as one value, and so that a pixel may have amounts of its own (ADR 044).
struct PresenceAmounts {
    float texture = 0.0F; ///< Texture, minus one to one.
    float clarity = 0.0F; ///< Clarity, minus one to one.
    float dehaze = 0.0F;  ///< Dehaze, minus one to one.

    friend bool operator==(const PresenceAmounts&, const PresenceAmounts&) = default;
};

/// @brief Resolves one Presence control from its setting.
///
/// Called by the planner for the whole photograph, and by the chain for a pixel
/// once a pixel can have a setting of its own (ADR 044).
/// @param setting A finite setting inside the Presence range.
/// @return The amount, minus one to one.
[[nodiscard]] constexpr float presenceAmountFor(float setting) noexcept {
    return setting / strongestPresence;
}

/// @brief Texture, Clarity and Dehaze, resolved: part of the pointwise group of a plan.
///
/// The amounts and the context's parameters. The context, a side image of
/// the input of the pointwise pass, is computed from the pixels after noise
/// reduction and from @ref lumaRow, @ref fine, @ref coarse and @ref haze
/// alone, none of which white balance, exposure or the amounts reach, only
/// Dehaze's sign (ADR 041). Every control at zero resolves to the default,
/// and nothing is computed.
struct PresencePlan {
    /// @brief Texture, Clarity and Dehaze, each minus one to one.
    PresenceAmounts amounts{};

    /// @brief Source channels to working luminance, as shot (::arraw::asShotLuminanceRow).
    Colour lumaRow{};

    /// @brief Texture's base: active when Texture is not zero.
    PresenceBase fine{};

    /// @brief Clarity's base: active when Clarity is not zero.
    ///
    /// Clarity reads both its blurred grid and the grid unblurred, whose
    /// difference is its band. A positive Dehaze reads the grid unblurred too.
    PresenceBase coarse{};

    /// @brief Dehaze's base, on the coarse cells: active when Dehaze is not zero.
    ///
    /// For a positive Dehaze, the floor of the cells (their opening by a
    /// window, lightly blurred but never below the opening), measured against
    /// the cells unblurred; for a negative one, their mean (broadly blurred,
    /// no window).
    PresenceBase haze{};

    /// @brief Whether any control does anything, and so whether the context is computed.
    [[nodiscard]] bool active() const noexcept {
        return fine.active() || coarse.active() || haze.active();
    }

    /// @brief Gives the cell of the coarse grid Clarity and Dehaze share; zero when neither is on.
    [[nodiscard]] std::uint32_t coarseReduction() const noexcept {
        return coarse.active() ? coarse.reduction : haze.reduction;
    }

    friend bool operator==(const PresencePlan&, const PresencePlan&) = default;
};

/// @brief Groups the fields of a Presence plan that the context depends on.
///
/// The context is a function of the pixels at the Denoise boundary and these
/// (Dehaze's sign through @ref PresencePlan::haze's window):
/// what a cache of it beside a ::arraw::Stage::Denoise checkpoint would compare.
[[nodiscard]] inline auto presenceContextFieldsOf(const PresencePlan& plan) {
    return std::tie(plan.lumaRow, plan.fine, plan.coarse, plan.haze);
}

/// @brief Resolves the Presence settings for one source.
/// @param settings What the photographer set.
/// @param encoding Encoding of the source's pixels, which gives the luminance row.
/// @param pixelScale Sensor pixels per source pixel (::arraw::ImageBuffer::pixelScale).
/// @param sourceSize Size of the source, whose long edge Clarity's radius is relative to.
/// @return The block; the default when every control is zero.
/// @throws std::invalid_argument if a setting is not finite, @p pixelScale is
/// not finite and above zero, or a control is asked of an encoding development
/// cannot start from.
[[nodiscard]] PresencePlan presencePlanFor(const PresenceSettings& settings,
                                           const ColorEncoding& encoding, double pixelScale,
                                           ImageSize sourceSize);

/// @brief Gives how far a source pixel's context reaches, in source pixels, for a region render.
///
/// A render restricted to part of the source would need this margin around
/// the part's footprint for its context to be the whole render's: each active
/// base's window and blur radius, a cell for the bilinear read and the cell's own block,
/// in source pixels; the grids must also stay aligned to the source's origin
/// (ADR 025's future work, as ::arraw::denoiseReach). Zero when Presence is off.
[[nodiscard]] std::uint32_t presenceReach(const PresencePlan& plan);

/// @brief The blurred log luminance of one base, cell by cell.
struct PresenceGrid {
    std::uint32_t width = 0;     ///< Cells across.
    std::uint32_t height = 0;    ///< Cells down.
    std::uint32_t reduction = 0; ///< Source pixels per cell side.
    std::vector<float> cells;    ///< Log2 luminance, row by row.
};

/// @brief The Presence context: the side image the pointwise chain reads, one grid per base.
struct PresenceContext {
    PresenceGrid fine;   ///< Texture's base; empty when Texture is zero.
    PresenceGrid coarse; ///< Clarity's base; empty when Clarity is zero.
    /// @brief The coarse grid unblurred: Clarity's band's top and what a positive Dehaze measures
    /// its floor's share against; empty when neither is on.
    PresenceGrid coarseCells;
    PresenceGrid haze; ///< Dehaze's base; empty when Dehaze is zero.
};

/// @brief What the pointwise chain knows about a pixel besides its colour (ADR 011's context).
///
/// The grids of the Presence context, read bilinearly at the pixel's
/// coordinate, in log2 luminance. Zero, and not read, for a grid that is off.
/// There is no default context for a plan with Presence on: a caller without
/// one uses the overloads of ::arraw::developPixel that take none.
struct PixelContext {
    float fineBase = 0.0F;   ///< Texture's base at the pixel.
    float coarseBase = 0.0F; ///< Clarity's base at the pixel.
    float coarseCell = 0.0F; ///< The unblurred coarse grid at the pixel (Clarity, positive Dehaze).
    float hazeBase = 0.0F;   ///< Dehaze's floor (or mean) at the pixel.
};

/// @brief Gives the size of a base's grid for a source.
[[nodiscard]] ImageSize presenceGridSize(const PresenceBase& base, ImageSize source);

/// @brief Computes the Presence context of the pointwise pass's input.
///
/// For each active base: each cell is the log2 of the mean luminance of the
/// `reduction x reduction` block of pixels it covers (the last row and column
/// average what they have), each luminance through the plan's as-shot row and
/// ::arraw::boundedLuminance; Dehaze's floor then opens the grid by an octagon,
/// the minimum across, down and along both diagonals, then the maximum along
/// the same four (::arraw::octagonOf), and reconstructs the opening by its
/// steps, each the maximum over a cell's 3x3 neighbourhood bounded by the cell
/// unopened; the grid is then blurred by a normalised Gaussian across, then
/// down, edges clamped, and a floor kept at least at its opening. Clarity and
/// Dehaze share the coarse cells, which are kept unblurred too for Clarity and
/// a positive Dehaze. Mirrors `presence_filter.frag`.
/// @param input The source after noise reduction, in any layout; read only.
/// @param plan Active plan.
[[nodiscard]] PresenceContext presenceContextOf(const ImageBuffer& input, const PresencePlan& plan);

/// @brief Gives the relative cost of each loop ::arraw::presenceContextOf runs, in the order it
/// runs them.
///
/// Measured wall time on a release build, in nanoseconds, so that the sum is
/// also the context's share of a render (ADR 042). Empty when Presence is off.
/// @param plan Plan the context is computed for.
/// @param size Size of the input.
[[nodiscard]] std::vector<double> presenceLoopWeights(const PresencePlan& plan, ImageSize size);

/// @brief Reads a context at the pixels of the image it was made from, row by row.
///
/// The bilinear read at `u = (x + 0.5) / reduction - 0.5`, clamped to the grid
/// (::arraw::gridTap), rows first, as the Denoise pass reads its grid.
class PresenceSampler {
public:
    /// @brief Prepares to read a context over an image of a size.
    /// @param context Context to read; must outlive the sampler.
    /// @param size Size of the image it was made from.
    PresenceSampler(const PresenceContext& context, ImageSize size);

    /// @brief Moves to a row.
    void setRow(std::uint32_t y);

    /// @brief Gives the context at a column of the current row.
    [[nodiscard]] PixelContext at(std::uint32_t x) const;

private:
    const PresenceContext& context_;
    std::vector<GridTap> fineColumns_;
    std::vector<GridTap> coarseColumns_;
    GridTap fineRow_{};
    GridTap coarseRow_{};
};

/// @brief Gives the log2 luminance the Presence controls measure a source colour by.
///
/// `log2(boundedLuminance(row . colour))` through the plan's as-shot row,
/// before white balance and exposure, so that the detail it gives against the
/// context is the same whatever they are. A NaN luminance measures as the
/// floor, an infinite one as the ceiling.
/// Mirrored by `presenceLogLuminance` in `src/gpu/shaders/develop.frag`.
[[nodiscard]] inline float presenceLogLuminance(const PresencePlan& plan, Colour colour) {
    const float luminance =
        plan.lumaRow[0] * colour[0] + plan.lumaRow[1] * colour[1] + plan.lumaRow[2] * colour[2];
    return std::log2(boundedLuminance(luminance));
}

/// @brief Limits a detail softly: `d / (1 + |d| / limit)`, never beyond the limit.
///
/// Slope one at zero, so small detail is untouched, and a hard edge's detail
/// tends to the limit rather than growing with the edge.
/// Mirrored by `softLimit` in `src/gpu/shaders/develop.frag`.
[[nodiscard]] inline float softLimit(float detail, float limit) {
    return detail / (1.0F + std::abs(detail) / limit);
}

/// @brief Applies Texture, Clarity and Dehaze to a toned colour.
///
/// Texture and Clarity change the colour by one gain, in stops,
/// hue-preserving: `texture * softLimit(L - fine, 0.5)`, with `L` the pixel's
/// log luminance (::arraw::presenceLogLuminance of its source colour), plus
/// `clarity * midtones(v) * softLimit(cells - coarse, 1)`, Clarity's band
/// between the unblurred coarse grid and its blur, with `v` the perceptual
/// luminance. Dehaze works on the toned luminance `Y`, kept by `open = 1 -
/// smoothstep(0.75, 1.25, Y)` from what is nearly white. A positive Dehaze
/// takes off a share of the floor around the pixel: `s = 2^min(haze - cells,
/// 0)` is the floor's share of the unblurred coarse grid at the pixel (not of
/// the pixel, so detail finer than a cell is scaled smoothly rather than
/// expanded), and the colour is scaled by `1 - dehaze * 0.6 * s * open`, which
/// subtracts the floor in proportion above a cell's scale and never crosses
/// black. A negative one adds the neutral veil `|dehaze| * 0.4 * open * m`,
/// with `m` the surroundings' mean in the colour's scale, and nothing where
/// `open` is zero. Last,
/// Dehaze's chroma: Saturation of `dehaze * 0.16` times the veil's share
/// (ADR 041). A colour with no luminance is returned as it is.
/// @param plan Resolved Presence, whose bases say which controls run.
/// @param amounts What the controls ask for at the pixel.
/// @param colour Colour after Basic Tone, in the working encoding.
/// @param logLuminance ::arraw::presenceLogLuminance of the pixel's source colour.
/// @param context The grids at the pixel; not read for a control that is zero.
/// @return The colour; itself when nothing is active.
///
/// Mirrored by `applyPresence` in `src/gpu/shaders/develop.frag`.
[[nodiscard]] Colour applyPresence(const PresencePlan& plan, const PresenceAmounts& amounts,
                                   Colour colour, float logLuminance, const PixelContext& context);

/// @brief Applies Texture, Clarity and Dehaze with the plan's own amounts.
///
/// The global case: every pixel has the amounts of the whole photograph.
/// @copydetails applyPresence(const PresencePlan&, const PresenceAmounts&, Colour, float, const
/// PixelContext&)
[[nodiscard]] inline Colour applyPresence(const PresencePlan& plan, Colour colour,
                                          float logLuminance, const PixelContext& context) {
    return applyPresence(plan, plan.amounts, colour, logLuminance, context);
}

} // namespace arraw

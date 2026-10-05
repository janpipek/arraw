#pragma once

#include "GrainModels.h"

#include <Develop.h>
#include <ImageBuffer.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace arraw {

struct GeometryPlan;
struct ProcessingPlan;
struct ToneCurvePlan;

/// @brief Intermediate value the pointwise shader writes instead of its result.
///
/// ADR 011's "branch on a uniform": when a GPU and a CPU development disagree,
/// comparing each stage says which of the matrix, the gain or the tone chain
/// is responsible. Development itself always asks for ::Developed. The values
/// are stable identifiers shared with the shader, not the order of the
/// pipeline: the tone curves (5) run before the shoulder (4).
enum class PointwiseProbe : std::uint32_t {
    Developed = 0,     ///< The whole chain: what development writes.
    AfterMatrix = 1,   ///< After the source-to-working transform.
    AfterExposure = 2, ///< After the exposure gain.
    AfterTone = 3,     ///< After the tone controls, before the tone curves: the curve input tap.
    AfterShoulder = 4, ///< After the shoulder, before the colour controls.
    AfterCurves = 5,   ///< After the tone curves, before the shoulder.
};

/// @brief Gives the probe that makes the pointwise shader write a tap.
///
/// ADR 011's taps on the GPU are probes: the shader branches on the uniform
/// and writes the colour at that position instead of the developed one, in
/// linear light, exactly as ::arraw::developToTap does on the CPU.
/// @param tap Tap to write.
/// @return ::arraw::PointwiseProbe::AfterTone for ::arraw::Tap::CurveInput: the
/// stop after Basic Tone and before the curves.
/// @throws std::invalid_argument if @p tap is not a tap.
[[nodiscard]] PointwiseProbe probeFor(Tap tap);

/// @brief Tells whether a probe's result depends on the tone curves.
///
/// The shader order lives here, beside the enum: a probe that stops before the
/// curves never reads their table, so the table need not be uploaded for it.
/// @param probe Probe to ask about.
/// @return `true` for every probe at or after the tone curves; `false` for those before.
[[nodiscard]] bool probeReadsCurves(PointwiseProbe probe);

/// @brief The pointwise chain's uniform block, byte for byte as std140 lays it out.
///
/// The shader data contract of `src/gpu/shaders/develop.frag`, whose
/// `Pointwise` block declares the same members in the same order. Filled from a
/// ::arraw::ProcessingPlan by ::arraw::packPointwise, never copied from one: the
/// plan holds `bool`, `std::optional` and padding, none of which has a portable
/// shader layout (GPU implementation plan, "Shader data contract").
///
/// Every scalar member is four bytes and the matrix rows and the band sets are
/// `vec4`s, so std140 adds no padding the declaration order does not already
/// show; the assertions below the struct hold the offsets to that. A band set
/// is eight floats, which std140 holds as an array of two `vec4`s: the same
/// bytes as the plan's `std::array<float, 8>`.
struct GpuPointwiseBlock {
    /// @brief Rows of the source-to-working matrix, each padded to a `vec4`.
    std::array<float, 12> toWorking{};

    /// @brief Linear gain of the Exposure setting.
    float exposureGain = 1.0F;

    /// @brief Contrast exponent about the grey pivot.
    float contrastSlope = 1.0F;

    /// @brief Multiplier that returns grey to the pivot after the exponent.
    float contrastScale = 1.0F;

    /// @brief Regional shifts, in the perceptual coordinate.
    float shadowShift = 0.0F;
    float highlightShift = 0.0F;
    float blackShift = 0.0F;
    float whiteShift = 0.0F;

    /// @brief Luminance the shoulder bends at; zero when there is no shoulder.
    ///
    /// Never infinite: some drivers flush infinities in uniforms or compile
    /// comparisons against them oddly, so "no shoulder" travels as
    /// @ref rollsHighlights instead.
    float shoulderKnee = 0.0F;

    /// @brief Whether the tone controls shape the scale: 0 or 1.
    ///
    /// `uint` rather than `bool`, which has no portable std140 layout.
    std::uint32_t shapesTone = 0;

    /// @brief Whether the shoulder bends highlights at all: 0 or 1.
    std::uint32_t rollsHighlights = 0;

    /// @brief Intermediate to write instead of the result; see ::arraw::PointwiseProbe.
    std::uint32_t probe = static_cast<std::uint32_t>(PointwiseProbe::Developed);

    /// @brief Whether the photograph is made grey, replacing the colour controls: 0 or 1.
    std::uint32_t convertsToGrayscale = 0;

    /// @brief Saturation on the scale of its maths: minus one is grey, plus one doubles chroma.
    float saturation = 0.0F;

    /// @brief Vibrance on the same scale.
    float vibrance = 0.0F;

    /// @brief Whether Saturation changes anything: 0 or 1.
    std::uint32_t adjustsSaturation = 0;

    /// @brief Whether Vibrance changes anything: 0 or 1.
    std::uint32_t adjustsVibrance = 0;

    /// @brief Whether any HSL band changes anything: 0 or 1.
    std::uint32_t adjustsHsl = 0;

    /// @brief Whether the luma curve changes anything: 0 or 1.
    ///
    /// The four curves' tables travel in a texture (::arraw::packToneCurves);
    /// a flag that is 0 keeps the shader from reading it.
    std::uint32_t curvesLuma = 0;

    /// @brief Whether the red curve changes anything: 0 or 1.
    std::uint32_t curvesRed = 0;

    /// @brief Whether the green curve changes anything: 0 or 1.
    std::uint32_t curvesGreen = 0;

    /// @brief Whether the blue curve changes anything: 0 or 1.
    std::uint32_t curvesBlue = 0;

    /// @brief Rounds the scalars up to the `vec4` boundary the band sets start on.
    std::array<std::uint32_t, 3> padding{};

    /// @brief Hue shift of each band, as two `vec4`s.
    std::array<float, 8> hueShift{};

    /// @brief Saturation shift of each band, as two `vec4`s.
    std::array<float, 8> bandSaturation{};

    /// @brief Luminance shift of each band, as two `vec4`s.
    std::array<float, 8> bandLuminance{};

    /// @brief Weight of each band in the grey, as two `vec4`s.
    std::array<float, 8> grayMix{};

    /// @brief Whether Colour Grading tints anything: 0 or 1.
    std::uint32_t grades = 0;

    /// @brief How far Balance moves the tonal position, in the perceptual coordinate.
    float gradeBalanceShift = 0.0F;

    /// @brief Width of each zone's bell over the tonal position.
    float gradeZoneWidth = 1.0F;

    /// @brief Rounds the grade's scalars up to the `vec4` boundary.
    std::uint32_t gradePadding = 0;

    /// @brief Oklab offsets of the Shadows and Midtones zones: `(a, b, a, b)`, one `vec4`.
    std::array<float, 4> gradeShadowMidtoneTint{};

    /// @brief Oklab offset of the Highlights zone in `xy`; `zw` unused.
    std::array<float, 4> gradeHighlightTint{};
};

static_assert(offsetof(GpuPointwiseBlock, toWorking) == 0);
static_assert(offsetof(GpuPointwiseBlock, exposureGain) == 48);
static_assert(offsetof(GpuPointwiseBlock, contrastSlope) == 52);
static_assert(offsetof(GpuPointwiseBlock, contrastScale) == 56);
static_assert(offsetof(GpuPointwiseBlock, shadowShift) == 60);
static_assert(offsetof(GpuPointwiseBlock, highlightShift) == 64);
static_assert(offsetof(GpuPointwiseBlock, blackShift) == 68);
static_assert(offsetof(GpuPointwiseBlock, whiteShift) == 72);
static_assert(offsetof(GpuPointwiseBlock, shoulderKnee) == 76);
static_assert(offsetof(GpuPointwiseBlock, shapesTone) == 80);
static_assert(offsetof(GpuPointwiseBlock, rollsHighlights) == 84);
static_assert(offsetof(GpuPointwiseBlock, probe) == 88);
static_assert(offsetof(GpuPointwiseBlock, convertsToGrayscale) == 92);
static_assert(offsetof(GpuPointwiseBlock, saturation) == 96);
static_assert(offsetof(GpuPointwiseBlock, vibrance) == 100);
static_assert(offsetof(GpuPointwiseBlock, adjustsSaturation) == 104);
static_assert(offsetof(GpuPointwiseBlock, adjustsVibrance) == 108);
static_assert(offsetof(GpuPointwiseBlock, adjustsHsl) == 112);
static_assert(offsetof(GpuPointwiseBlock, curvesLuma) == 116);
static_assert(offsetof(GpuPointwiseBlock, curvesRed) == 120);
static_assert(offsetof(GpuPointwiseBlock, curvesGreen) == 124);
static_assert(offsetof(GpuPointwiseBlock, curvesBlue) == 128);
static_assert(offsetof(GpuPointwiseBlock, padding) == 132);
static_assert(offsetof(GpuPointwiseBlock, hueShift) == 144);
static_assert(offsetof(GpuPointwiseBlock, bandSaturation) == 176);
static_assert(offsetof(GpuPointwiseBlock, bandLuminance) == 208);
static_assert(offsetof(GpuPointwiseBlock, grayMix) == 240);
static_assert(offsetof(GpuPointwiseBlock, grades) == 272);
static_assert(offsetof(GpuPointwiseBlock, gradeBalanceShift) == 276);
static_assert(offsetof(GpuPointwiseBlock, gradeZoneWidth) == 280);
static_assert(offsetof(GpuPointwiseBlock, gradePadding) == 284);
static_assert(offsetof(GpuPointwiseBlock, gradeShadowMidtoneTint) == 288);
static_assert(offsetof(GpuPointwiseBlock, gradeHighlightTint) == 304);
static_assert(sizeof(GpuPointwiseBlock) == 320);

/// @brief Widest output, in pixels per side, that the geometry block can address exactly.
///
/// See ::arraw::GpuGeometryBlock: the shader's products are exact only while
/// `2 * x + 1` fits in 15 bits. Also the largest texture Vulkan implementations
/// commonly promise (lavapipe's `maxImageDimension2D`).
inline constexpr std::uint32_t maxGeometryOutputExtent = 1U << 14;

/// @brief The geometry pass's uniform block, byte for byte as std140 lays it out.
///
/// The shader data contract of `src/gpu/shaders/geometry.frag`. Rather than
/// the crop and the matrix, it carries the one affine map they compose to:
/// output pixel `(x, y)`, taken at its centre, samples the developed image at
///
///     origin + (x + 0.5) * columnStep + (y + 0.5) * rowStep
///
/// in source edge units, which is what ::arraw::applyGeometry computes per
/// pixel through GeometryPlan::toSource.
///
/// No absolute position is ever held in one float: at 6000 pixels a float
/// position is quantised to 5e-4 pixel, which shows as tens of 16-bit codes at
/// a sharp edge. Instead the map is composed on the CPU in double and split:
///
///  - each step is `high + low`, where `high` is the step rounded to 9
///    significant bits. `(x + 0.5)` is `(2x + 1) / 2` with `2x + 1 < 2^15` for
///    `x < 2^14` (::arraw::maxGeometryOutputExtent), so `(x + 0.5) * high` is an
///    integer of at most 15 bits times a 9-bit mantissa: at most 24 significant
///    bits, exactly representable in float. The shader then splits that exact
///    product into its whole and fractional parts, which is exact too.
///  - `low` is what rounding left, at most 2^-10 of the step, so its products
///    are small (at most about 16 pixels at the widest output) and their float
///    rounding costs about 1e-6 pixel.
///  - the origin is a whole part (`int32`) and a fractional part in [0, 1).
///
/// The whole parts are summed as integers, the fractions and low products as
/// floats, and the result renormalised, so the fractional position that sets the
/// blend weights keeps float precision whatever the source size.
///
/// Exact quarter-turns with pixel-aligned crops have integral steps (`high`
/// integral, `low` zero) and a half-integral origin, so the fraction is exactly
/// one half and those copy samples bit for bit without a special case.
struct GpuGeometryBlock {
    /// @brief Whole part of the source position of the output's top-left edge.
    std::array<std::int32_t, 2> originWhole{};

    /// @brief Fractional part of that position, in [0, 1).
    std::array<float, 2> originFraction{};

    /// @brief Source displacement of one output column, rounded to 9 significant bits.
    std::array<float, 2> columnStepHigh{1.0F, 0.0F};

    /// @brief Source displacement of one output row, rounded to 9 significant bits.
    std::array<float, 2> rowStepHigh{0.0F, 1.0F};

    /// @brief What rounding left of the column step: the step is high plus low.
    std::array<float, 2> columnStepLow{};

    /// @brief What rounding left of the row step: the step is high plus low.
    std::array<float, 2> rowStepLow{};

    /// @brief Developed image dimensions, for clamping to its edge.
    std::array<std::uint32_t, 2> sourceSize{};

    /// @brief Output dimensions, the size of the pass's render target.
    std::array<std::uint32_t, 2> outputSize{};
};

static_assert(offsetof(GpuGeometryBlock, originWhole) == 0);
static_assert(offsetof(GpuGeometryBlock, originFraction) == 8);
static_assert(offsetof(GpuGeometryBlock, columnStepHigh) == 16);
static_assert(offsetof(GpuGeometryBlock, rowStepHigh) == 24);
static_assert(offsetof(GpuGeometryBlock, columnStepLow) == 32);
static_assert(offsetof(GpuGeometryBlock, rowStepLow) == 40);
static_assert(offsetof(GpuGeometryBlock, sourceSize) == 48);
static_assert(offsetof(GpuGeometryBlock, outputSize) == 56);
static_assert(sizeof(GpuGeometryBlock) == 64);

/// @brief Which of the three intermediates a ::arraw::GpuPass::ResizeAcross render writes.
///
/// The vertical pass needs more from the horizontal one than its pixels: the
/// range of visible colours in each window and whether it held a pixel that was
/// not opaque (see `src/core/Resample.cpp`). A render has one output, so the
/// horizontal shader is run once per plane.
enum class ResizePlane : std::uint32_t {
    Sums = 0, ///< Premultiplied RGBA after the rule against ringing below black.
    Low = 1,  ///< Lowest visible colour per channel in rgb; in a, 1 if the window was translucent.
    High = 2, ///< Highest visible colour per channel in rgb; a is unused.
};

/// @brief Number of ::arraw::ResizePlane values.
inline constexpr std::size_t resizePlaneCount = 3;

/// @brief The resize passes' uniform block, byte for byte as std140 lays it out.
///
/// The shader data contract of `src/gpu/shaders/resize_across.frag` and
/// `resize_down.frag`, whose `Resize` blocks declare the same members in the
/// same order.
struct GpuResizeBlock {
    /// @brief Plane the horizontal pass writes, as a ::arraw::ResizePlane; unused by the vertical
    /// one.
    std::uint32_t plane = 0;

    /// @brief Length of the region along the resized axis, for clamping tap indices to it.
    std::uint32_t inputLength = 0;

    /// @brief Column and row of the source's first pixel that is resized, for the horizontal pass.
    ///
    /// The region a render cuts out (ADR 025): tap indices are clamped to the
    /// region's own length and then moved by this, so an edge repeats the
    /// region's edge pixel as it does on the CPU after cutting. The row is
    /// where output row 0 reads. Zero for the whole image, and unused by the
    /// vertical pass, which reads what the horizontal one wrote.
    std::array<std::uint32_t, 2> offset{};
};

static_assert(offsetof(GpuResizeBlock, plane) == 0);
static_assert(offsetof(GpuResizeBlock, inputLength) == 4);
static_assert(offsetof(GpuResizeBlock, offset) == 8);
static_assert(sizeof(GpuResizeBlock) == 16);

/// @brief One grain lattice in the Effects pass's uniform block, as std140 lays out a struct.
///
/// ::arraw::GrainLayer, member for member; the shader's `GrainLayer` struct
/// declares the same members in the same order.
struct GpuGrainLayer {
    /// @brief Lattice cell holding output pixel (0, 0)'s centre.
    std::array<std::int32_t, 2> cell{};

    /// @brief Where in that cell the centre lies.
    std::array<float, 2> fraction{};

    /// @brief Lattice cells one output pixel spans along each axis.
    std::array<float, 2> delta{};

    /// @brief Multiplier of the layer's noise; zero leaves it out.
    float weight = 0.0F;

    /// @brief Seed of its lattice values.
    std::uint32_t seed = 0;
};

static_assert(offsetof(GpuGrainLayer, cell) == 0);
static_assert(offsetof(GpuGrainLayer, fraction) == 8);
static_assert(offsetof(GpuGrainLayer, delta) == 16);
static_assert(offsetof(GpuGrainLayer, weight) == 24);
static_assert(offsetof(GpuGrainLayer, seed) == 28);
static_assert(sizeof(GpuGrainLayer) == 32);

/// @brief The Effects pass's uniform block, byte for byte as std140 lays it out.
///
/// The shader data contract of `src/gpu/shaders/effects.frag`, whose `Effects`
/// block declares the same members in the same order. Output pixel `(x, y)`,
/// at its centre, lies at `origin + (x + 0.5, y + 0.5) * step` in fractions of
/// the crop frame (::arraw::FrameMapping, worked out in double on the host and
/// narrowed here). Float is ample for the vignette: a position is good to
/// about 1e-7 of the frame. Grain does not use it: its lattices are placed on
/// the output pixels on the host (::arraw::GrainPlacement), so that the shader
/// only works with small numbers at any zoom.
struct GpuEffectsBlock {
    /// @brief Crop-frame position of output pixel (0, 0)'s top-left corner.
    std::array<float, 2> origin{};

    /// @brief Crop-frame fraction one output pixel covers along each axis.
    std::array<float, 2> step{1.0F, 1.0F};

    /// @brief Whether the vignette changes anything: 0 or 1.
    std::uint32_t vignettes = 0;

    /// @brief Whether it lightens the edges rather than darkening them: 0 or 1.
    std::uint32_t vignetteLightens = 0;

    /// @brief Whether its falloff is a hard edge: 0 or 1.
    std::uint32_t vignetteHardEdge = 0;

    /// @brief Exposure change at the full falloff, in stops; see ::arraw::VignettePlan.
    float vignetteStops = 0.0F;

    /// @brief Radius at which the falloff begins, in corner radii.
    float vignetteInner = 0.0F;

    /// @brief Radius at which it is complete.
    float vignetteOuter = 1.0F;

    /// @brief Whether grain is drawn: 0 or 1.
    std::uint32_t grains = 0;

    /// @brief Which model draws it: the ::arraw::GrainModel value.
    std::uint32_t grainModel = 0;

    /// @brief The model's lattices on this render.
    std::array<GpuGrainLayer, grainLayerCount> grainLayers{};
};

static_assert(offsetof(GpuEffectsBlock, origin) == 0);
static_assert(offsetof(GpuEffectsBlock, step) == 8);
static_assert(offsetof(GpuEffectsBlock, vignettes) == 16);
static_assert(offsetof(GpuEffectsBlock, vignetteLightens) == 20);
static_assert(offsetof(GpuEffectsBlock, vignetteHardEdge) == 24);
static_assert(offsetof(GpuEffectsBlock, vignetteStops) == 28);
static_assert(offsetof(GpuEffectsBlock, vignetteInner) == 32);
static_assert(offsetof(GpuEffectsBlock, vignetteOuter) == 36);
static_assert(offsetof(GpuEffectsBlock, grains) == 40);
static_assert(offsetof(GpuEffectsBlock, grainModel) == 44);
static_assert(offsetof(GpuEffectsBlock, grainLayers) == 48);
static_assert(sizeof(GpuEffectsBlock) == 176);

/// @brief Fills the pointwise block from a resolved plan.
/// @param plan Plan to pack; only its pointwise fields are read.
/// @param probe Intermediate the shader should write instead of its result.
/// @return The block, ready to be copied into a uniform buffer.
[[nodiscard]] GpuPointwiseBlock packPointwise(const ProcessingPlan& plan,
                                              PointwiseProbe probe = PointwiseProbe::Developed);

/// @brief Fills the effects block from a resolved plan.
/// @param plan Plan to pack; its effects, geometry and resize are read.
/// @return The block, ready to be copied into a uniform buffer.
/// @pre @p plan has a geometry and a resize (see ::arraw::frameMappingOf).
[[nodiscard]] GpuEffectsBlock packEffects(const ProcessingPlan& plan);

/// @brief Packs the four tone curves of a plan as an image the pointwise shader reads.
///
/// Texel `i` holds entry `i` of each table: red the luma curve, green the red
/// curve, blue the green curve and alpha the blue curve. Inactive curves are
/// zeros, which the shader never reads. The result is labelled RGBA float in
/// the working encoding only so that it can be uploaded; it holds curve
/// values, not colour.
/// @param curves Resolved curves to pack.
/// @return An image ::arraw::toneCurveSamples pixels wide and one tall.
[[nodiscard]] ImageBuffer packToneCurves(const ToneCurvePlan& curves);

/// @brief Fills the geometry block from a resolved geometry.
/// @param plan Geometry to pack.
/// @return The block, ready to be copied into a uniform buffer.
/// @throws std::invalid_argument if the output is wider or taller than
/// ::arraw::maxGeometryOutputExtent, beyond which the shader is not exact.
[[nodiscard]] GpuGeometryBlock packGeometry(const GeometryPlan& plan);

/// @brief Packs the weights of one axis of a resize as an image the shader reads.
///
/// Row `i` describes output coordinate `i`, from ::arraw::axisWeights, the same
/// weights the CPU resample uses. Texel 0 holds `(first, count, 0, 0)`, the
/// index of the first source pixel (which may lie outside the image) and the
/// number of taps, as floats, which are exact below 2^24. Texels 1 and on hold
/// the weights four to a texel, in order, narrowed to float. The shader clamps
/// tap indices to the image, as the CPU does, so the image is independent of
/// the source's content.
///
/// The result is labelled RGBA float in the working encoding only so that it
/// can be uploaded; it holds weights, not colour.
/// @param in Source length along the axis, at least 1.
/// @param out Result length along the axis, at least 1.
/// @param filter Kernel to evaluate.
/// @return An image `1 + ceil(maxTaps / 4)` pixels wide and @p out pixels tall.
[[nodiscard]] ImageBuffer packResizeWeights(std::uint32_t in, std::uint32_t out,
                                            ResizeFilter filter);

} // namespace arraw

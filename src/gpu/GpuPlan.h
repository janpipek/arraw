#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace arraw {

struct GeometryPlan;
struct ProcessingPlan;

/// @brief Intermediate value the pointwise shader writes instead of its result.
///
/// ADR 011's "branch on a uniform": when a GPU and a CPU development disagree,
/// comparing each stage says which of the matrix, the gain or the tone chain
/// is responsible. Development itself always asks for ::Developed.
enum class PointwiseProbe : std::uint32_t {
    Developed = 0,     ///< The whole chain: what development writes.
    AfterMatrix = 1,   ///< After the source-to-working transform.
    AfterExposure = 2, ///< After the exposure gain.
    AfterTone = 3,     ///< After the tone controls, before the shoulder.
};

/// @brief The pointwise chain's uniform block, byte for byte as std140 lays it out.
///
/// The shader data contract of `src/gpu/shaders/develop.frag`, whose
/// `Pointwise` block declares the same members in the same order. Filled from a
/// ::arraw::ProcessingPlan by ::arraw::packPointwise, never copied from one: the
/// plan holds `bool`, `std::optional` and padding, none of which has a portable
/// shader layout (GPU implementation plan, "Shader data contract").
///
/// Every member is four bytes and the matrix rows are `vec4`s, so std140 adds
/// no padding the declaration order does not already show; the assertions
/// below the struct hold the offsets to that.
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

    /// @brief Rounds the block up to a `vec4` boundary, as std140 does.
    std::uint32_t padding = 0;
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
static_assert(sizeof(GpuPointwiseBlock) == 96);

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

/// @brief Fills the pointwise block from a resolved plan.
/// @param plan Plan to pack; only its pointwise fields are read.
/// @param probe Intermediate the shader should write instead of its result.
/// @return The block, ready to be copied into a uniform buffer.
[[nodiscard]] GpuPointwiseBlock packPointwise(const ProcessingPlan& plan,
                                              PointwiseProbe probe = PointwiseProbe::Developed);

/// @brief Fills the geometry block from a resolved geometry.
/// @param plan Geometry to pack.
/// @return The block, ready to be copied into a uniform buffer.
/// @throws std::invalid_argument if the output is wider or taller than
/// ::arraw::maxGeometryOutputExtent, beyond which the shader is not exact.
[[nodiscard]] GpuGeometryBlock packGeometry(const GeometryPlan& plan);

} // namespace arraw

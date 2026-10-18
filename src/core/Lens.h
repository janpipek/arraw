#pragma once

#include <ImageBuffer.h>
#include <LensCorrection.h>

namespace arraw {

/// @brief The lens block of a plan: the corrections the Lens pass applies (ADR 050, section 2).
///
/// Holds the resolved tables and nothing about size, so plans for a pyramid level, a half-size
/// decode and the full decode are equal and share a rung. With no table the pass does not run and
/// its boundary collapses onto the one before it.
struct LensPlan {
    /// @brief Tables, centre, fill scale and provenance.
    LensCorrection correction;

    /// @brief Whether the pass changes anything.
    [[nodiscard]] bool active() const noexcept {
        return correction.active();
    }

    friend bool operator==(const LensPlan&, const LensPlan&) = default;
};

namespace detail {

/// @brief Interpolation kernel of the lens pass.
enum class LensResampler {
    Bilinear,   ///< 2x2 taps; the cheapest and the softest, kept to measure against.
    CatmullRom, ///< 4x4 taps, bicubic; what the pass uses.
    Lanczos3,   ///< 6x6 taps, the sharpest and the costliest; kept to measure against.
};

/// @brief Applies a lens plan with a chosen kernel.
/// @copydetails ::arraw::applyLens
ImageBuffer applyLens(const ImageBuffer& source, const LensPlan& plan, LensResampler resampler);

} // namespace detail

/// @brief Applies the distortion, lateral chromatic aberration and vignetting of a plan in one
/// pass.
///
/// For each output pixel: the corrected position is mapped to the sensor image (the fill scale,
/// then the distortion table), red and blue move from there by their own scales at the sensor
/// radius, each channel is read at its position with a bicubic Catmull-Rom kernel (taps beyond an
/// edge repeat the edge pixel), and the vignetting gain at the sensor radius multiplies the colour
/// (ADR 050, section 2). The output is the source's size, so the corrected frame keeps its aspect.
///
/// Vignetting alone is a gain and resamples nothing; with no distortion green is copied exactly
/// and only red and blue move. A value above 1 survives, since the working encoding has headroom;
/// a channel whose taps were all at least 0 is clamped at 0 after the kernel's ringing, as in
/// ::arraw::resample. A position a kernel would read at a whole pixel reads that pixel exactly, so
/// a plan whose tables are all 1 returns the source's bits.
///
/// Rows are split into bands across threads; the bits do not depend on the split.
/// @param source Float pixels in the working format, in the sensor frame.
/// @param plan What to apply; an inactive plan returns a copy.
/// @return The corrected frame in the source's encoding, orientation and pixel scale.
/// @throws std::invalid_argument if @p source is not in the working format.
/// @throws ::arraw::Cancelled if the observed operation is cancelled.
[[nodiscard]] ImageBuffer applyLens(const ImageBuffer& source, const LensPlan& plan);

} // namespace arraw

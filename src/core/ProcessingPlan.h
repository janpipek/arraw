#pragma once

#include <ColorEncoding.h>
#include <DevelopSettings.h>

namespace arraw {

/// @brief Everything a photograph's settings imply, worked out once.
///
/// Settings are what a photographer sets; a plan is what the pixels need. The
/// separation is ADR 009's rule — no backend derives anything from
/// ::arraw::DevelopSettings itself — applied to the whole pipeline rather than
/// only to geometry: the CPU reference reads this, and the GPU will upload the
/// same values as its uniform block.
///
/// Settings that are switched off should resolve to a value that costs
/// nothing here rather than to a test inside the per-pixel loop.
struct ProcessingPlan {
    /// @brief Source primaries into the working space, white balance included.
    ///
    /// White balance, the camera matrix, and any change of primaries are all
    /// linear, so they compose into one transform and cost one multiply per
    /// pixel between them.
    Matrix3 toWorking = Matrix3::identity();

    /// @brief Linear gain that the Exposure setting asks for.
    float exposureGain = 1.0F;

    friend bool operator==(const ProcessingPlan&, const ProcessingPlan&) = default;
};

/// @brief Works out what a photograph's settings mean for its pixels.
/// @param encoding Encoding the decoded pixels are in.
/// @param settings Settings to resolve.
/// @return The plan both backends execute.
/// @throws std::invalid_argument if development cannot start from @p encoding,
/// or the settings cannot be resolved against it.
[[nodiscard]] ProcessingPlan planFor(const ColorEncoding& encoding,
                                     const DevelopSettings& settings);

/// @brief Applies the pointwise stages to one colour, in their fixed order.
///
/// The order lives here and nowhere else, so that it can be read in one place
/// and tested without a buffer. A fragment shader's `main` mirrors this
/// sequence by hand, and per-stage comparisons hold the two together (ADR 011).
/// @param plan Resolved settings.
/// @param colour Source colour, in the encoding the plan was built for.
/// @return The developed colour, in the working encoding.
[[nodiscard]] constexpr Colour developPixel(const ProcessingPlan& plan, Colour colour) {
    colour = plan.toWorking * colour;
    colour = {colour[0] * plan.exposureGain, colour[1] * plan.exposureGain,
              colour[2] * plan.exposureGain};
    return colour;
}

} // namespace arraw

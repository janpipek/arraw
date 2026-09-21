#pragma once

#include "ColorSpaces.h"

#include <ColorEncoding.h>
#include <DevelopSettings.h>
#include <Photo.h>

#include <cmath>
#include <limits>

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

    /// @brief Whether any tone control asks for the scale to be shaped.
    ///
    /// Nothing set resolves to `false`, and the chain skips the crossing into
    /// the perceptual coordinate and back — which costs two powers, and would
    /// return a value a hair away from the one it was given. A setting that is
    /// off falls out in the plan rather than as a test inside the loop
    /// (ADR 011).
    bool shapesTone = false;

    /// @brief Exponent the tone scale is raised to about middle grey.
    ///
    /// The Contrast setting resolved into the one number the chain uses: the
    /// perceptual slope at the pivot. One leaves the tone scale alone.
    float contrastSlope = 1.0F;

    /// @brief Luminance at which highlights begin to roll toward white.
    ///
    /// Where the Filmic Highlights amount puts the bend, in linear luminance:
    /// a stronger amount brings the knee down out of white. A photograph asked
    /// for no roll-off resolves to a knee no luminance reaches, so the chain
    /// costs a comparison rather than a branch on a setting (ADR 011).
    float shoulderKnee = std::numeric_limits<float>::infinity();

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

/// @brief Works out what a photograph's document means for its pixels.
///
/// What a render is planned against (ADR 012): the encoding comes from what
/// the file declared and the settings from the document that declared it, so
/// the two cannot arrive from different photographs.
/// @param photo Document to resolve.
/// @return The plan both backends execute.
/// @throws std::invalid_argument if the photograph's encoding or settings
/// cannot be resolved.
[[nodiscard]] ProcessingPlan planFor(const Photo& photo);

/// @brief Perceptual coordinate the tone controls act in.
///
/// Tone shaping happens on `y^(1/2.2)` rather than on scene-linear luminance:
/// linear 0.25 is upper-midtone grey, not the dark quarter, so a control that
/// acted there would put its whole range in the highlights (ADR 010).
/// @param luminance Linear luminance, zero or above.
/// @return The same brightness, perceptually spaced.
[[nodiscard]] constexpr float toPerceptual(float luminance) {
    return std::pow(luminance, 1.0F / 2.2F);
}

/// @brief Returns a perceptual value to scene-linear luminance.
/// @param value Perceptually spaced brightness.
/// @return The linear luminance it stands for.
[[nodiscard]] constexpr float toLinear(float value) {
    return std::pow(value, 2.2F);
}

/// @brief Middle grey in the perceptual coordinate, which Contrast pivots on.
///
/// An eighteen percent grey card, encoded: `0.18^(1/2.2)`, to the nearest
/// float, so that a contrast control leaves the value a photographer metered
/// for exactly where it was.
inline constexpr float greyPivot = 0.45865646F;

/// @brief Shapes one luminance through the tone controls, in their fixed order.
///
/// Contrast is a straight line in log--log through the grey pivot: monotone
/// everywhere, never negative, and continuing smoothly past white rather than
/// flattening into it (ADR 013). The regional controls join it here.
/// @param plan Resolved settings.
/// @param luminance Linear luminance to shape.
/// @return The shaped linear luminance.
[[nodiscard]] constexpr float shapeLuminance(const ProcessingPlan& plan, float luminance) {
    const float value = toPerceptual(luminance);
    return toLinear(greyPivot * std::pow(value / greyPivot, plan.contrastSlope));
}

/// @brief Applies the tone controls to a colour, through its luminance.
///
/// Tone shapes brightness and the colour follows by the ratio, so hue and
/// saturation come through untouched and colour work stays in Oklab where
/// ADR 010 puts it. A colour with no brightness has no ratio to scale by, and
/// takes the shaped value neutrally — which is what a lifted black is.
/// @param plan Resolved settings.
/// @param colour Colour in the working encoding.
/// @return The colour with its tone shaped.
[[nodiscard]] constexpr Colour shapeTone(const ProcessingPlan& plan, Colour colour) {
    if (!plan.shapesTone) {
        return colour;
    }

    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    if (!(luminance > 0.0F)) {
        const float lifted = shapeLuminance(plan, 0.0F);
        return {lifted, lifted, lifted};
    }

    const float ratio = shapeLuminance(plan, luminance) / luminance;
    return {colour[0] * ratio, colour[1] * ratio, colour[2] * ratio};
}

/// @brief Rolls a colour's brightest values toward white rather than clipping.
///
/// The shoulder that ends the chain (ADR 010), as a bend in luminance: below
/// the knee nothing moves, above it the excess is compressed ever harder, so
/// the result approaches white without ever reaching it and two values that
/// would both have clipped stay apart. The bend starts at slope one, so a
/// gradient shows no edge where it begins.
///
/// Colour fades with it. Something genuinely overexposed loses colour as it
/// brightens, so as a value is pulled down its chroma is faded toward the
/// neutral of the same luminance — the "path to white". Brightness is the
/// shoulder's, colour is the fade's: the luminance that comes out is the one
/// the bend asked for either way.
/// @param plan Resolved settings.
/// @param colour Colour in the working encoding, possibly above white.
/// @return The colour with its highlights rolled.
[[nodiscard]] constexpr Colour rollHighlights(const ProcessingPlan& plan, Colour colour) {
    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    if (!(luminance > plan.shoulderKnee)) {
        return colour;
    }

    const float headroom = 1.0F - plan.shoulderKnee;
    const float above = (luminance - plan.shoulderKnee) / headroom;
    const float rolled = plan.shoulderKnee + headroom * (above / (1.0F + above));

    const float ratio = rolled / luminance;
    // Chroma fades as the square of how far the value was pulled down, times
    // its square root: gentle while the roll is gentle, complete by the time
    // the value is being crushed into white.
    const float chroma = ratio * std::sqrt(ratio);
    return {rolled + chroma * (colour[0] * ratio - rolled),
            rolled + chroma * (colour[1] * ratio - rolled),
            rolled + chroma * (colour[2] * ratio - rolled)};
}

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
    colour = shapeTone(plan, colour);
    return rollHighlights(plan, colour);
}

} // namespace arraw

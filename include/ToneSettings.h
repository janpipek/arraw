#pragma once

namespace arraw {

/// @brief Darkest and brightest Exposure arraw models, in EV.
///
/// Named here rather than left to whatever a caller happens to pass; they
/// become rows in the descriptor table of ADR 008 when it exists.
inline constexpr float darkestExposure = -5.0F;

/// @copydoc darkestExposure
inline constexpr float brightestExposure = 5.0F;

/// @brief Flattest and steepest Contrast arraw models.
///
/// A perceptual slope of 0.71 or 1.41 at middle grey; the slider is the
/// exponent's scale rather than the slope itself (ADR 013).
inline constexpr float flattestContrast = -100.0F;

/// @copydoc flattestContrast
inline constexpr float steepestContrast = 100.0F;

/// @brief The range the four regional tone controls share.
///
/// Shadows, Highlights, Blacks and Whites each move their own region of the
/// tone scale by at most a fixed amount, so one range serves all four; how far
/// each reaches is the plan's business (ADR 013).
inline constexpr float weakestToneControl = -100.0F;

/// @copydoc weakestToneControl
inline constexpr float strongestToneControl = 100.0F;

/// @brief Weakest and strongest highlight roll-off arraw models.
///
/// Zero is a true neutral — a photographer who wants a hard clip may have one
/// — rather than a floor that would make the number mean something other than
/// what it says (ADR 010).
inline constexpr float noFilmicHighlights = 0.0F;

/// @copydoc noFilmicHighlights
inline constexpr float fullFilmicHighlights = 100.0F;

/// @brief Photographic tone adjustments in domain units (ADRs 010 and 013).
struct ToneSettings {
    /// @brief Exposure adjustment, in EV.
    float exposure = 0.0F;

    /// @brief How steeply the tone scale rises through middle grey.
    ///
    /// The first of the tone controls and the only global one: it pivots about
    /// the grey card, so that the value a photographer meters for does not
    /// move while everything around it spreads or gathers (ADR 013). It
    /// deliberately pushes bright values above white, which the highlight
    /// roll-off then catches.
    float contrast = 0.0F;

    /// @brief How much the dark tones are lifted or deepened.
    ///
    /// A region rather than an end: it fades out at black, which is Blacks'
    /// business, and at the midtones, which are Contrast's (ADR 013).
    float shadows = 0.0F;

    /// @brief How much the bright tones are recovered or pushed up.
    ///
    /// Reaches a little past white, so that recovery can take hold of the
    /// headroom the roll-off is about to compress.
    float highlights = 0.0F;

    /// @brief Where the black point sits.
    float blacks = 0.0F;

    /// @brief Where the white point sits.
    ///
    /// Everything above white moves with it, rather than being crushed into
    /// it: raising the white point is not the same as clipping to it.
    float whites = 0.0F;

    /// @brief How much the brightest values roll toward white, 0 to 100.
    ///
    /// The shoulder that ends the chain (ADR 010). A sensor records a far
    /// wider range than a file can hold, and exposure is a real multiply, so
    /// values above white are ordinary; without a bend at the top they all
    /// become the same flat white, with the hard edge that gives away. The
    /// amount says where the bend starts: gentle catches only what would have
    /// clipped, strong reaches down into the upper midtones.
    ///
    /// Not a switch. Its default is a gentle roll, because most photographs
    /// read better with graceful highlights than with a digital clip, and a
    /// photographer changes how much rather than whether.
    float filmicHighlights = 25.0F;

    friend bool operator==(const ToneSettings&, const ToneSettings&) = default;
};

} // namespace arraw

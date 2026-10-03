#pragma once

#include <ColorEncoding.h>
#include <DevelopState.h>
#include <ImageBuffer.h>

namespace arraw {

/// @brief The colour of a light, as a photographer names it.
///
/// Two numbers, because one is not enough: real light sources sit *near* the
/// line of glowing-hot-object colours rather than on it. @ref kelvin says how
/// far along that line the light is, and @ref tint says how far off it.
struct ColourTemperature {
    /// @brief Temperature of the light, in kelvin.
    ///
    /// Low is warm (candles, tungsten), high is cool (shade, overcast) — the
    /// opposite of how "warm" and "cool" sound, because it names the
    /// temperature of the glowing object, not the mood of the picture.
    float kelvin = 5500.0F;

    /// @brief How far off that line the light sits.
    ///
    /// Positive says the light was greener than a glowing object can be, and
    /// so shifts the photograph toward magenta; negative does the reverse.
    /// Fluorescent and LED light is the usual reason it is not zero, and even
    /// daylight is a little off the line: measured here, D65 reads +9.8.
    float tint = 0.0F;

    friend bool operator==(const ColourTemperature&, const ColourTemperature&) = default;
};

/// @brief Warmest light arraw models, in kelvin.
///
/// The limits are named here rather than left to whatever a test happened to
/// pass. The temperature and tint rows of the descriptor table
/// (SettingDescriptors.h, ADR 008) refer to them.
inline constexpr float warmestKelvin = 2000.0F;

/// @brief Coolest light arraw models, in kelvin.
inline constexpr float coolestKelvin = 12000.0F;

/// @brief Largest departure from the curve arraw models, either side of it.
inline constexpr float tintLimit = 150.0F;

/// @brief Computes the gains that neutralise a light, for one camera.
///
/// Every sensor answers a given light differently, so the same
/// @p temperature gives different gains on different bodies — which is exactly
/// why a temperature, rather than the gains, is what gets stored and copied
/// between photographs.
/// A temperature or tint outside the modelled range is clamped into it rather
/// than refused: this is the processing contract, which ADR 008 requires to
/// hold whatever reached it. Values that are not finite are refused, since no
/// clamping makes them mean anything.
/// @param camera Sensor the gains are for.
/// @param temperature Light to neutralise.
/// @return Per-channel multipliers, normalised so green is 1.
/// @throws std::invalid_argument if @p temperature is not finite, or the
/// camera cannot see that light as a positive colour.
[[nodiscard]] Gains whiteBalanceGains(const CameraNative& camera, ColourTemperature temperature);

/// @brief Rescales gains so green is 1, the convention everywhere in arraw.
///
/// Only the ratios between channels are a white balance; the overall level is
/// Exposure's business, so pinning green keeps the two from arguing.
/// @param gains Multipliers to rescale.
/// @return The same ratios, with green at 1.
[[nodiscard]] Gains withGreenAtOne(Gains gains);

/// @brief Reads back which light a set of gains neutralises.
///
/// The inverse of @ref whiteBalanceGains. Which gains to ask about matters:
/// what the camera *recorded* is what a photographer should be shown, while
/// what the decode *applied* is what the pixels actually went through, and for
/// a file that declared no white balance those are not the same (ADR 007).
/// @param camera Sensor the gains belong to.
/// @param gains Multipliers to interpret.
/// @return The light those gains neutralise.
[[nodiscard]] ColourTemperature temperatureForGains(const CameraNative& camera, Gains gains);

/// @brief Reads back the light the camera says it saw.
///
/// What to show a photographer when a file opens: the scene's own light rather
/// than an arbitrary starting number. For a file that recorded none, this is
/// not what the pixels went through -- see @ref temperatureForGains.
/// @param camera Sensor and the gains its file recorded.
/// @return The light those gains neutralise.
[[nodiscard]] ColourTemperature asShotTemperature(const CameraNative& camera);

/// @brief Default half-width of the window a pick averages, in source pixels.
inline constexpr int defaultPickRadius = 3;

/// @brief Reads which light makes the source neutral around a point of the developed picture.
///
/// The eyedropper of a white balance panel. The point is given where the
/// photographer sees it, in the developed and cropped frame, and is carried
/// back to the decoded pixels through the same geometry a render resolves
/// (orientation, rotation, straighten, flips, crop; ADR 009). The pixels in a
/// window around it are averaged in the camera's own channels, and the answer
/// is the light whose gains turn that average neutral.
///
/// The answer means what a stored temperature means. The decode has already
/// multiplied the channels by @ref CameraNative::appliedMultipliers, so the
/// gains that make the average neutral are a *change* from those; the light
/// returned is the one whose full gains are that change times the applied
/// ones, which is precisely the relation develop resolves a Custom temperature
/// with (ADR 007). Storing the result as Custom therefore turns the picked
/// spot neutral, whatever the settings the photograph is shown with now.
/// @param source Decoded photograph, in a camera's own encoding.
/// @param state Develop state whose geometry frames the point.
/// @param x Horizontal position in the developed frame, 0 at the left edge to 1 at the right.
/// @param y Vertical position in the developed frame, 0 at the top edge to 1 at the bottom.
/// @param radius Half-width of the averaged window in source pixels, clamped at the edges.
/// @return The light, with kelvin and tint held inside the modelled range.
/// @throws std::invalid_argument if @p source is not camera-native, the point lies outside
/// the frame or is not finite, @p radius is negative, or the window has no
/// positive signal in some channel.
[[nodiscard]] ColourTemperature neutralTemperatureAt(const ImageBuffer& source,
                                                     const DevelopState& state, double x, double y,
                                                     int radius = defaultPickRadius);

} // namespace arraw

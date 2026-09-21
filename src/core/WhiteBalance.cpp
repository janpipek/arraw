#include "WhiteBalance.h"

#include "ColorSpaces.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

using namespace arraw;

// What white balance has to do, in one paragraph. A photograph of a white wall
// under candlelight has more red in it than one taken in shade — not because
// the wall changed, but because the light did. Correcting that means dividing
// the picture by the colour of the light, which means we need to know that
// colour as three camera-channel numbers. The photographer does not think in
// camera channels, though; they think "this was tungsten, about 3200 K". So
// the job is a translation, and it runs in both directions: a temperature the
// photographer names becomes channel gains, and the gains a camera recorded
// become a temperature to show them.

namespace {

/// @brief One sample of the colours a glowing object takes as it heats up.
///
/// The colours of hot objects — an ember, a filament, the sun — trace a single
/// curve, and a light's temperature is a position along it. This table walks
/// that curve in even steps of *mired*, which is a million divided by the
/// temperature. Mired is used rather than kelvin because equal steps in it are
/// equal-looking steps of colour: 2000 K to 2200 K is a large visible change,
/// 9000 K to 9200 K is almost none.
///
/// @ref slope is the direction of the line crossing the curve at that point,
/// along which every colour looks the same temperature but greener or more
/// magenta. That is what tint moves along. The values are the standard
/// Robertson table (Wyszecki and Stiles), the same one Adobe's DNG tools use,
/// which is why our numbers can mean what `crs:Temperature` means.
struct LocusPoint {
    float mired; ///< One million divided by the temperature in kelvin.
    float u;     ///< Colour of that light, first coordinate.
    float v;     ///< Colour of that light, second coordinate.
    float slope; ///< Direction of the same-temperature line through it.
};

constexpr std::array<LocusPoint, 31> locus{{
    {0.0F, 0.18006F, 0.26352F, -0.24341F},   {10.0F, 0.18066F, 0.26589F, -0.25479F},
    {20.0F, 0.18133F, 0.26846F, -0.26876F},  {30.0F, 0.18208F, 0.27119F, -0.28539F},
    {40.0F, 0.18293F, 0.27407F, -0.30470F},  {50.0F, 0.18388F, 0.27709F, -0.32675F},
    {60.0F, 0.18494F, 0.28021F, -0.35156F},  {70.0F, 0.18611F, 0.28342F, -0.37915F},
    {80.0F, 0.18740F, 0.28668F, -0.40955F},  {90.0F, 0.18880F, 0.28997F, -0.44278F},
    {100.0F, 0.19032F, 0.29326F, -0.47888F}, {125.0F, 0.19462F, 0.30141F, -0.58204F},
    {150.0F, 0.19962F, 0.30921F, -0.70471F}, {175.0F, 0.20525F, 0.31647F, -0.84901F},
    {200.0F, 0.21142F, 0.32312F, -1.0182F},  {225.0F, 0.21807F, 0.32909F, -1.2168F},
    {250.0F, 0.22511F, 0.33439F, -1.4512F},  {275.0F, 0.23247F, 0.33904F, -1.7298F},
    {300.0F, 0.24010F, 0.34308F, -2.0637F},  {325.0F, 0.24792F, 0.34655F, -2.4681F},
    {350.0F, 0.25591F, 0.34951F, -2.9641F},  {375.0F, 0.26400F, 0.35200F, -3.5814F},
    {400.0F, 0.27218F, 0.35407F, -4.3633F},  {425.0F, 0.28039F, 0.35577F, -5.3762F},
    {450.0F, 0.28863F, 0.35714F, -6.7262F},  {475.0F, 0.29685F, 0.35823F, -8.5955F},
    {500.0F, 0.30505F, 0.35907F, -11.324F},  {525.0F, 0.31320F, 0.35968F, -15.628F},
    {550.0F, 0.32129F, 0.36011F, -23.325F},  {575.0F, 0.32931F, 0.36038F, -40.770F},
    {600.0F, 0.33724F, 0.36051F, -116.45F},
}};

/// @brief How many tint units one step across the curve is worth.
///
/// Arbitrary, in the way a slider's range is arbitrary; it matches Adobe's, so
/// that a tint of 20 means what a photographer expects it to mean. The sign
/// makes a positive tint magenta.
constexpr float tintScale = -3000.0F;

/// @brief A colour with its brightness discarded, in the space the curve lives in.
///
/// Two numbers instead of three, because white balance is only about the
/// *colour* of the light; how bright it was is Exposure's business.
struct Chromaticity {
    float u = 0.0F;
    float v = 0.0F;
};

/// @brief Discards a colour's brightness, keeping where it sits on the chart.
/// @param xyz Colour in CIE XYZ.
/// @return The same colour with brightness removed.
Chromaticity chromaticityOf(std::array<float, 3> xyz) {
    const float denominator = xyz[0] + 15.0F * xyz[1] + 3.0F * xyz[2];
    if (denominator <= 0.0F) {
        throw std::invalid_argument("A colour with no light in it has no temperature");
    }
    return {4.0F * xyz[0] / denominator, 6.0F * xyz[1] / denominator};
}

/// @brief Puts the brightness back, arbitrarily, to get a usable colour again.
/// @param point Where the light sits on the chart.
/// @return The colour in CIE XYZ, scaled so its brightness is 1.
std::array<float, 3> xyzOf(Chromaticity point) {
    const float denominator = point.u - 4.0F * point.v + 2.0F;
    if (denominator == 0.0F) {
        throw std::invalid_argument("That light has no colour in the visible range");
    }
    const float x = 1.5F * point.u / denominator;
    const float y = point.v / denominator;
    if (y <= 0.0F) {
        throw std::invalid_argument("That light has no colour in the visible range");
    }
    return {x / y, 1.0F, (1.0F - x - y) / y};
}

/// @brief Computes the unit direction of a table entry's same-temperature line.
/// @param point Table entry.
/// @return A unit-length direction.
Chromaticity isothermDirection(const LocusPoint& point) {
    const float length = std::sqrt(1.0F + point.slope * point.slope);
    return {1.0F / length, point.slope / length};
}

/// @brief Rescales a direction to unit length.
Chromaticity normalise(Chromaticity direction) {
    const float length = std::sqrt(direction.u * direction.u + direction.v * direction.v);
    if (length == 0.0F) {
        throw std::invalid_argument("A direction of zero length has no direction");
    }
    return {direction.u / length, direction.v / length};
}

/// @brief Finds the colour of a light, given its temperature and tint.
///
/// Walks the table to the two entries the temperature falls between, mixes
/// them, and then slides along the same-temperature line by the tint.
/// @param temperature Light to locate.
/// @return Where that light sits on the chart.
Chromaticity chromaticityFor(ColourTemperature temperature) {
    if (temperature.kelvin <= 0.0F) {
        throw std::invalid_argument("A light's temperature must be above zero");
    }
    const float mired = 1.0e6F / temperature.kelvin;

    std::size_t index = 0;
    while (index < locus.size() - 2 && mired >= locus[index + 1].mired) {
        ++index;
    }
    const LocusPoint& lower = locus[index];
    const LocusPoint& upper = locus[index + 1];

    // How far between the two entries the temperature sits: 1 at the lower, 0
    // at the upper. Even in mired, which is why this is a straight mix.
    const float weight = (upper.mired - mired) / (upper.mired - lower.mired);

    const Chromaticity onCurve{lower.u * weight + upper.u * (1.0F - weight),
                               lower.v * weight + upper.v * (1.0F - weight)};

    const Chromaticity lowerDirection = isothermDirection(lower);
    const Chromaticity upperDirection = isothermDirection(upper);
    const Chromaticity direction =
        normalise({lowerDirection.u * weight + upperDirection.u * (1.0F - weight),
                   lowerDirection.v * weight + upperDirection.v * (1.0F - weight)});

    const float offset = temperature.tint / tintScale;
    return {onCurve.u + direction.u * offset, onCurve.v + direction.v * offset};
}

/// @brief Finds the temperature and tint of a colour: the reverse of the above.
///
/// Walks the same table, watching which side of each same-temperature line the
/// colour falls on. The moment it crosses, the colour lies between that entry
/// and the previous one, and how far it sits off the curve is the tint.
/// @param point Colour of a light.
/// @return Its temperature and tint.
ColourTemperature temperatureOf(Chromaticity point) {
    float previousDistance = 0.0F;
    Chromaticity previousDirection{};

    for (std::size_t index = 1; index < locus.size(); ++index) {
        Chromaticity direction = isothermDirection(locus[index]);
        const float offsetU = point.u - locus[index].u;
        const float offsetV = point.v - locus[index].v;
        float distance = -offsetU * direction.v + offsetV * direction.u;

        const bool crossed = distance <= 0.0F;
        const bool exhausted = index == locus.size() - 1;
        if (!crossed && !exhausted) {
            previousDistance = distance;
            previousDirection = direction;
            continue;
        }

        distance = crossed ? -distance : 0.0F;
        // Where between the two entries the crossing happened. The first entry
        // has no predecessor to mix with, so the colour is taken as sitting on
        // it -- the table's cold end is a limit, not a wall.
        const float weight = index == 1 ? 0.0F : distance / (previousDistance + distance);

        const LocusPoint& lower = locus[index - 1];
        const LocusPoint& upper = locus[index];
        const float mired = lower.mired * weight + upper.mired * (1.0F - weight);

        const Chromaticity onCurve{lower.u * weight + upper.u * (1.0F - weight),
                                   lower.v * weight + upper.v * (1.0F - weight)};
        const Chromaticity blended =
            normalise({previousDirection.u * weight + direction.u * (1.0F - weight),
                       previousDirection.v * weight + direction.v * (1.0F - weight)});

        const float tint =
            ((point.u - onCurve.u) * blended.u + (point.v - onCurve.v) * blended.v) * tintScale;
        return {1.0e6F / mired, tint};
    }

    throw std::invalid_argument("That colour is not near the range of natural light");
}

/// @brief CIE XYZ to the working space, and back.
constexpr Matrix3 xyzToWorking = colorspaces::srgbToWorking * colorspaces::xyzToSrgb;
constexpr Matrix3 workingToXyz = xyzToWorking.inverse();

/// @brief Rebuilds the sensor's true response to colour.
///
/// The stored matrix has been through a normalisation that threw away how
/// unevenly the sensor answers red, green and blue — two quite different
/// sensors can share it. @ref CameraNative::daylightScale holds what was
/// divided out, so multiplying it back in recovers the real thing, which is
/// what a temperature has to be measured against (ADR 007).
///
/// Built in this direction because it is the one the stored values already
/// point in; the other direction is one inversion away.
/// @param camera Sensor to describe.
/// @return The transform from that sensor's channels into CIE XYZ.
Matrix3 cameraToXyz(const CameraNative& camera) {
    for (const float scale : camera.daylightScale) {
        if (scale <= 0.0F) {
            throw std::invalid_argument("The camera's calibration has a channel of zero");
        }
    }
    return workingToXyz * camera.toWorking * Matrix3::scale(camera.daylightScale);
}

/// @brief Brings a light into the range arraw models.
///
/// The processing contract clamps whatever reaches it, so that no pixel maths
/// depends on a caller having done so first (ADR 008). Values that are not
/// finite are a different matter: no limit makes them mean anything.
/// @param temperature Light to bound.
/// @return The same light, inside the modelled range.
ColourTemperature bounded(ColourTemperature temperature) {
    if (!std::isfinite(temperature.kelvin) || !std::isfinite(temperature.tint)) {
        throw std::invalid_argument("A light's temperature and tint must be finite");
    }
    return {std::clamp(temperature.kelvin, warmestKelvin, coolestKelvin),
            std::clamp(temperature.tint, -tintLimit, tintLimit)};
}

} // namespace

Gains arraw::withGreenAtOne(Gains gains) {
    if (gains[1] <= 0.0F) {
        throw std::invalid_argument("A white balance with no green in it is not a white balance");
    }
    return {gains[0] / gains[1], 1.0F, gains[2] / gains[1]};
}

Gains arraw::whiteBalanceGains(const CameraNative& camera, ColourTemperature temperature) {
    // The light, as the sensor would have recorded it...
    const std::array<float, 3> neutral =
        cameraToXyz(camera).inverse() * xyzOf(chromaticityFor(bounded(temperature)));
    for (const float channel : neutral) {
        if (channel <= 0.0F) {
            throw std::invalid_argument("This camera cannot see that light as a colour");
        }
    }
    // ...and the gains that cancel it, which is division, one channel at a time.
    return withGreenAtOne({1.0F / neutral[0], 1.0F / neutral[1], 1.0F / neutral[2]});
}

ColourTemperature arraw::temperatureForGains(const CameraNative& camera, Gains gains) {
    const Gains balanced = withGreenAtOne(gains);
    for (const float channel : balanced) {
        if (!std::isfinite(channel) || channel <= 0.0F) {
            throw std::invalid_argument("White balance gains must be finite and above zero");
        }
    }
    // Undo everything whiteBalanceGains does, in reverse order: gains back to
    // the light the sensor saw, the sensor's channels back to CIE XYZ, and the
    // colour back to a place on the curve.
    const std::array<float, 3> neutral{1.0F / balanced[0], 1.0F / balanced[1], 1.0F / balanced[2]};
    return temperatureOf(chromaticityOf(cameraToXyz(camera) * neutral));
}

ColourTemperature arraw::asShotTemperature(const CameraNative& camera) {
    return temperatureForGains(camera, camera.asShotMultipliers);
}

#include "Develop.h"

#include "ColorSpaces.h"

#include <WhiteBalance.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <variant>

using namespace arraw;

namespace {

/// @brief Converts one stored sample to the unit range development works in.
template <typename Sample> constexpr float toUnit(Sample value) {
    if constexpr (std::is_same_v<Sample, float>) {
        return value;
    } else {
        return static_cast<float>(value) / static_cast<float>(std::numeric_limits<Sample>::max());
    }
}

/// @brief Works out how far the white balance has to move from where it is.
///
/// The decode already multiplied the channels by something — the camera's own
/// reading, or a substitute when the file recorded none. Asking for a light is
/// therefore not asking for gains but for the *difference* between the gains
/// that light wants and the ones already in the pixels. Leaving the mode at
/// As Shot makes that difference exactly one, which is why developing with
/// default settings cannot change a photograph's colour.
/// @param camera Sensor the photograph came from.
/// @param settings Settings to resolve.
/// @return Per-channel multipliers to apply, with green at 1.
Gains whiteBalanceDelta(const CameraNative& camera, const DevelopSettings& settings) {
    if (settings.whiteBalance == WhiteBalanceMode::AsShot) {
        return {1.0F, 1.0F, 1.0F};
    }

    // Moving only the tint leaves the temperature at the camera's reading, and
    // the other way round, so neither slider drags the other with it.
    const ColourTemperature asShot = asShotTemperature(camera);
    const ColourTemperature wanted{settings.temperature.value_or(asShot.kelvin),
                                   settings.tint.value_or(asShot.tint)};

    const Gains wantedGains = whiteBalanceGains(camera, wanted);
    const Gains applied = withGreenAtOne(camera.appliedMultipliers);
    return withGreenAtOne(
        {wantedGains[0] / applied[0], wantedGains[1] / applied[1], wantedGains[2] / applied[2]});
}

/// @brief Resolves the transform out of a source encoding into the working one.
///
/// A camera encoding carries its own, with the white balance composed into it:
/// two matrices multiplied once per photograph rather than two passes over
/// every pixel. The working encoding needs neither. Any other named space is a
/// decoding concern -- import converts what it reads, so one arriving here
/// means a buffer skipped that step.
Matrix3 toWorkingMatrix(const ColorEncoding& encoding, const DevelopSettings& settings) {
    if (const auto* camera = std::get_if<CameraNative>(&encoding)) {
        return camera->toWorking * Matrix3::scale(whiteBalanceDelta(*camera, settings));
    }
    if (isWorkingEncoding(encoding)) {
        if (settings.whiteBalance != WhiteBalanceMode::AsShot) {
            // A temperature in kelvin is measured against a sensor's response.
            // A photograph that arrived already in a standard colour space has
            // no sensor behind it, and gets the incremental setting instead
            // (ADR 008), which is not implemented yet.
            throw std::invalid_argument(
                "A temperature needs a sensor to measure against; this photograph has none");
        }
        return Matrix3::identity();
    }
    throw std::invalid_argument(
        "Development starts from the working encoding or a camera's own primaries");
}

/// @brief Applies the transform and exposure to every pixel of one layout.
///
/// Exposure is a scalar, so it commutes with the colour transform and is
/// folded into the same pass. Alpha is copied rather than scaled: no develop
/// setting produces transparency, and a source that carried some keeps it.
template <typename Sample>
void developSamples(const ImageBuffer& source, ImageBuffer& result, const Matrix3& matrix,
                    float gain) {
    const auto input = source.samples<Sample>();
    const auto output = result.samples<float>();
    const std::size_t channels = channelCount(source.format());
    const auto pixels = static_cast<std::size_t>(source.size().pixelCount());

    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto* in = &input[pixel * channels];
        auto* out = &output[pixel * 4];

        const std::array<float, 3> colour =
            matrix * std::array<float, 3>{toUnit(in[0]), toUnit(in[1]), toUnit(in[2])};
        out[0] = colour[0] * gain;
        out[1] = colour[1] * gain;
        out[2] = colour[2] * gain;
        out[3] = channels == 4 ? toUnit(in[3]) : 1.0F;
    }
}

} // namespace

ImageBuffer arraw::develop(const ImageBuffer& source, const DevelopSettings& settings,
                           const RenderRequest& request) {
    if (request.targetSize.has_value()) {
        throw std::invalid_argument("Rendering to a requested size is not implemented yet");
    }

    const Matrix3 matrix = toWorkingMatrix(source.encoding(), settings);
    const float gain = std::exp2(settings.exposure);

    ImageBuffer result(source.size(), workingFormat, workingEncoding);
    switch (source.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        developSamples<std::uint8_t>(source, result, matrix, gain);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        developSamples<std::uint16_t>(source, result, matrix, gain);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        developSamples<float>(source, result, matrix, gain);
        break;
    }
    return result;
}

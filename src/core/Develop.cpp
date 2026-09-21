#include "Develop.h"

#include "ColorSpaces.h"

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

/// @brief Resolves the transform out of a source encoding into the working one.
///
/// A camera encoding carries its own; the working encoding needs none. Any
/// other named space is a decoding concern: import converts what it reads, so
/// one arriving here means a buffer skipped that step.
Matrix3 toWorkingMatrix(const ColorEncoding& encoding) {
    if (const auto* camera = std::get_if<CameraNative>(&encoding)) {
        return camera->toWorking;
    }
    if (isWorkingEncoding(encoding)) {
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

    const Matrix3 matrix = toWorkingMatrix(source.encoding());
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

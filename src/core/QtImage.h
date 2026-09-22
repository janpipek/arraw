#pragma once

#include <QColorSpace>
#include <QImage>

#include <optional>

#include <ImageBuffer.h>

/// @brief Conversions between arraw's image types and Qt's.
///
/// Everything that knows how an ::arraw::ImageBuffer corresponds to a QImage
/// lives here, so import and export cannot disagree about it. Replacing Qt's
/// codecs one day means replacing this file.
namespace arraw::qtimage {

/// @brief QImage layout holding samples exactly as a buffer layout does.
/// @param format Buffer sample layout to map.
/// @return The matching QImage format, or `QImage::Format_Invalid` when QImage
/// has no layout-compatible equivalent.
[[nodiscard]] QImage::Format toImageFormat(PixelFormat format);

/// @brief Buffer layout holding samples exactly as a QImage layout does.
/// @param format QImage layout to map.
/// @return The matching buffer layout, or `std::nullopt` when arraw has no
/// layout-compatible equivalent.
[[nodiscard]] std::optional<PixelFormat> toPixelFormat(QImage::Format format);

/// @brief Colour space a named encoding stands for.
///
/// Camera-native encodings have no Qt equivalent: their primaries are per-body
/// data, and a buffer carrying them has to pass white balance before anything
/// here can describe it.
/// @param encoding Encoding to resolve.
/// @return The matching colour space.
/// @throws std::invalid_argument if @p encoding is not a recognised value.
[[nodiscard]] QColorSpace toColorSpace(NamedEncoding encoding);

/// @brief Read-only view of a buffer as a QImage, without copying its samples.
///
/// The returned image shares the buffer's storage, so the buffer must outlive
/// it. The image carries no colour space; the caller assigns one if the
/// operation needs it.
/// @param buffer Buffer to view.
/// @return An image sharing the samples, or a null image if the buffer's
/// layout has no QImage equivalent.
/// @throws std::invalid_argument if the dimensions or stride exceed QImage's
/// integer limits.
[[nodiscard]] QImage toImage(const ImageBuffer& buffer);

/// @brief Copies a QImage into a new, tightly packed buffer.
///
/// The image's layout decides the buffer's, so any colour or layout conversion
/// belongs to the caller, before this call.
/// @param image Image to copy; must be in a layout arraw can hold.
/// @param encoding Meaning to record for the sample values.
/// @param orientation Source orientation still to be applied during development.
/// @return A buffer holding a copy of the image's samples.
/// @throws std::invalid_argument if the image is null or its layout has no
/// buffer equivalent.
[[nodiscard]] ImageBuffer toBuffer(const QImage& image, ColorEncoding encoding,
                                   ImageOrientation orientation = ImageOrientation::Normal);

} // namespace arraw::qtimage

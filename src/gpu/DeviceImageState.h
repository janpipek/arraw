#pragma once

#include "DeviceImage.h"

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

#include <utility>

namespace arraw::detail {

/// @brief What a device image is, described without naming the device's API.
///
/// Core may hold a ::arraw::DeviceImage, so this header names no graphics type
/// either: the texture and the transfer that reads it back belong to a subclass
/// defined in the one translation unit that compiles against QRhi. What is here
/// is the part the engine above needs to describe the pixels without touching
/// them.
///
/// Held as `shared_ptr<const>`, so it is immutable once minted, and deliberately
/// neither copyable nor movable: a copy would be a second owner of one texture.
struct DeviceImageState {
    /// @brief Constructs the description a device image answers with.
    /// @param holder Device holding the pixels.
    /// @param dimensions Pixel dimensions of the texture.
    /// @param layout Host sample layout the texture corresponds to.
    /// @param meaning Meaning of the texture's RGB sample values.
    /// @param source Source orientation the host buffer carried.
    DeviceImageState(DeviceId holder, ImageSize dimensions, PixelFormat layout,
                     ColorEncoding meaning, ImageOrientation source)
        : device(holder), size(dimensions), format(layout), encoding(std::move(meaning)),
          orientation(source) {}

    DeviceImageState(const DeviceImageState&) = delete;
    DeviceImageState& operator=(const DeviceImageState&) = delete;
    virtual ~DeviceImageState() = default;

    /// @brief Copies the texture into a new host buffer carrying this description.
    /// @throws std::runtime_error if the device cannot read the texture back.
    [[nodiscard]] virtual ImageBuffer readBack() const = 0;

    /// @brief Device holding the pixels.
    DeviceId device;

    /// @brief Pixel dimensions of the texture.
    ImageSize size;

    /// @brief Host sample layout the texture corresponds to.
    PixelFormat format;

    /// @brief Meaning of the texture's RGB sample values.
    ColorEncoding encoding;

    /// @brief Source orientation still to be applied, carried so that a read
    /// back buffer is the one that was uploaded rather than a near relative.
    ImageOrientation orientation;
};

} // namespace arraw::detail

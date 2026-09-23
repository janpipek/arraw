#pragma once

#include <ImageBuffer.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace arraw {

namespace detail {
struct DeviceImageState;
} // namespace detail

/// @brief Identity of one graphics device, as far as anything outside it cares.
///
/// QRhi resources belong to the instance that made them, are used from that
/// instance's one owner thread, and are not shareable between instances, so a
/// preview viewport and an offscreen exporter are two devices and a texture
/// from one is meaningless to the other. Carrying the identity next to the
/// texture makes that a refusable mistake rather than a convention (ADR 015).
enum class DeviceId : std::uint64_t {
    None = 0, ///< No device: what an empty ::arraw::DeviceImage reports.
};

/// @brief Pixels held by a graphics device, and the device that holds them.
///
/// The GPU alternative of a ::arraw::RenderCheckpoint's payload. Deliberately
/// names no graphics type: the QRhi texture lives behind
/// ::arraw::detail::DeviceImageState, whose device-side subclass is defined
/// where Qt may be named, so that the engine's headers, and the public API above
/// them, stay free of Qt. Only `src/gpu` translation units compile against
/// `Qt6::GuiPrivate`.
///
/// Default-constructs empty. A non-empty one comes from a device, which is why
/// nothing here creates one.
///
/// Immutable, and cheap to copy: copies share one texture.
class DeviceImage {
public:
    /// @brief Constructs an empty image, bound to no device.
    DeviceImage() noexcept = default;

    /// @brief Whether this image holds pixels on a device.
    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    /// @brief Device holding the pixels, or ::arraw::DeviceId::None if empty.
    [[nodiscard]] DeviceId device() const noexcept;

    /// @brief Pixel dimensions of the texture.
    [[nodiscard]] ImageSize size() const noexcept;

    /// @brief Host sample layout the texture corresponds to.
    ///
    /// Only ::arraw::PixelFormat::RgbaF32 occurs today. Whether a half-float
    /// texture is worth its precision loss is an empirical question the GPU
    /// probe answers before anything depends on it; naming the host layout here
    /// keeps the storage contract visible rather than implied.
    [[nodiscard]] PixelFormat format() const noexcept;

    /// @brief Meaning of the texture's RGB sample values.
    /// @throws std::logic_error if the image is empty.
    [[nodiscard]] const ColorEncoding& encoding() const;

    /// @brief Copies the texture into host memory.
    /// @return A new buffer holding the texture's pixels.
    /// @throws std::logic_error if the image is empty.
    /// @throws std::runtime_error if the device cannot read the texture back.
    [[nodiscard]] ImageBuffer readBack() const;

private:
    /// Only a device mints one of these, which is the ownership rule ADR 015
    /// decides written as an access rule.
    friend class GpuContext;

    explicit DeviceImage(std::shared_ptr<const detail::DeviceImageState> state) noexcept
        : state_(std::move(state)) {}

    /// @brief Description and device-side storage, shared by every copy.
    std::shared_ptr<const detail::DeviceImageState> state_;
};

} // namespace arraw

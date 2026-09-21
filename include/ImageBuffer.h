#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace arraw {

/// @brief Pixel dimensions of an image, in pixels.
struct ImageSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /// @brief Checks whether either dimension is zero.
    /// @return `true` if @ref width or @ref height is zero.
    [[nodiscard]] constexpr bool empty() const noexcept {
        return width == 0 || height == 0;
    }

    /// @brief Computes the total number of pixels.
    /// @return `width * height`, widened to avoid overflow.
    [[nodiscard]] constexpr std::uint64_t pixelCount() const noexcept {
        return static_cast<std::uint64_t>(width) * height;
    }

    friend bool operator==(const ImageSize&, const ImageSize&) = default;
};

/// @brief CPU-resident sample layout of an ::ImageBuffer.
///
/// All formats are tightly packed and use interleaved RGB(A) channel order.
/// Integer samples use native byte order. Alpha, when present, is straight.
enum class PixelFormat {
    RgbU8,
    RgbaU8,
    RgbU16,
    RgbaU16,
    RgbF32,
    RgbaF32,
};

/// @brief Number of channels (3 for RGB, 4 for RGBA) in a pixel format.
/// @param format Pixel format to query.
/// @return 3 or 4.
/// @throws std::invalid_argument if @p format is not a recognised value.
[[nodiscard]] constexpr std::size_t channelCount(PixelFormat format) {
    switch (format) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbU16:
    case PixelFormat::RgbF32:
        return 3;
    case PixelFormat::RgbaU8:
    case PixelFormat::RgbaU16:
    case PixelFormat::RgbaF32:
        return 4;
    }
    throw std::invalid_argument("unknown pixel format");
}

/// @brief Size in bytes of a single sample of a pixel format.
/// @param format Pixel format to query.
/// @return `sizeof(uint8_t)`, `sizeof(uint16_t)`, or `sizeof(float)`.
/// @throws std::invalid_argument if @p format is not a recognised value.
[[nodiscard]] constexpr std::size_t bytesPerChannel(PixelFormat format) {
    switch (format) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        return sizeof(std::uint8_t);
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        return sizeof(std::uint16_t);
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        return sizeof(float);
    }
    throw std::invalid_argument("unknown pixel format");
}

/// @brief Size in bytes of one full pixel (all channels) of a pixel format.
/// @param format Pixel format to query.
/// @return @ref channelCount(PixelFormat) times @ref bytesPerChannel(PixelFormat).
/// @throws std::invalid_argument if @p format is not a recognised value.
[[nodiscard]] constexpr std::size_t bytesPerPixel(PixelFormat format) {
    return channelCount(format) * bytesPerChannel(format);
}

/// @brief Meaning of an ::ImageBuffer's RGB sample values.
///
/// Input profiles are converted to the working encoding while loading; named
/// output encodings receive their matching ICC profile when exported.
enum class ColorEncoding {
    LinearRec2020, ///< Rec.2020 primaries with a linear transfer function.
    Srgb,
    DisplayP3,
    AdobeRgb,
};

/// @brief Encoding that development happens in; see ADR 003.
inline constexpr ColorEncoding workingEncoding = ColorEncoding::LinearRec2020;

/// @brief One tightly packed, CPU-resident colour raster.
///
/// The buffer is movable but deliberately not copyable; share published
/// images as `std::shared_ptr<const ImageBuffer>`.
class ImageBuffer {
public:
    /// @brief Allocates a zero-initialised buffer.
    /// @param size Pixel dimensions; must be non-empty.
    /// @param format Sample layout to store.
    /// @param encoding Meaning of the RGB sample values.
    /// @throws std::invalid_argument if @p size is empty.
    /// @throws std::length_error if the buffer size would overflow `size_t`.
    ImageBuffer(ImageSize size, PixelFormat format, ColorEncoding encoding);

    ImageBuffer(const ImageBuffer&) = delete;
    ImageBuffer& operator=(const ImageBuffer&) = delete;
    ImageBuffer(ImageBuffer&&) noexcept = default;
    ImageBuffer& operator=(ImageBuffer&&) noexcept = default;
    ~ImageBuffer() = default;

    /// @brief Pixel dimensions of the buffer.
    [[nodiscard]] ImageSize size() const noexcept {
        return size_;
    }

    /// @brief Sample layout of the buffer.
    [[nodiscard]] PixelFormat format() const noexcept {
        return format_;
    }

    /// @brief Meaning of the buffer's RGB sample values.
    [[nodiscard]] ColorEncoding encoding() const noexcept {
        return encoding_;
    }

    /// @brief Number of bytes between the start of consecutive rows.
    [[nodiscard]] std::size_t rowStride() const noexcept {
        return rowStride_;
    }

    /// @brief Total size of the sample storage, in bytes.
    [[nodiscard]] std::size_t byteSize() const noexcept;

    /// @brief Zero-copy reinterpretation of the sample storage as raw bytes.
    ///
    /// Useful for file I/O, GPU upload, hashing, etc. Not a second copy of
    /// the pixels; the returned span is only valid for as long as this
    /// ImageBuffer is.
    /// @return A read-only view over the buffer's storage.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

    /// @copydoc bytes() const
    [[nodiscard]] std::span<std::byte> bytes() noexcept;

    /// @brief Typed view over the buffer's sample storage.
    ///
    /// A sample is one channel value, rather than a complete pixel.
    ///
    /// @tparam T Sample type; must be `uint8_t`, `uint16_t`, or `float`, and
    /// must agree with @ref format().
    /// @return A read-only view over the buffer's storage.
    /// @throws std::bad_variant_access if @p T does not match @ref format().
    template <typename T> [[nodiscard]] std::span<const T> samples() const {
        static_assert(isSupportedSample<T>);
        const auto& values = std::get<std::vector<T>>(storage_);
        return {values.data(), values.size()};
    }

    /// @copydoc samples() const
    template <typename T> [[nodiscard]] std::span<T> samples() {
        static_assert(isSupportedSample<T>);
        auto& values = std::get<std::vector<T>>(storage_);
        return {values.data(), values.size()};
    }

    /// @brief Create an independent copy of the buffer.
    [[nodiscard]] ImageBuffer clone() const;

    /// @brief Holds real, correctly-aligned `std::vector<T>` objects rather
    /// than a `std::vector<std::byte>` blob.
    ///
    /// That's what lets samples<T>() and bytes() both be legal, UB-free
    /// views over the same memory: reading any object's representation as
    /// bytes is always allowed, but reinterpreting a raw byte buffer back as
    /// float (say) is not, without extra ceremony.
    using Storage =
        std::variant<std::vector<std::uint8_t>, std::vector<std::uint16_t>, std::vector<float>>;

private:
    template <typename T>
    static constexpr bool isSupportedSample =
        std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::uint16_t> ||
        std::is_same_v<T, float>;

    ImageSize size_;
    PixelFormat format_;
    ColorEncoding encoding_;
    std::size_t rowStride_;
    Storage storage_;
};

} // namespace arraw

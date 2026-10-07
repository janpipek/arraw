#pragma once

#include <ColorEncoding.h>
#include <ImageOrientation.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace arraw {

namespace detail {

/// @brief Allocator whose vectors leave their elements uninitialised unless given a value.
///
/// Sizing a `std::vector` with it allocates without writing the memory, which
/// ::arraw::ImageBuffer then zeroes on every thread rather than one. An
/// implementation detail of the buffer's storage.
/// @tparam T Element type, a trivially constructible sample.
template <typename T> struct DefaultInitAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;

    DefaultInitAllocator() = default;

    template <typename U> constexpr DefaultInitAllocator(const DefaultInitAllocator<U>&) noexcept {}

    /// @brief Allocates storage for some elements.
    /// @param count Elements to make room for.
    /// @return The first element's address.
    [[nodiscard]] T* allocate(std::size_t count) {
        return std::allocator<T>().allocate(count);
    }

    /// @brief Releases storage obtained from ::arraw::detail::DefaultInitAllocator::allocate.
    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>().deallocate(pointer, count);
    }

    /// @brief Default-initialises an element, which for a sample writes nothing.
    template <typename U>
    void construct(U* where) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(where)) U;
    }

    /// @brief Constructs an element from arguments, as `std::allocator` does.
    template <typename U, typename... Args> void construct(U* where, Args&&... args) {
        ::new (static_cast<void*>(where)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    friend constexpr bool operator==(const DefaultInitAllocator&,
                                     const DefaultInitAllocator<U>&) noexcept {
        return true;
    }
};

/// @brief Vector of samples that is sized without being filled.
template <typename T> using SampleVector = std::vector<T, DefaultInitAllocator<T>>;

} // namespace detail

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

/// @brief Sample layout that development happens in.
///
/// Exposure lifts samples above 1, white balance does the same, and the camera
/// matrix produces negatives for sensor colours outside Rec.2020; none of that
/// survives an integer layout. ADR 003's pattern applies here too: the
/// enumerator names the layout, this constant names the role.
inline constexpr PixelFormat workingFormat = PixelFormat::RgbaF32;

/// @brief One tightly packed, CPU-resident colour raster.
///
/// The buffer is movable but deliberately not copyable; share published
/// images as `std::shared_ptr<const ImageBuffer>`.
class ImageBuffer {
public:
    /// @brief Allocates a zero-initialised buffer.
    ///
    /// The samples are zeroed across threads, in bands of rows, without
    /// counting as a unit of progress. A large buffer made inside a banded loop
    /// would start threads of its own on every band: make it before the loop.
    /// @param size Pixel dimensions; must be non-empty.
    /// @param format Sample layout to store.
    /// @param encoding Meaning of the RGB sample values.
    /// @param orientation Source orientation still to be applied during development.
    /// @throws std::invalid_argument if @p size is empty.
    /// @throws std::length_error if the buffer size would overflow `size_t`.
    ImageBuffer(ImageSize size, PixelFormat format, ColorEncoding encoding,
                ImageOrientation orientation = ImageOrientation::Normal);

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
    [[nodiscard]] const ColorEncoding& encoding() const noexcept {
        return encoding_;
    }

    /// @brief Source orientation still to be applied to these pixels.
    [[nodiscard]] ImageOrientation orientation() const noexcept {
        return orientation_;
    }

    /// @brief Sensor pixels one pixel of this buffer spans along each side.
    ///
    /// 1 for a full decode. A spatial stage whose reach is measured in sensor
    /// pixels, such as noise reduction, divides it by this, so that a reduced
    /// copy develops as an approximation of the full photograph (ADR 039).
    /// ::arraw::halved doubles it, a half-size RAW decode gives 2, a resize
    /// multiplies it by the reduction, and copies, conversions, crops and
    /// development before the resize keep it. It is a fact about the pixels, so
    /// it travels with them rather than with a request.
    [[nodiscard]] double pixelScale() const noexcept {
        return pixelScale_;
    }

    /// @brief Says how many sensor pixels one pixel of this buffer spans along each side.
    /// @param scale The span; finite and above zero.
    /// @throws std::invalid_argument if @p scale is not finite or not above zero.
    void setPixelScale(double scale);

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
        const auto& values = std::get<detail::SampleVector<T>>(storage_);
        return {values.data(), values.size()};
    }

    /// @copydoc samples() const
    template <typename T> [[nodiscard]] std::span<T> samples() {
        static_assert(isSupportedSample<T>);
        auto& values = std::get<detail::SampleVector<T>>(storage_);
        return {values.data(), values.size()};
    }

    /// @brief Create an independent copy of the buffer, its pixel scale included.
    [[nodiscard]] ImageBuffer clone() const;

    /// @brief Holds real, correctly-aligned vector objects rather
    /// than a `std::vector<std::byte>` blob.
    ///
    /// That's what lets samples<T>() and bytes() both be legal, UB-free
    /// views over the same memory: reading any object's representation as
    /// bytes is always allowed, but reinterpreting a raw byte buffer back as
    /// float (say) is not, without extra ceremony.
    ///
    /// The vectors do not fill themselves when sized (see
    /// ::arraw::detail::DefaultInitAllocator): the constructor zeroes them on
    /// every thread, which a vector's own fill, on one, takes ~190 ms for a
    /// 24 MP RGBA float buffer to do (ADR 043).
    using Storage = std::variant<detail::SampleVector<std::uint8_t>,
                                 detail::SampleVector<std::uint16_t>, detail::SampleVector<float>>;

private:
    /// @brief Copies the samples as they are, without zeroing first; only clone() copies.
    ImageBuffer(const ImageBuffer&) = default;

    template <typename T>
    static constexpr bool isSupportedSample =
        std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::uint16_t> ||
        std::is_same_v<T, float>;

    ImageSize size_;
    PixelFormat format_;
    ColorEncoding encoding_;
    ImageOrientation orientation_;
    double pixelScale_ = 1.0;
    std::size_t rowStride_;
    Storage storage_;
};

} // namespace arraw

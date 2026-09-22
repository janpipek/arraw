#include "ImageBuffer.h"

using namespace std;
using namespace arraw;

constexpr static ImageSize validateSize(const ImageSize& size);

static constexpr size_t checkedMultiply(std::size_t left, std::size_t right);

static constexpr std::size_t checkedPixelCount(ImageSize size);

static constexpr std::size_t checkedSampleCount(ImageSize size, PixelFormat format);

static ImageBuffer::Storage allocateStorage(std::size_t sampleCount, PixelFormat format);

ImageBuffer::ImageBuffer(ImageSize size, PixelFormat format, ColorEncoding encoding,
                         ImageOrientation orientation)
    : size_(validateSize(size)), format_(format), encoding_(std::move(encoding)),
      orientation_(orientation), rowStride_(checkedMultiply(size_.width, bytesPerPixel(format_))),
      storage_(allocateStorage(checkedSampleCount(size_, format_), format_)) {}

span<byte> ImageBuffer::bytes() noexcept {
    return std::visit([](auto& samples) { return std::as_writable_bytes(std::span{samples}); },
                      storage_);
}

span<const byte> ImageBuffer::bytes() const noexcept {
    return std::visit([](const auto& samples) { return std::as_bytes(std::span{samples}); },
                      storage_);
}

size_t ImageBuffer::byteSize() const noexcept {
    return std::visit(
        [](const auto& samples) {
            using Sample = typename std::decay_t<decltype(samples)>::value_type;
            return samples.size() * sizeof(Sample);
        },
        storage_);
}

ImageBuffer ImageBuffer::clone() const {
    ImageBuffer result(this->size(), this->format(), this->encoding(), this->orientation());
    result.storage_ = this->storage_;
    return result;
}

static ImageBuffer::Storage allocateStorage(std::size_t sampleCount, PixelFormat format) {
    switch (format) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        return std::vector<std::uint8_t>(sampleCount);
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        return std::vector<std::uint16_t>(sampleCount);
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        return std::vector<float>(sampleCount);
    }
    throw std::invalid_argument("unknown pixel format");
}

constexpr ImageSize validateSize(const ImageSize& size) {
    if (size.empty())
        throw std::invalid_argument("image dimensions must be non-zero");
    return size;
}

constexpr ::size_t checkedMultiply(const std::size_t left, const std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)
        throw std::length_error("image buffer size overflows size_t");
    return left * right;
}

constexpr std::size_t checkedPixelCount(ImageSize size) {
    const auto count = size.pixelCount();
    if (count > std::numeric_limits<std::size_t>::max())
        throw std::length_error("image pixel count exceeds size_t");
    return static_cast<std::size_t>(count);
}

constexpr std::size_t checkedSampleCount(ImageSize size, PixelFormat format) {
    return checkedMultiply(checkedPixelCount(size), channelCount(format));
}

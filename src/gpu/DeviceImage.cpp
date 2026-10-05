#include "DeviceImage.h"

#include "DeviceImageState.h"

#include <stdexcept>

namespace arraw {

DeviceId DeviceImage::device() const noexcept {
    return state_ ? state_->device : DeviceId::None;
}

ImageSize DeviceImage::size() const noexcept {
    return state_ ? state_->size : ImageSize{};
}

PixelFormat DeviceImage::format() const noexcept {
    return state_ ? state_->format : workingFormat;
}

double DeviceImage::pixelScale() const noexcept {
    return state_ ? state_->pixelScale : 1.0;
}

std::size_t DeviceImage::channelCount() const noexcept {
    return state_ ? state_->channels : 4;
}

const ColorEncoding& DeviceImage::encoding() const {
    if (!state_) {
        throw std::logic_error("An empty device image has no encoding");
    }
    return state_->encoding;
}

ImageBuffer DeviceImage::readBack() const {
    if (!state_) {
        throw std::logic_error("An empty device image has no pixels to read back");
    }
    // The transfer belongs to the device that owns the texture, so it is the
    // state's to perform: only the side of the boundary that knows QRhi can.
    return state_->readBack();
}

} // namespace arraw

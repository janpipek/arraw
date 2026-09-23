#include <RenderCheckpoint.h>

#include "CheckpointState.h"

#include <stdexcept>
#include <utility>

namespace arraw {

namespace {

/// @brief Visits a payload's two alternatives with one callable.
///
/// Neither alternative can leave the variant valueless — `ImageBuffer` and
/// `DeviceImage` both move without throwing — so the accessors built on this
/// can be `noexcept` honestly.
template <typename Visitor> decltype(auto) onPixels(const CheckpointPixels& pixels, Visitor&& v) {
    return std::visit(std::forward<Visitor>(v), pixels);
}

} // namespace

ImageSize CheckpointState::size() const noexcept {
    return onPixels(pixels, [](const auto& held) { return held.size(); });
}

const ColorEncoding& CheckpointState::encoding() const {
    return onPixels(pixels,
                    [](const auto& held) -> const ColorEncoding& { return held.encoding(); });
}

bool CheckpointState::isResident() const noexcept {
    return std::holds_alternative<DeviceImage>(pixels);
}

ImageBuffer CheckpointState::readBack() const {
    if (const auto* buffer = std::get_if<ImageBuffer>(&pixels)) {
        // The payload is shared and immutable, so even a host-resident
        // checkpoint owes the caller its own copy.
        return buffer->clone();
    }
    return std::get<DeviceImage>(pixels).readBack();
}

RenderCheckpoint makeCheckpoint(Stage boundary, ProcessingPlan plan, CheckpointPixels pixels) {
    return RenderCheckpoint{std::make_shared<const CheckpointState>(
        CheckpointState{boundary, std::move(plan), std::move(pixels)})};
}

RenderCheckpoint::RenderCheckpoint(std::shared_ptr<const CheckpointState> state)
    : state_(std::move(state)) {
    if (!state_) {
        throw std::invalid_argument("A checkpoint needs state");
    }
    if (static_cast<std::size_t>(state_->boundary) >= stageCount) {
        throw std::invalid_argument("A checkpoint needs a recognised pass boundary");
    }
    const auto* image = std::get_if<DeviceImage>(&state_->pixels);
    if (image != nullptr && !image->valid()) {
        throw std::invalid_argument("A checkpoint cannot hold an empty device image");
    }
    if (state_->size().empty()) {
        throw std::invalid_argument("A checkpoint cannot hold an empty image");
    }
    if (const auto* buffer = std::get_if<ImageBuffer>(&state_->pixels);
        buffer != nullptr &&
        buffer->byteSize() != buffer->size().pixelCount() * bytesPerPixel(buffer->format())) {
        throw std::invalid_argument("A checkpoint needs complete pixel storage");
    }
}

Stage RenderCheckpoint::boundary() const noexcept {
    return state_->boundary;
}

ImageSize RenderCheckpoint::size() const noexcept {
    return state_->size();
}

const ColorEncoding& RenderCheckpoint::encoding() const {
    return state_->encoding();
}

bool RenderCheckpoint::isResident() const noexcept {
    return state_->isResident();
}

ImageBuffer RenderCheckpoint::readBack() const {
    return state_->readBack();
}

} // namespace arraw

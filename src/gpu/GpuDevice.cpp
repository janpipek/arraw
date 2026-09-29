#include "GpuDevice.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace arraw::detail {

namespace {

/// @brief An offscreen frame, ended on every way out of the scope that began it.
///
/// Only the normal way out asks how ending went; an exception already says
/// what went wrong, and ending the frame is only what keeps the device usable.
class OffscreenFrame {
public:
    /// @throws std::runtime_error if the frame cannot be begun.
    OffscreenFrame(QRhi& rhi, const std::string& purpose) : rhi_(rhi) {
        if (rhi_.beginOffscreenFrame(&commands_) != QRhi::FrameOpSuccess || commands_ == nullptr) {
            throw std::runtime_error("The GPU device could not begin a frame to " + purpose);
        }
    }

    OffscreenFrame(const OffscreenFrame&) = delete;
    OffscreenFrame& operator=(const OffscreenFrame&) = delete;
    OffscreenFrame(OffscreenFrame&&) = delete;
    OffscreenFrame& operator=(OffscreenFrame&&) = delete;

    ~OffscreenFrame() {
        if (open_) {
            rhi_.endOffscreenFrame();
        }
    }

    /// @brief Command buffer the frame records into.
    [[nodiscard]] QRhiCommandBuffer& commands() const noexcept {
        return *commands_;
    }

    /// @brief Ends the frame and waits for its work.
    /// @return Whether the frame succeeded.
    [[nodiscard]] bool end() {
        open_ = false;
        return rhi_.endOffscreenFrame() == QRhi::FrameOpSuccess;
    }

private:
    QRhi& rhi_;
    QRhiCommandBuffer* commands_ = nullptr;
    bool open_ = true;
};

/// @brief Returns an unsubmitted batch to the device's pool.
struct BatchRelease {
    void operator()(QRhiResourceUpdateBatch* batch) const noexcept {
        batch->release();
    }
};

} // namespace

void submitUpdates(GpuDevice& device, const std::string& purpose,
                   const std::function<void(QRhiResourceUpdateBatch&)>& record) {
    QRhi& rhi = *device.rhi;
    OffscreenFrame frame(rhi, purpose);
    // Declared after the frame, so a batch never submitted is released before
    // the frame ends.
    std::unique_ptr<QRhiResourceUpdateBatch, BatchRelease> batch(rhi.nextResourceUpdateBatch());
    if (!batch) {
        throw std::runtime_error("The GPU device had no resource update batch free to " + purpose);
    }
    record(*batch);
    // Commits and releases the batch; no pass is needed for transfers alone.
    frame.commands().resourceUpdate(batch.release());
    if (!frame.end()) {
        device.lost = true;
        throw std::runtime_error("The GPU device failed to " + purpose);
    }
}

} // namespace arraw::detail

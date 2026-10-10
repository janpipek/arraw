#pragma once

#include "DeviceImage.h"

#include <cstdint>

namespace arraw::detail {

/// @brief The device-side copy of a ladder's packed brush coverage (ADR 044, section 8).
///
/// Declared in core and implemented where Qt may be named (`src/gpu`), as
/// ::arraw::detail::DeviceImageState is, so that core never names a graphics type. A
/// ::arraw::detail::CoverageResidency owns at most one; it is dropped with the residency, which
/// is dropped when the ladder rebinds or clears. It belongs to the thread that owns its device.
class DeviceCoverage {
public:
    DeviceCoverage(const DeviceCoverage&) = delete;
    DeviceCoverage& operator=(const DeviceCoverage&) = delete;
    virtual ~DeviceCoverage() = default;

    /// @brief Gives the device whose textures this holds.
    [[nodiscard]] DeviceId device() const noexcept {
        return device_;
    }

    /// @brief Gives the number of rectangles uploaded into existing textures (for tests).
    [[nodiscard]] std::uint64_t rectanglesUploaded() const noexcept {
        return rectangles_;
    }

    /// @brief Gives the number of planes uploaded whole, as a new texture (for tests).
    [[nodiscard]] std::uint64_t planesUploaded() const noexcept {
        return planes_;
    }

protected:
    /// @brief Constructs an empty copy for a device.
    explicit DeviceCoverage(DeviceId device) noexcept : device_(device) {}

    /// @brief Counts rectangles uploaded into an existing texture.
    void countRectangles(std::uint64_t count) noexcept {
        rectangles_ += count;
    }

    /// @brief Counts one plane uploaded whole.
    void countPlane() noexcept {
        ++planes_;
    }

private:
    DeviceId device_;
    std::uint64_t rectangles_ = 0;
    std::uint64_t planes_ = 0;
};

} // namespace arraw::detail

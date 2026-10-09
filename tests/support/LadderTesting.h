#pragma once

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <Progress.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/// Helpers for the tests of the checkpoint ladder, on the CPU and on the GPU.

namespace arraw::test {

/// @brief Whether two working-format buffers hold the same bits.
[[nodiscard]] inline bool sameBits(const ImageBuffer& left, const ImageBuffer& right) {
    const auto a = left.samples<float>();
    const auto b = right.samples<float>();
    return left.size() == right.size() && a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

/// @brief Builds a state with the edits the preview's tests vary, one per stage.
/// @param exposure Tone: changes the pointwise chain.
/// @param straighten Geometry: degrees.
/// @param vignette Effects: vignette amount.
/// @param colorNoise Denoise: colour noise reduction.
[[nodiscard]] inline DevelopState stateWith(float exposure, double straighten,
                                            float vignette = 0.0F, float colorNoise = 0.0F) {
    DevelopSettings settings;
    settings.tone.exposure = exposure;
    settings.geometry.straighten = straighten;
    settings.effects.vignette.amount = vignette;
    settings.noiseReduction.color = colorNoise;
    return DevelopState{settings};
}

/// @brief Builds a request that fits the frame inside a square.
[[nodiscard]] inline RenderRequest fitting(std::uint32_t edge) {
    return {.size = RenderRequest::FitInside{edge, edge}};
}

/// @brief Records the reports of a render.
struct ReportLog {
    std::vector<Progress> reports; ///< Every report, in order.

    /// @brief Gives a callback that records into this log.
    [[nodiscard]] ProgressChannel::Callback callback() {
        return [this](const Progress& progress) { reports.push_back(progress); };
    }

    /// @brief Whether the fractions never fall, the steps never go back, and the last is one.
    [[nodiscard]] bool wellFormed() const {
        if (reports.empty() || reports.back().fraction != 1.0) {
            return false;
        }
        for (std::size_t index = 1; index < reports.size(); ++index) {
            if (reports[index].fraction < reports[index - 1].fraction ||
                static_cast<int>(reports[index].step) < static_cast<int>(reports[index - 1].step)) {
                return false;
            }
        }
        return true;
    }
};

} // namespace arraw::test

#pragma once

#include <DevelopState.h>

#include <chrono>

namespace arraw::app {

/// @brief Time a preview render waits after a noise reduction edit, so that a drag asks for one.
inline constexpr std::chrono::milliseconds noiseReductionRenderDelay{200};

/// @brief Tells how long to hold a preview render back after an edit.
///
/// A noise reduction edit reruns the Denoise pass and everything after it
/// (ADR 039), the most expensive render there is on the CPU, so a drag of one
/// of its rows waits for the hand to rest, as `main` did. On the GPU the pass
/// is a few renders, and a drag follows the hand like any other. Any other
/// edit, or one that changes anything besides the noise reduction settings,
/// renders at once.
/// @param before State before the edit.
/// @param after State after the edit.
/// @param onGpu Whether the preview renders on the GPU, as its last render said.
/// @return ::arraw::app::noiseReductionRenderDelay if only the noise reduction settings differ
/// and the preview renders on the CPU, zero otherwise.
[[nodiscard]] std::chrono::milliseconds renderDelayFor(const DevelopState& before,
                                                       const DevelopState& after, bool onGpu);

} // namespace arraw::app

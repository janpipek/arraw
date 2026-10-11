#pragma once

#include "PreviewRenderer.h"

#include <Progress.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace arraw::test {

/// @brief A progress callback that holds the worker at the first report of a render until opened.
///
/// Makes "a render is in flight" a known state instead of a race: the test closes the gate, makes
/// a request, waits until the worker is held inside its render, and only then queues the others.
class ProgressGate {
public:
    /// Callback to give the renderer.
    [[nodiscard]] app::PreviewRenderer::ProgressCallback callback() {
        return [this](std::uint64_t, const Progress&) {
            std::unique_lock lock(mutex_);
            if (closed_) {
                held_ = true;
                changed_.notify_all();
                changed_.wait_for(lock, holdLimit, [this] { return !closed_; });
            }
        };
    }

    /// Makes the next report of a render hold the worker.
    void close() {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
        held_ = false;
    }

    /// Waits until the worker is held at a report.
    [[nodiscard]] bool waitHeld() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, holdLimit, [this] { return held_; });
    }

    /// Lets the worker go, and every later render pass.
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            closed_ = false;
        }
        changed_.notify_all();
    }

private:
    static constexpr auto holdLimit = std::chrono::seconds(60);
    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_ = false;
    bool held_ = false;
};

} // namespace arraw::test

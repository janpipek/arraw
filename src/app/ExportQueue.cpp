#include "ExportQueue.h"

#include "GpuContext.h"
#include "GpuDevelop.h"

#include <Diagnostics.h>
#include <RenderCheckpoint.h>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace arraw::app {

namespace {

/// @brief The worker's GPU device, made when the first job needs it.
///
/// Lives as a local of the worker's loop, so that it is destroyed on the
/// thread that owns it. Holds no device image between jobs.
class GpuExport {
public:
    GpuExport() = default;
    GpuExport(const GpuExport&) = delete;
    GpuExport& operator=(const GpuExport&) = delete;
    GpuExport(GpuExport&&) = delete;
    GpuExport& operator=(GpuExport&&) = delete;
    ~GpuExport() = default;

    /// @brief Develops a job on the device and reads the result back.
    ///
    /// Every device image is gone before this returns, whether it succeeds
    /// or not. A lost device is dropped, and every later call is refused.
    /// @param job What to develop.
    /// @param failure Receives why the GPU did not develop; left alone on success.
    /// @return The developed photograph, or nothing if the CPU must.
    [[nodiscard]] std::optional<ImageBuffer> develop(const ExportJob& job, std::string& failure) {
        if (!tried_) {
            tried_ = true;
            context_ = createHardwareContext(reason_);
        }
        if (!context_) {
            failure = reason_;
            return std::nullopt;
        }
        try {
            const RenderCheckpoint checkpoint =
                developOnGpu(*context_, *job.source, job.state, Stage::Resize, job.request);
            return checkpoint.readBack();
        } catch (const std::exception& error) {
            failure = error.what();
        } catch (...) {
            failure = "Unknown error while developing on the GPU";
        }
        // The checkpoint above is destroyed by now, so the device may go.
        if (context_->lost()) {
            reason_ = "GPU device lost";
            failure += " (the device is lost, so the rest is exported on the CPU)";
            context_.reset();
        }
        return std::nullopt;
    }

private:
    /// Device of this thread; empty before the first job, and after a loss.
    std::unique_ptr<GpuContext> context_;
    /// Why there is no device; empty while there is one.
    std::string reason_;
    /// Whether a device has been tried for.
    bool tried_ = false;
};

/// @brief Runs one job, turning a failure into a result.
ExportResult execute(std::uint64_t id, const ExportJob& job, GpuExport* gpu) {
    ExportResult result{.id = id, .path = job.path};
    // Nothing may escape the thread, or the process terminates.
    try {
        if (!job.source) {
            throw std::invalid_argument("No photograph to export");
        }
        std::optional<ImageBuffer> developed;
        if (gpu != nullptr) {
            developed = gpu->develop(job, result.fallbackReason);
            result.onGpu = developed.has_value();
            if (result.onGpu) {
                result.fallbackReason.clear();
            }
        }
        if (!developed) {
            developed = develop(*job.source, job.state, job.request);
        }
        CollectedDiagnostics log;
        exportImage(*developed, job.path, job.options, job.metadata, log);
        for (const auto& entry : log.entries()) {
            result.warnings.push_back(describe(entry));
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "Unknown error while exporting";
    }
    return result;
}

} // namespace

ExportQueue::ExportQueue(std::function<void(ExportResult)> onResult, Device device)
    : onResult_(std::move(onResult)), device_(device),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

ExportQueue::~ExportQueue() {
    worker_.request_stop();
    worker_.join();
}

std::uint64_t ExportQueue::enqueue(ExportJob job) {
    std::uint64_t id = 0;
    {
        const std::scoped_lock lock(mutex_);
        id = ++lastId_;
        queue_.push_back(Pending{id, std::move(job)});
    }
    wake_.notify_one();
    return id;
}

std::size_t ExportQueue::cancelQueued() {
    const std::scoped_lock lock(mutex_);
    const std::size_t dropped = queue_.size();
    queue_.clear();
    return dropped;
}

void ExportQueue::run(std::stop_token stop) {
    // The device belongs to this thread: made by the first job, and destroyed
    // when this function returns, before the thread ends.
    std::optional<GpuExport> gpu;
    if (device_ == Device::Auto) {
        gpu.emplace();
    }
    while (true) {
        std::optional<Pending> next;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (stop.stop_requested()) {
                return; // Queued jobs are dropped.
            }
            next = std::move(queue_.front());
            queue_.pop_front();
        }
        ExportResult result = execute(next->id, next->job, gpu ? &*gpu : nullptr);
        // The job's photograph is released with it, before the next wait.
        next.reset();
        try {
            onResult_(std::move(result));
        } catch (...) {
            // The callback's failure is not ours to handle, and must not end the thread.
        }
    }
}

} // namespace arraw::app

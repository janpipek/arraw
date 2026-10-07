#include "PhotoLoader.h"

#include "DebugDiagnostics.h"
#include "ThumbnailWorker.h"
#include "TimingTrace.h"

#include <ImageImport.h>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace arraw::app {

/// @brief One thread serving the newest job posted to it.
///
/// A job posted while another runs replaces any job still queued and cancels the running one's
/// channel. The job is told its own channel, and is expected to throw ::arraw::Cancelled, or
/// return, once that channel is cancelled.
class PhotoLoader::Lane {
public:
    /// @brief Work of one job, given the channel that a newer job cancels.
    using Work = std::function<void(ProgressChannel& channel)>;

    Lane() : thread_([this](const std::stop_token& stop) { run(stop); }) {}

    Lane(const Lane&) = delete;
    Lane& operator=(const Lane&) = delete;
    Lane(Lane&&) = delete;
    Lane& operator=(Lane&&) = delete;

    /// @brief Stops the thread, cancelling the running job, and waits for it.
    ~Lane() {
        requestStop();
        thread_.join();
    }

    /// @brief Drops the queued job, cancels the running one and asks the thread to end,
    /// without waiting for it.
    ///
    /// Lets the owner stop every lane before waiting for any.
    void requestStop() {
        {
            // Dropped before the stop is requested, so the thread never takes it on waking.
            const std::scoped_lock lock(mutex_);
            pending_.reset();
            if (inFlight_) {
                inFlight_->cancel();
            }
        }
        thread_.request_stop();
    }

    /// @brief Queues a job, replacing the one queued and cancelling the one running.
    /// @param onProgress Receiver of the job's progress; may be empty.
    /// @param work The job.
    void post(ProgressChannel::Callback onProgress, Work work) {
        {
            const std::scoped_lock lock(mutex_);
            pending_.emplace(Job{std::move(onProgress), std::move(work)});
            if (inFlight_) {
                inFlight_->cancel();
            }
        }
        wake_.notify_one();
    }

    /// @brief Drops the queued job and cancels the running one.
    void cancel() {
        const std::scoped_lock lock(mutex_);
        pending_.reset();
        if (inFlight_) {
            inFlight_->cancel();
        }
    }

private:
    /// Job waiting for the thread.
    struct Job {
        ProgressChannel::Callback onProgress;
        Work work;
    };

    void run(const std::stop_token& stop) {
        while (true) {
            Job job;
            std::shared_ptr<ProgressChannel> channel;
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [this] { return pending_.has_value(); })) {
                    return;
                }
                job = std::move(*pending_);
                pending_.reset();
                channel = std::make_shared<ProgressChannel>(std::move(job.onProgress));
                inFlight_ = channel;
            }
            try {
                job.work(*channel);
            } catch (...) {
                // A job reports its own failures; nothing may end the thread.
            }
            const std::scoped_lock lock(mutex_);
            inFlight_.reset();
        }
    }

    /// Guard of pending_ and inFlight_.
    std::mutex mutex_;
    /// Signal that a job is queued, or the thread should stop.
    std::condition_variable_any wake_;
    /// Newest job not yet started.
    std::optional<Job> pending_;
    /// Channel of the running job; null when idle.
    std::shared_ptr<ProgressChannel> inFlight_;
    /// Declared last so that every member above exists before it starts.
    std::jthread thread_;
};

PhotoLoader::Decoder PhotoLoader::imageDecoder() {
    return [](const std::filesystem::path& path, ProgressChannel& channel) {
        DebugDiagnostics log;
        return loadImage(path, log, {}, &channel);
    };
}

PhotoLoader::PhotoLoader(ThumbnailCache cache, DecodedCallback onDecoded, PreviewCallback onPreview,
                         ProgressCallback onProgress, Decoder decoder)
    : cache_(std::move(cache)), onDecoded_(std::move(onDecoded)), onPreview_(std::move(onPreview)),
      onProgress_(std::move(onProgress)), decoder_(std::move(decoder)),
      decodes_(std::make_unique<Lane>()), previews_(std::make_unique<Lane>()) {}

PhotoLoader::~PhotoLoader() {
    // Both at once, so that closing waits for the slower of the two, not their sum.
    decodes_->requestStop();
    previews_->requestStop();
}

std::uint64_t PhotoLoader::decode(std::filesystem::path path) {
    const std::uint64_t id = ++lastId_;
    ProgressChannel::Callback forward;
    if (onProgress_) {
        forward = [this, id](const Progress& progress) { onProgress_(id, progress); };
    }
    decodes_->post(
        std::move(forward), [this, id, path = std::move(path)](ProgressChannel& channel) {
            const detail::TimingSpan timing("loader.decode", id);
            DecodedPhoto result{.request = id, .path = path, .decoded = nullptr, .error = {}};
            try {
                result.decoded = std::make_shared<const ImageBuffer>(decoder_(path, channel));
            } catch (const Cancelled&) {
                return; // Superseded: nobody wants it.
            } catch (const std::exception& error) {
                result.error = error.what();
            } catch (...) {
                // Delivered all the same: the window waits for this decode.
                result.error = "unknown error";
            }
            if (channel.cancelled() && !result.decoded) {
                return; // A failure after a cancel is the cancel's.
            }
            try {
                onDecoded_(std::move(result));
            } catch (...) {
                // The callback's failure is not ours to handle.
            }
        });
    return id;
}

void PhotoLoader::cancelDecode() {
    decodes_->cancel();
}

std::uint64_t PhotoLoader::readCameraPreview(std::filesystem::path path) {
    const std::uint64_t id = ++lastId_;
    previews_->post({}, [this, id, path = std::move(path)](ProgressChannel& channel) {
        const detail::TimingSpan timing("loader.cameraPreview", id);
        CameraPreview result{.request = id, .path = path, .image = {}};
        try {
            result.image = embeddedPreviewImage(cache_, path);
        } catch (const std::exception&) {
            // None to show: the crop mode keeps its other stand-ins.
        }
        if (channel.cancelled() || !onPreview_) {
            return;
        }
        try {
            onPreview_(std::move(result));
        } catch (...) {
            // The callback's failure is not ours to handle.
        }
    });
    return id;
}

} // namespace arraw::app

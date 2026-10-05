#include "ThumbnailWorker.h"

#include "DisplayImage.h"

#include <Develop.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImagePyramid.h>
#include <Sidecar.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>

#include <pthread/qos.h>
#else
#include <sys/resource.h>
#endif

namespace arraw::app {

namespace {

namespace fs = std::filesystem;

/// @brief Lowers the priority of the calling thread, so the preview and the interface win.
///
/// std::thread has no priority, and QThread::setPriority only reaches a QThread, so this
/// goes to the OS: on Linux, nice 10 for this thread alone (Linux's `setpriority` with a zero id
/// is per thread; a starved idle class is avoided on purpose, as a busy machine would then never
/// finish a thumbnail); on macOS the utility quality-of-service class; on Windows below-normal.
/// A failure changes nothing that matters and is ignored.
void lowerThreadPriority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    setpriority(PRIO_PROCESS, 0, 10);
#endif
}

/// @brief Copies a camera preview into a Qt image.
QImage toQImage(const ImageBuffer& preview) {
    const auto bytes = preview.bytes();
    const QImage view(reinterpret_cast<const uchar*>(bytes.data()),
                      static_cast<int>(preview.size().width),
                      static_cast<int>(preview.size().height),
                      static_cast<qsizetype>(preview.rowStride()), QImage::Format_RGBA8888);
    return view.copy();
}

/// @brief Reads the develop state a photograph's sidecar records, if it records one.
///
/// Nothing when there is no sidecar, when it records no settings, or when it
/// cannot be read: the photograph then develops from what its kind starts
/// from, as opening it would say.
std::optional<DevelopState> savedState(const fs::path& primary) {
    try {
        if (const auto contents = readSidecar(primary)) {
            return contents->state;
        }
    } catch (const std::exception&) {
        // An unreadable sidecar develops as defaults, as opening the photograph would say.
    }
    return std::nullopt;
}

/// @brief Gives the state a photograph's thumbnail develops with.
///
/// What its sidecar records, or else what its kind starts from, which takes a
/// look at the file's header (no decode) to tell a RAW from anything else.
DevelopState thumbnailState(const fs::path& primary) {
    if (auto saved = savedState(primary)) {
        return std::move(*saved);
    }
    return defaultStateFor(readImageMetadata(primary).encoding);
}

/// @brief Develops the thumbnail of a photograph: half-size decode, pyramid, develop.
QImage developThumbnail(const fs::path& primary, const DevelopState& state) {
    ImageBuffer source = loadImage(primary, discardedDiagnostics(), {.halfSize = true});
    RenderRequest request;
    request.size = RenderRequest::FitInside{ThumbnailCache::maxEdge, ThumbnailCache::maxEdge};
    // Planned against the buffer: a half-size decode is not the size the metadata declares.
    const int level = pyramidLevelFor(source.size(), source.orientation(), state, request);
    for (int i = 0; i < level; ++i) {
        source = halved(source);
    }
    // Radii in sensor pixels shrink with the reduction: the half-size decode
    // and every halving say so on the pixels (ADR 039).
    return toDisplayImage(develop(source, state, request));
}

} // namespace

ThumbnailWorker::ThumbnailWorker(ThumbnailCache cache,
                                 std::function<void(ThumbnailResult)> onResult, bool pruneAtStart)
    : cache_(std::move(cache)), onResult_(std::move(onResult)),
      pruner_([this, pruneAtStart](const std::stop_token&) {
          if (pruneAtStart) {
              lowerThreadPriority();
              try {
                  cache_.prune();
              } catch (const std::exception&) {
                  // A cache that cannot be tidied is still a cache.
              }
          }
      }),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

ThumbnailWorker::~ThumbnailWorker() {
    worker_.request_stop();
    pruner_.request_stop();
}

std::uint64_t ThumbnailWorker::setShots(std::vector<fs::path> primaries) {
    std::uint64_t now = 0;
    {
        const std::scoped_lock lock(mutex_);
        now = ++generation_;
        urgent_.clear();
        embedded_.clear();
        developed_.clear();
        known_.clear();
        for (fs::path& primary : primaries) {
            if (known_.insert(primary).second) {
                embedded_.push_back(primary);
                developed_.push_back(std::move(primary));
            }
        }
    }
    wake_.notify_one();
    return now;
}

void ThumbnailWorker::addShots(const std::vector<fs::path>& primaries) {
    {
        const std::scoped_lock lock(mutex_);
        for (const fs::path& primary : primaries) {
            if (known_.insert(primary).second) {
                embedded_.push_back(primary);
                developed_.push_back(primary);
            }
        }
    }
    wake_.notify_one();
}

void ThumbnailWorker::setVisible(std::vector<fs::path> primaries) {
    const std::scoped_lock lock(mutex_);
    visible_.clear();
    visible_.insert(std::make_move_iterator(primaries.begin()),
                    std::make_move_iterator(primaries.end()));
}

void ThumbnailWorker::invalidate(const fs::path& primary) {
    {
        const std::scoped_lock lock(mutex_);
        if (!known_.contains(primary)) {
            return;
        }
        // The developed picture will replace the embedded one, so that job is no use.
        std::erase(embedded_, primary);
        std::erase(developed_, primary);
        std::erase(urgent_, primary);
        urgent_.push_back(primary);
    }
    wake_.notify_one();
}

std::size_t ThumbnailWorker::pendingJobs() const {
    const std::scoped_lock lock(mutex_);
    return urgent_.size() + embedded_.size() + developed_.size() + (running_ ? 1 : 0);
}

ThumbnailWorker::Job ThumbnailWorker::takeFrom(std::deque<fs::path>& queue, ThumbnailKind kind) {
    auto chosen =
        std::ranges::find_if(queue, [this](const fs::path& p) { return visible_.contains(p); });
    if (chosen == queue.end()) {
        chosen = queue.begin();
    }
    Job job{std::move(*chosen), kind};
    queue.erase(chosen);
    return job;
}

std::optional<ThumbnailWorker::Job> ThumbnailWorker::next(const std::stop_token& stop,
                                                          std::uint64_t& generation) {
    std::unique_lock lock(mutex_);
    running_ = false;
    if (!wake_.wait(lock, stop, [&] {
            return !urgent_.empty() || !embedded_.empty() || !developed_.empty();
        })) {
        return std::nullopt;
    }
    generation = generation_.load();
    running_ = true;
    if (!urgent_.empty()) {
        Job job{std::move(urgent_.front()), ThumbnailKind::Developed};
        urgent_.pop_front();
        return job;
    }
    if (!embedded_.empty()) {
        return takeFrom(embedded_, ThumbnailKind::Embedded);
    }
    return takeFrom(developed_, ThumbnailKind::Developed);
}

void ThumbnailWorker::run(const std::stop_token& stop) {
    lowerThreadPriority();
    std::uint64_t generation = 0;
    while (const auto job = next(stop, generation)) {
        execute(*job, generation);
    }
}

void ThumbnailWorker::execute(const Job& job, std::uint64_t generation) {
    // Nothing may escape the thread, or the process terminates.
    try {
        QImage image;
        if (job.kind == ThumbnailKind::Embedded) {
            const auto key = ThumbnailCache::embeddedKey(job.primary);
            if (!key) {
                return;
            }
            image = cache_.load(*key);
            if (image.isNull()) {
                const auto preview = readEmbeddedPreview(job.primary, ThumbnailCache::maxEdge);
                if (!preview) {
                    return; // The shot has none; the placeholder stays until the developed one.
                }
                image = toQImage(*preview);
                cache_.store(*key, image);
            }
        } else {
            const DevelopState state = thumbnailState(job.primary);
            const auto key = ThumbnailCache::developedKey(job.primary, state);
            if (!key) {
                return;
            }
            image = cache_.load(*key);
            if (image.isNull()) {
                if (generation != generation_.load()) {
                    return; // Nobody wants it any more; a decode is not worth it.
                }
                image = developThumbnail(job.primary, state);
                cache_.store(*key, image);
            }
        }
        if (!image.isNull() && generation == generation_.load()) {
            onResult_({job.primary, job.kind, std::move(image), generation});
        }
    } catch (const std::exception&) {
        // A file that will not decode keeps its placeholder.
    }
}

} // namespace arraw::app

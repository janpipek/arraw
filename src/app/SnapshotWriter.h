#pragma once

#include <Sidecar.h>
#include <SnapshotWrites.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace arraw::app {

/// @brief Result of one snapshot write the writer carried out or failed.
struct SnapshotWriteResult {
    /// @brief How a write failed.
    enum class Failure {
        None,        ///< It was carried out; see ::arraw::app::SnapshotWriteResult::outcome.
        Conflict,    ///< The sidecar's collection was not the revision the write expected.
        LockTimeout, ///< The sidecar stayed locked by another writer.
        Refused,     ///< The write was invalid (a full list, a bad name, a list too large).
        Error,       ///< Anything else: a sidecar that cannot be read or written.
    };

    /// @brief Number ::arraw::app::SnapshotWriter::submit gave the request.
    std::uint64_t ticket = 0;

    /// @brief Tag the caller gave the request, such as the window's session serial.
    std::uint64_t owner = 0;

    /// @brief The request, echoed.
    SnapshotWrite write;

    /// @brief What the write did; valid when @ref failure is None.
    SnapshotWriteOutcome outcome;

    /// @brief How it failed, if it did.
    Failure failure = Failure::None;

    /// @brief What() of the failure; empty when there was none.
    std::string error;

    /// @brief After a Conflict, the collection read right afterwards; nothing if that failed.
    std::optional<SnapshotUpdate> fresh;

    /// @brief ::arraw::app::fileStamp of the sidecar right after a write that changed it.
    std::optional<std::int64_t> stamp;
};

/// @brief Thread that writes snapshot changes to sidecars, one request at a time, in order.
///
/// One thread and one FIFO queue (ADR 056, Gate 4). Results are delivered in submission order, on
/// the writer's thread; the caller marshals them to the thread that owns its session. Like
/// ::arraw::app::ThumbnailWorker it needs no event loop, so tests drive it without a window.
///
/// Adjacent thumbnail patches for one photograph, up to
/// ::arraw::app::SnapshotWriter::maximumBatch, are written under one lock with one commit; each
/// still gets its own result, and they share the stamp. A structural request is never reordered
/// past another request. The writer holds the sidecar's lock only inside the core calls, so a
/// Save on another thread waits for it as it would for another process.
///
/// A conflict result carries the collection read right after it. The destructor delivers nothing
/// more, drops queued thumbnail patches (derived data) and carries out the queued structural
/// writes (user intent, durable), whose failures go to the Qt log.
class SnapshotWriter {
public:
    /// @brief Most thumbnail patches written under one lock.
    static constexpr std::size_t maximumBatch = 32;

    /// @brief Starts the thread.
    /// @param onResult Called on the writer thread for each request, in submission order. Must
    /// not throw. No call begins once destruction has begun.
    explicit SnapshotWriter(std::function<void(SnapshotWriteResult)> onResult);

    SnapshotWriter(const SnapshotWriter&) = delete;
    SnapshotWriter& operator=(const SnapshotWriter&) = delete;
    SnapshotWriter(SnapshotWriter&&) = delete;
    SnapshotWriter& operator=(SnapshotWriter&&) = delete;

    /// @brief Stops delivering, drops queued thumbnail patches, carries out the queued structural
    /// writes (each bounded by the lock timeout) and joins.
    ~SnapshotWriter();

    /// @brief Queues a write.
    /// @param write Write to carry out.
    /// @param owner Tag returned with the result.
    /// @return The request's ticket, increasing by one per request.
    std::uint64_t submit(SnapshotWrite write, std::uint64_t owner);

    /// @brief Counts the requests queued or being carried out.
    /// @return Requests still owed.
    [[nodiscard]] std::size_t pending() const;

    /// @brief Sets a function called on the writer thread before each core call.
    ///
    /// A seam for tests, which block in it to hold the writer at a known point; a batch of
    /// thumbnail patches is one call.
    /// @param hook Function, or an empty one for none.
    void setBeforeWriteHook(std::function<void()> hook);

private:
    /// @brief A request in the queue.
    struct Request {
        std::uint64_t ticket = 0;
        std::uint64_t owner = 0;
        SnapshotWrite write;
    };

    /// @brief Serves requests until asked to stop and the queue is empty.
    void run();

    /// @brief Takes the next request, with the thumbnail patches that follow it for the same
    /// photograph, or waits for one.
    /// @return Nothing when it is time to stop.
    [[nodiscard]] std::optional<std::deque<Request>> take();

    /// @brief Carries out a run of requests and delivers their results.
    void carryOut(std::deque<Request> run);

    /// Receiver of results, called on the writer thread.
    std::function<void(SnapshotWriteResult)> onResult_;

    /// Guard of the members below.
    mutable std::mutex mutex_;

    /// Signal that a request is queued or the thread should stop.
    std::condition_variable wake_;

    /// Requests not yet taken.
    std::deque<Request> queue_;

    /// Requests being carried out.
    std::size_t running_ = 0;

    /// Last ticket given.
    std::uint64_t ticket_ = 0;

    /// Whether results are still delivered.
    bool delivering_ = true;

    /// Whether the thread should end once the queue is empty.
    bool stopping_ = false;

    /// Test seam, see setBeforeWriteHook().
    std::function<void()> beforeWrite_;

    /// Declared last so that everything above exists before it starts.
    std::thread thread_;
};

} // namespace arraw::app

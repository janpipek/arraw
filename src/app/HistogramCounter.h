#pragma once

#include "OutputHistogramCount.h"

#include <ColorEncoding.h>
#include <OutputHistogram.h>

#include <QImage>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace arraw::app {

/// @brief Histogram of one submitted picture, with what tells it apart from the others.
struct HistogramCount {
    /// @brief Number ::arraw::app::HistogramCounter::submit returned for the picture.
    std::uint64_t serial = 0;

    /// @brief Generation the picture was submitted in (see
    /// ::arraw::app::HistogramCounter::clear).
    std::uint64_t generation = 0;

    /// @brief The counts.
    OutputHistogram histogram;
};

/// @brief Counts the codes of a picture in an output encoding, on the counter thread.
///
/// The seam ::arraw::app::HistogramCounter's tests replace. Takes the picture, the row spans
/// (empty for whole rows), the encoding and the abandon flag, which it should look at often and
/// stop when it is raised (what it returns then is dropped). May throw; the picture then gets no
/// result.
using HistogramCountFunction = std::function<OutputHistogram(
    const QImage&, std::span<const RowSpan>, NamedEncoding, const std::atomic<bool>&)>;

/// @brief Counts a QImage with ::arraw::countOutputCodes, on its bits, without a copy.
///
/// The counting function a ::arraw::app::HistogramCounter uses unless given another. 8-bit RGB(A),
/// 16-bit RGBA and 32-bit float RGBA layouts are read in place; any other format is converted to
/// `QImage::Format_RGBA8888` first. Reads through `constBits`, so the image is never detached.
/// @param image Picture, its samples in @p encoding.
/// @param spans Empty to count whole rows; else one span per row.
/// @param encoding Output encoding of the samples.
/// @param abandon Flag that stops the count at its next band of rows.
/// @return The counts; an empty histogram for a null image.
/// @throws std::invalid_argument as ::arraw::countOutputCodes does.
[[nodiscard]] OutputHistogram countImageCodes(const QImage& image, std::span<const RowSpan> spans,
                                              NamedEncoding encoding,
                                              const std::atomic<bool>& abandon);

/// @brief Thread that counts the histogram of the picture shown (ADR 055, section 3).
///
/// Owns one std::jthread, idle unless a picture waits. A picture is held as a QImage, a shallow
/// copy whose data is shared, and only read. One job waits at most: a newer submit replaces the
/// waiting one, so the newest picture is the next counted, and never cancels the count under
/// way, which is bounded and would otherwise starve during a drag. clear() and the destructor
/// abandon the count under way at its next band of rows. Uses no Qt signals, so it works without
/// an event loop.
class HistogramCounter {
public:
    /// @brief Receiver of each finished count, on the counter thread; the caller marshals it where
    /// it is needed, and drops it if it is older than what it shows or of an earlier generation.
    /// Must not throw; what it throws is dropped.
    using ResultReceiver = std::function<void(HistogramCount)>;

    /// @brief Starts the thread.
    /// @param onResult Receiver of the counts.
    /// @param count Counting function; empty for ::arraw::app::countImageCodes.
    explicit HistogramCounter(ResultReceiver onResult, HistogramCountFunction count = {});

    HistogramCounter(const HistogramCounter&) = delete;
    HistogramCounter& operator=(const HistogramCounter&) = delete;
    HistogramCounter(HistogramCounter&&) = delete;
    HistogramCounter& operator=(HistogramCounter&&) = delete;

    /// @brief Abandons the count under way, drops the waiting job and waits for the thread.
    ///
    /// The receiver is not called once this returns.
    ~HistogramCounter();

    /// @brief Queues a picture to count, replacing the one waiting, if any. Safe from any thread.
    /// @param image Picture; shared, never copied or written.
    /// @param spans Empty to count whole rows; else one span per row of @p image.
    /// @param encoding Output encoding of the picture's samples.
    /// @return Serial of the job, increasing with each call.
    std::uint64_t submit(QImage image, std::vector<RowSpan> spans, NamedEncoding encoding);

    /// @brief Starts a new generation: drops the waiting job and abandons the count under way,
    /// whose result is then not delivered. Safe from any thread.
    ///
    /// A result of the earlier generation whose delivery has already begun may still arrive, so
    /// the receiver checks the generation too.
    /// @return The new generation.
    std::uint64_t clear();

    /// @brief Tells the current generation. Safe from any thread.
    [[nodiscard]] std::uint64_t generation() const;

private:
    /// @brief Picture waiting to be counted.
    struct Job {
        /// Number submit returned for it.
        std::uint64_t serial = 0;
        /// Generation it was submitted in.
        std::uint64_t generation = 0;
        /// Picture, shared with the submitter.
        QImage image;
        /// Spans of its rows to count; empty for whole rows.
        std::vector<RowSpan> spans;
        /// Encoding of its samples.
        NamedEncoding encoding = NamedEncoding::Srgb;
    };

    /// @brief Counts jobs until asked to stop.
    /// @param stop Raised by the destructor.
    void run(const std::stop_token& stop);

    /// Receiver of the counts, called on the thread.
    ResultReceiver onResult_;

    /// Counting function, called on the thread.
    HistogramCountFunction count_;

    /// Guard of pending_, lastSerial_, generation_ and the abandon_ resets.
    mutable std::mutex mutex_;

    /// Signal that a job waits or the thread should stop.
    std::condition_variable_any wake_;

    /// Newest job not yet started.
    std::optional<Job> pending_;

    /// Serial of the newest job.
    std::uint64_t lastSerial_ = 0;

    /// Current generation.
    std::uint64_t generation_ = 0;

    /// Flag the count under way watches; raised by clear() and the destructor, lowered (under
    /// the guard) when the next job starts.
    std::atomic<bool> abandon_{false};

    /// Declared last so that every member above exists before it starts.
    std::jthread worker_;
};

} // namespace arraw::app

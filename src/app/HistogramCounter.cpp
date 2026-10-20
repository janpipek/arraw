#include "HistogramCounter.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace arraw::app {

namespace {

/// @brief Gives the layout ::arraw::countOutputCodes reads a QImage format as, if it reads it in
/// place.
/// @param format Qt's format.
/// @return The sample layout, or nothing for a format that must be converted first.
std::optional<PixelFormat> pixelFormatOf(QImage::Format format) {
    switch (format) {
    case QImage::Format_RGB888:
        return PixelFormat::RgbU8;
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBX8888:
        return PixelFormat::RgbaU8;
    case QImage::Format_RGBA64:
    case QImage::Format_RGBX64:
        return PixelFormat::RgbaU16;
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBX32FPx4:
        return PixelFormat::RgbaF32;
    default:
        return std::nullopt;
    }
}

} // namespace

OutputHistogram countImageCodes(const QImage& image, std::span<const RowSpan> spans,
                                NamedEncoding encoding, const std::atomic<bool>& abandon) {
    if (image.isNull()) {
        OutputHistogram empty;
        empty.encoding = encoding;
        return empty;
    }
    const auto count = [&](const QImage& readable, PixelFormat format) {
        const ImageSize size{static_cast<std::uint32_t>(readable.width()),
                             static_cast<std::uint32_t>(readable.height())};
        return countOutputCodes(reinterpret_cast<const std::byte*>(readable.constBits()), size,
                                static_cast<std::size_t>(readable.bytesPerLine()), format, encoding,
                                spans, &abandon);
    };
    if (const auto format = pixelFormatOf(image.format())) {
        return count(image, *format);
    }
    return count(image.convertToFormat(QImage::Format_RGBA8888), PixelFormat::RgbaU8);
}

HistogramCounter::HistogramCounter(ResultReceiver onResult, HistogramCountFunction count)
    : onResult_(std::move(onResult)), count_(count ? std::move(count) : countImageCodes),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

HistogramCounter::~HistogramCounter() {
    {
        const std::scoped_lock lock(mutex_);
        pending_.reset();
        abandon_.store(true);
    }
    worker_.request_stop();
    worker_.join();
}

std::uint64_t HistogramCounter::submit(QImage image, std::vector<RowSpan> spans,
                                       NamedEncoding encoding) {
    std::uint64_t serial = 0;
    {
        const std::scoped_lock lock(mutex_);
        serial = ++lastSerial_;
        // The job it replaces is released here, outside the counter thread.
        pending_ = Job{.serial = serial,
                       .generation = generation_,
                       .image = std::move(image),
                       .spans = std::move(spans),
                       .encoding = encoding};
    }
    wake_.notify_one();
    return serial;
}

std::uint64_t HistogramCounter::clear() {
    const std::scoped_lock lock(mutex_);
    pending_.reset();
    // Under the guard, so that it cannot be lowered for a job of the old generation afterwards.
    abandon_.store(true);
    return ++generation_;
}

std::uint64_t HistogramCounter::generation() const {
    const std::scoped_lock lock(mutex_);
    return generation_;
}

void HistogramCounter::run(const std::stop_token& stop) {
    while (true) {
        std::optional<Job> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] { return pending_.has_value(); });
            if (stop.stop_requested()) {
                return;
            }
            job = std::move(pending_);
            pending_.reset();
            // Raised only by clear() and the destructor, both under this guard: a job taken
            // after them is of the new generation, and is counted.
            abandon_.store(false);
        }
        std::optional<OutputHistogram> histogram;
        try {
            histogram = count_(job->image, job->spans, job->encoding, abandon_);
        } catch (...) {
            // A picture that cannot be counted gets no result; the thread goes on.
        }
        const std::uint64_t serial = job->serial;
        const std::uint64_t generation = job->generation;
        // The picture's data is released before the delivery, so the submitter alone holds it.
        job.reset();
        if (!histogram) {
            continue;
        }
        {
            const std::scoped_lock lock(mutex_);
            if (stop.stop_requested()) {
                return;
            }
            if (generation != generation_) {
                continue; // Abandoned by clear(): whatever was counted is partial.
            }
        }
        try {
            onResult_(HistogramCount{
                .serial = serial, .generation = generation, .histogram = std::move(*histogram)});
        } catch (...) {
            // The receiver's failure is not ours to handle, and must not end the thread.
        }
    }
}

} // namespace arraw::app

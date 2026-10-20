#include "HistogramCounter.h"
#include "OutputHistogramCount.h"

#include <OutputHistogram.h>

#include <QColor>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

using namespace arraw;
using namespace std::chrono_literals;

namespace {

/// Bound on every wait, so a hang fails the test instead of the run.
constexpr auto timeout = 60s;

/// Counts delivered by a counter's receiver, collected for the test thread.
class Collector {
public:
    /// Receiver to give the counter.
    [[nodiscard]] app::HistogramCounter::ResultReceiver callback() {
        return [this](app::HistogramCount result) {
            {
                const std::scoped_lock lock(mutex_);
                results_.push_back(std::move(result));
            }
            changed_.notify_all();
        };
    }

    /// Waits until @p count results have arrived.
    [[nodiscard]] bool waitFor(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return results_.size() >= count; });
    }

    [[nodiscard]] std::vector<app::HistogramCount> results() {
        const std::scoped_lock lock(mutex_);
        return results_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<app::HistogramCount> results_;
};

/// @brief A counting function that holds the counter thread until opened or abandoned.
///
/// Makes "a count is under way" a known state instead of a race, as ProgressGate does for
/// renders. Each picture is told apart by its width, which the returned histogram carries as its
/// pixel count.
class CountGate {
public:
    /// One call of the counting function.
    struct Call {
        /// Width of the picture counted.
        int width = 0;
        /// Whether it returned because the count was abandoned.
        bool abandoned = false;
    };

    /// Counting function to give the counter.
    [[nodiscard]] app::HistogramCountFunction function() {
        return [this](const QImage& image, std::span<const RowSpan>, NamedEncoding encoding,
                      const std::atomic<bool>& abandon) {
            std::unique_lock lock(mutex_);
            ++entered_;
            changed_.notify_all();
            // The abandon flag has no notification of its own: polled.
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (closed_ && !abandon.load() && std::chrono::steady_clock::now() < deadline) {
                changed_.wait_for(lock, 1ms);
            }
            calls_.push_back({.width = image.width(), .abandoned = abandon.load()});
            changed_.notify_all();
            OutputHistogram histogram;
            histogram.encoding = encoding;
            histogram.pixels = static_cast<std::uint64_t>(image.width());
            return histogram;
        };
    }

    /// Makes the next counts hold the thread.
    void close() {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
    }

    /// Lets the thread go, and every later count pass.
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            closed_ = false;
        }
        changed_.notify_all();
    }

    /// Waits until @p count calls have begun.
    [[nodiscard]] bool waitEntered(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return entered_ >= count; });
    }

    /// Waits until @p count calls have returned.
    [[nodiscard]] bool waitReturned(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return calls_.size() >= count; });
    }

    [[nodiscard]] std::vector<Call> calls() {
        const std::scoped_lock lock(mutex_);
        return calls_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_ = false;
    std::size_t entered_ = 0;
    std::vector<Call> calls_;
};

/// @brief Makes an opaque picture of a given width, told apart by it.
QImage pictureOfWidth(int width) {
    QImage image(width, 2, QImage::Format_RGBA8888);
    image.fill(QColor(10, 20, 30));
    return image;
}

/// @brief Makes a 4x3 picture whose pixels all differ.
QImage distinctPicture() {
    QImage image(4, 3, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(x * 60, y * 100, 255 - x * 20 - y * 10));
        }
    }
    return image;
}

/// @brief Counts a picture's bits directly, as the counter's default function should.
OutputHistogram countDirectly(const QImage& image, std::span<const RowSpan> spans = {}) {
    return countOutputCodes(
        reinterpret_cast<const std::byte*>(image.constBits()),
        {static_cast<std::uint32_t>(image.width()), static_cast<std::uint32_t>(image.height())},
        static_cast<std::size_t>(image.bytesPerLine()), PixelFormat::RgbaU8, NamedEncoding::Srgb,
        spans);
}

} // namespace

TEST_CASE("a submitted image is counted and reported with its serial", "[HistogramCounter]") {
    Collector collector;
    app::HistogramCounter counter(collector.callback());
    const QImage picture = distinctPicture();

    const std::uint64_t serial = counter.submit(picture, {}, NamedEncoding::Srgb);

    REQUIRE(collector.waitFor(1));
    const auto results = collector.results();
    REQUIRE(results.size() == 1);
    CHECK(results[0].serial == serial);
    CHECK(results[0].generation == counter.generation());
    CHECK(results[0].histogram.pixels == 12);
    CHECK(results[0].histogram == countDirectly(picture));
}

TEST_CASE("while one count runs, only the newest pending job is counted next",
          "[HistogramCounter]") {
    Collector collector;
    CountGate gate;
    gate.close();
    app::HistogramCounter counter(collector.callback(), gate.function());

    const std::uint64_t first = counter.submit(pictureOfWidth(1), {}, NamedEncoding::Srgb);
    REQUIRE(gate.waitEntered(1));
    counter.submit(pictureOfWidth(2), {}, NamedEncoding::Srgb);
    counter.submit(pictureOfWidth(3), {}, NamedEncoding::Srgb);
    const std::uint64_t newest = counter.submit(pictureOfWidth(4), {}, NamedEncoding::Srgb);
    gate.open();

    REQUIRE(collector.waitFor(2));
    // A later job, counted after these two: nothing replaced in between comes before it.
    const std::uint64_t last = counter.submit(pictureOfWidth(5), {}, NamedEncoding::Srgb);
    REQUIRE(collector.waitFor(3));

    const auto results = collector.results();
    REQUIRE(results.size() == 3);
    CHECK(results[0].serial == first);
    CHECK(results[0].histogram.pixels == 1);
    CHECK(results[1].serial == newest);
    CHECK(results[1].histogram.pixels == 4);
    CHECK(results[2].serial == last);
    CHECK(results[2].histogram.pixels == 5);
    CHECK(first < newest);
    CHECK(newest < last);
    CHECK(gate.calls().size() == 3);
}

TEST_CASE("a running count is not cancelled by a newer job", "[HistogramCounter]") {
    Collector collector;
    CountGate gate;
    gate.close();
    app::HistogramCounter counter(collector.callback(), gate.function());

    const std::uint64_t first = counter.submit(pictureOfWidth(1), {}, NamedEncoding::Srgb);
    REQUIRE(gate.waitEntered(1));
    const std::uint64_t second = counter.submit(pictureOfWidth(2), {}, NamedEncoding::Srgb);
    gate.open();

    REQUIRE(collector.waitFor(2));
    const auto calls = gate.calls();
    REQUIRE(calls.size() == 2);
    CHECK_FALSE(calls[0].abandoned);
    const auto results = collector.results();
    CHECK(results[0].serial == first);
    CHECK(results[0].histogram.pixels == 1);
    CHECK(results[1].serial == second);
}

TEST_CASE("clear abandons the running count and drops its result", "[HistogramCounter]") {
    Collector collector;
    CountGate gate;
    gate.close();
    app::HistogramCounter counter(collector.callback(), gate.function());
    const std::uint64_t before = counter.generation();

    counter.submit(pictureOfWidth(1), {}, NamedEncoding::Srgb);
    REQUIRE(gate.waitEntered(1));
    counter.submit(pictureOfWidth(2), {}, NamedEncoding::Srgb); // Waiting; dropped by clear.
    const std::uint64_t generation = counter.clear();
    CHECK(generation == before + 1);
    CHECK(counter.generation() == generation);

    // The gate stays closed: only the abandon flag lets the count return.
    REQUIRE(gate.waitReturned(1));
    CHECK(gate.calls()[0].abandoned);

    gate.open();
    const std::uint64_t after = counter.submit(pictureOfWidth(3), {}, NamedEncoding::Srgb);
    REQUIRE(collector.waitFor(1));
    REQUIRE(gate.waitReturned(2));

    const auto results = collector.results();
    REQUIRE(results.size() == 1);
    CHECK(results[0].serial == after);
    CHECK(results[0].generation == generation);
    CHECK(results[0].histogram.pixels == 3);
    const auto calls = gate.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].width == 3);
    CHECK_FALSE(calls[1].abandoned);
}

TEST_CASE("the destructor returns promptly during a count", "[HistogramCounter]") {
    Collector collector;
    CountGate gate;
    gate.close();
    auto counter = std::make_unique<app::HistogramCounter>(collector.callback(), gate.function());

    counter->submit(pictureOfWidth(1), {}, NamedEncoding::Srgb);
    REQUIRE(gate.waitEntered(1));
    counter->submit(pictureOfWidth(2), {}, NamedEncoding::Srgb);

    const auto start = std::chrono::steady_clock::now();
    counter.reset();
    const auto took = std::chrono::steady_clock::now() - start;

    // The gate's own limit is a minute: anything near a second means the flag did not stop it.
    CHECK(took < 2s);
    const auto calls = gate.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].abandoned);
    CHECK(collector.results().empty());
}

TEST_CASE("the submitted image is not detached", "[HistogramCounter]") {
    Collector collector;
    const QImage picture = distinctPicture();
    const qint64 key = picture.cacheKey();
    const uchar* bits = picture.constBits();
    std::mutex mutex;
    qint64 seenKey = 0;
    const uchar* seenBits = nullptr;
    app::HistogramCounter counter(
        collector.callback(), [&](const QImage& image, std::span<const RowSpan> spans,
                                  NamedEncoding encoding, const std::atomic<bool>& abandon) {
            auto histogram = app::countImageCodes(image, spans, encoding, abandon);
            const std::scoped_lock lock(mutex);
            seenKey = image.cacheKey();
            seenBits = image.constBits();
            return histogram;
        });

    counter.submit(picture, {}, NamedEncoding::Srgb);
    REQUIRE(collector.waitFor(1));

    CHECK(picture.cacheKey() == key);
    CHECK(picture.constBits() == bits);
    const std::scoped_lock lock(mutex);
    CHECK(seenKey == key);
    CHECK(seenBits == bits);
}

TEST_CASE("spans reach the count", "[HistogramCounter]") {
    Collector collector;
    app::HistogramCounter counter(collector.callback());
    const QImage picture = distinctPicture();
    const std::vector<RowSpan> spans{{.begin = 0, .end = 2}, {}, {.begin = 1, .end = 4}};

    counter.submit(picture, spans, NamedEncoding::Srgb);
    REQUIRE(collector.waitFor(1));

    const auto results = collector.results();
    REQUIRE(results.size() == 1);
    CHECK(results[0].histogram.pixels == 5);
    CHECK(results[0].histogram == countDirectly(picture, spans));
}

TEST_CASE("a count that throws gets no result and the thread goes on", "[HistogramCounter]") {
    Collector collector;
    std::atomic<int> thrown{0};
    app::HistogramCounter counter(collector.callback(),
                                  [&](const QImage& image, std::span<const RowSpan>,
                                      NamedEncoding encoding, const std::atomic<bool>&) {
                                      if (image.width() == 1) {
                                          thrown.store(1);
                                          thrown.notify_all();
                                          throw std::invalid_argument("refused");
                                      }
                                      OutputHistogram histogram;
                                      histogram.encoding = encoding;
                                      histogram.pixels = static_cast<std::uint64_t>(image.width());
                                      return histogram;
                                  });

    counter.submit(pictureOfWidth(1), {}, NamedEncoding::Srgb);
    // Submitted only once the first has thrown, so that it does not replace the first.
    thrown.wait(0);
    const std::uint64_t serial = counter.submit(pictureOfWidth(2), {}, NamedEncoding::Srgb);
    REQUIRE(collector.waitFor(1));

    const auto results = collector.results();
    REQUIRE(results.size() == 1);
    CHECK(results[0].serial == serial);
}

#include "ExportQueue.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"
#include "support/TestImages.h"

#include <ExifInfo.h>
#include <ImageImport.h>

#include <QImage>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

using namespace arraw;
using namespace std::chrono_literals;

namespace {

/// Bound on every wait, so a hang fails the test instead of the run.
constexpr auto timeout = 60s;

/// Results delivered by a queue's callback, collected for the test thread.
class Collector {
public:
    /// Callback to give the queue.
    [[nodiscard]] std::function<void(app::ExportResult)> callback() {
        return [this](app::ExportResult result) {
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

    [[nodiscard]] std::vector<app::ExportResult> results() {
        const std::scoped_lock lock(mutex_);
        return results_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<app::ExportResult> results_;
};

std::shared_ptr<const ImageBuffer> makeSource() {
    return std::make_shared<const ImageBuffer>(
        test::rainbow({64, 32}, PixelFormat::RgbaF32, workingEncoding));
}

app::ExportJob jobTo(const std::filesystem::path& path, ImageFileFormat format) {
    app::ExportJob job;
    job.source = makeSource();
    job.options.format = format;
    job.path = path;
    return job;
}

QImage load(const std::filesystem::path& path) {
    return QImage(QString::fromStdU16String(path.u16string()));
}

} // namespace

TEST_CASE("Exports run in order and write files that load back", "[app][export][queue]") {
    const test::TempDir dir;
    Collector collector;
    app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Cpu);

    const auto png = dir.file("a.png");
    const auto tiff = dir.file("b.tif");
    app::ExportJob resized = jobTo(tiff, ImageFileFormat::Tiff);
    resized.request.size = RenderRequest::FitInside{32, 32};
    const auto first = queue.enqueue(jobTo(png, ImageFileFormat::Png));
    const auto second = queue.enqueue(std::move(resized));

    REQUIRE(collector.waitFor(2));
    const auto results = collector.results();
    REQUIRE(results[0].id == first);
    REQUIRE(results[1].id == second);
    REQUIRE(results[0].error.empty());
    REQUIRE(results[1].error.empty());
    REQUIRE_FALSE(results[0].onGpu);
    REQUIRE(results[0].path == png);

    const QImage a = load(png);
    REQUIRE_FALSE(a.isNull());
    REQUIRE(a.width() == 64);
    REQUIRE(a.height() == 32);
    const QImage b = load(tiff);
    REQUIRE_FALSE(b.isNull());
    REQUIRE(b.width() == 32);
    REQUIRE(b.height() == 16);
}

TEST_CASE("An export job carries the chosen metadata", "[app][export][queue][metadata]") {
    const test::TempDir dir;
    Collector collector;
    app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Cpu);

    const auto carried = dir.file("carried.jpg");
    const auto location = dir.file("location.jpg");
    const auto none = dir.file("none.jpg");
    const auto source = test::fixture("exif-32x24.dng");
    app::ExportJob a = jobTo(carried, ImageFileFormat::Jpeg);
    a.metadata = ExportMetadata{.source = source};
    app::ExportJob b = jobTo(location, ImageFileFormat::Jpeg);
    b.metadata = ExportMetadata{.source = source, .selection = {.location = true}};
    app::ExportJob c = jobTo(none, ImageFileFormat::Jpeg);
    c.metadata = ExportMetadata{
        .source = source, .selection = {.capture = false, .location = false, .descriptive = false}};
    queue.enqueue(std::move(a));
    queue.enqueue(std::move(b));
    queue.enqueue(std::move(c));

    REQUIRE(collector.waitFor(3));
    for (const auto& result : collector.results()) {
        REQUIRE(result.error.empty());
    }
    const ExifInfo info = readExif(carried);
    REQUIRE(info.model == "Fixture One");
    REQUIRE_FALSE(info.gps);
    REQUIRE(readExif(location).gps);
    const ExifInfo empty = readExif(none);
    REQUIRE_FALSE(empty.make);
    REQUIRE_FALSE(empty.model);
    REQUIRE_FALSE(empty.gps);
}

TEST_CASE("An export whose metadata cannot be read succeeds and carries the warning",
          "[app][export][queue][metadata]") {
    const test::TempDir dir;
    Collector collector;
    app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Cpu);
    const auto path = dir.file("out.jpg");
    app::ExportJob job = jobTo(path, ImageFileFormat::Jpeg);
    job.metadata = ExportMetadata{.source = dir.file("absent.dng")};
    queue.enqueue(std::move(job));
    app::ExportJob fine = jobTo(dir.file("fine.jpg"), ImageFileFormat::Jpeg);
    queue.enqueue(std::move(fine));

    REQUIRE(collector.waitFor(2));
    const auto results = collector.results();
    REQUIRE(results[0].error.empty());
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(results[0].warnings.size() == 1);
    REQUIRE(results[0].warnings.front().find("without some metadata") != std::string::npos);
    REQUIRE(results[1].warnings.empty());
}

TEST_CASE("A failing export reports, and the next one still runs", "[app][export][queue]") {
    const test::TempDir dir;
    Collector collector;
    app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Cpu);

    (void)queue.enqueue(jobTo(dir.path() / "missing" / "a.png", ImageFileFormat::Png));
    (void)queue.enqueue(jobTo(dir.file("b.png"), ImageFileFormat::Png));

    REQUIRE(collector.waitFor(2));
    const auto results = collector.results();
    REQUIRE_FALSE(results[0].error.empty());
    REQUIRE(results[1].error.empty());
    REQUIRE_FALSE(load(dir.file("b.png")).isNull());
}

TEST_CASE("A job without a photograph fails rather than crashing", "[app][export][queue]") {
    const test::TempDir dir;
    Collector collector;
    app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Cpu);
    app::ExportJob job = jobTo(dir.file("a.png"), ImageFileFormat::Png);
    job.source.reset();

    (void)queue.enqueue(std::move(job));

    REQUIRE(collector.waitFor(1));
    REQUIRE_FALSE(collector.results()[0].error.empty());
}

TEST_CASE("Dropping the queued jobs leaves the others", "[app][export][queue]") {
    const test::TempDir dir;
    Collector collector;
    std::size_t dropped = 0;
    {
        app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Cpu);
        for (int i = 0; i < 20; ++i) {
            (void)queue.enqueue(
                jobTo(dir.file("p" + std::to_string(i) + ".png"), ImageFileFormat::Png));
        }
        dropped = queue.cancelQueued();
        REQUIRE(dropped <= 20);
        // Those not dropped have started or are about to; each still reports.
        REQUIRE(collector.waitFor(20 - dropped));
    }
    REQUIRE(collector.results().size() == 20 - dropped);
}

TEST_CASE("Destroying a queue with jobs pending returns and leaves no partial file",
          "[app][export][queue]") {
    const test::TempDir dir;
    Collector collector;
    {
        app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Cpu);
        for (int i = 0; i < 20; ++i) {
            (void)queue.enqueue(
                jobTo(dir.file("p" + std::to_string(i) + ".png"), ImageFileFormat::Png));
        }
    }
    const std::size_t reported = collector.results().size();
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        // A temporary of an unfinished write would not be named *.png.
        REQUIRE(entry.path().extension() == ".png");
        REQUIRE_FALSE(load(entry.path()).isNull());
        ++files;
    }
    REQUIRE(files == reported);
    REQUIRE(reported < 20);
}

TEST_CASE("Desktop CPU preference overrides automatic export rendering",
          "[app][export][queue][settings]") {
    const test::TempDir directory;
    Collector collector;
    app::ExportQueue queue(collector.callback(), app::ExportQueue::Device::Auto,
                           {.cpuOnly = true, .gpu = std::nullopt});
    const auto path = directory.file("cpu.png");
    queue.enqueue(jobTo(path, ImageFileFormat::Png));
    REQUIRE(collector.waitFor(1));
    const auto result = collector.results().front();
    REQUIRE(result.error.empty());
    REQUIRE_FALSE(result.onGpu);
    REQUIRE(result.fallbackReason.empty());
    REQUIRE_FALSE(load(path).isNull());
}

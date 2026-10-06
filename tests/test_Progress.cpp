#include "ProgressScope.h"
#include "RowBands.h"
#include "support/TestImages.h"

#include <CurveHistogram.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageImport.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;

/// The out-of-band channel of a decode or a render: progress out, cancellation in (ADR 042).

namespace {

/// @brief A source large enough that every loop runs in several chunks.
ImageBuffer makeSource() {
    return test::rainbow({480, 320}, PixelFormat::RgbaF32, workingEncoding);
}

/// @brief Settings that run every step a render has.
DevelopState everyStep() {
    DevelopState state;
    state.settings.noiseReduction.luminance = 60.0F;
    state.settings.noiseReduction.color = 40.0F;
    state.settings.presence.clarity = 30.0F;
    state.settings.presence.texture = 20.0F;
    state.settings.presence.dehaze = 25.0F;
    state.settings.geometry.straighten = 3.0;
    state.settings.geometry.crop.rectangle = UprightCropRect{0.05, 0.1, 0.9, 0.95};
    state.settings.effects.vignette.amount = -30.0F;
    state.settings.effects.grain.amount = 25.0F;
    return state;
}

/// @brief A request that resizes, so that the resize step runs.
RenderRequest resized() {
    RenderRequest request;
    request.size = RenderRequest::FitInside{300, 300};
    return request;
}

/// @brief Whether two buffers hold the same bits.
bool sameBits(const ImageBuffer& left, const ImageBuffer& right) {
    const auto a = left.samples<float>();
    const auto b = right.samples<float>();
    return left.size() == right.size() && a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

/// @brief Whether two decoded buffers hold the same samples, whatever their type.
bool sameDecode(const ImageBuffer& left, const ImageBuffer& right) {
    if (left.size() != right.size() || left.format() != right.format()) {
        return false;
    }
    const auto same = [&]<typename Sample>() {
        const auto a = left.samples<Sample>();
        const auto b = right.samples<Sample>();
        return std::ranges::equal(a, b);
    };
    switch (left.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        return same.template operator()<std::uint8_t>();
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        return same.template operator()<std::uint16_t>();
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        break;
    }
    return same.template operator()<float>();
}

/// @brief Reports of one or more operations, in the order they came.
struct Recorder {
    std::vector<Progress> reports;

    [[nodiscard]] ProgressChannel::Callback callback() {
        return [this](const Progress& progress) { reports.push_back(progress); };
    }
};

/// @brief Requires a stream of reports to be monotone, in step order, and to end at one.
void requireWellFormed(const std::vector<Progress>& reports) {
    REQUIRE_FALSE(reports.empty());
    for (std::size_t index = 1; index < reports.size(); ++index) {
        INFO("report " << index);
        REQUIRE(reports[index].fraction >= reports[index - 1].fraction);
        REQUIRE(static_cast<int>(reports[index].step) >= static_cast<int>(reports[index - 1].step));
    }
    REQUIRE(reports.front().fraction >= 0.0);
    REQUIRE(reports.back().fraction == 1.0);
}

} // namespace

TEST_CASE("A render with a channel gives the bits of one without", "[progress][develop]") {
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();
    const ImageBuffer unobserved = develop(source, state, resized());
    ProgressChannel channel([](const Progress&) {});
    const ImageBuffer observed = develop(source, state, resized(), &channel);
    REQUIRE(sameBits(unobserved, observed));

    // A channel without a callback only carries cancellation, and changes nothing either.
    ProgressChannel quiet;
    REQUIRE(sameBits(unobserved, develop(source, state, resized(), &quiet)));
}

TEST_CASE("Progress rises through the steps in order and ends at one", "[progress][develop]") {
    const ImageBuffer source = makeSource();
    Recorder recorder;
    ProgressChannel channel(recorder.callback());
    static_cast<void>(develop(source, everyStep(), resized(), &channel));
    requireWellFormed(recorder.reports);

    // Every step the plan runs is named on the way.
    std::vector<ProgressStep> steps;
    for (const Progress& report : recorder.reports) {
        if (steps.empty() || steps.back() != report.step) {
            steps.push_back(report.step);
        }
    }
    REQUIRE(steps == std::vector{ProgressStep::Denoise, ProgressStep::Context,
                                 ProgressStep::Pointwise, ProgressStep::Geometry,
                                 ProgressStep::Resize, ProgressStep::Effects});
}

TEST_CASE("A render with nothing on reports its start and its end", "[progress][develop]") {
    Recorder recorder;
    ProgressChannel channel(recorder.callback());
    static_cast<void>(develop(makeSource(), {}, {}, &channel));
    requireWellFormed(recorder.reports);
    REQUIRE(recorder.reports.front().fraction == 0.0);
}

TEST_CASE("Reports are thinned to visible steps", "[progress][develop]") {
    Recorder recorder;
    ProgressChannel channel(recorder.callback());
    static_cast<void>(develop(makeSource(), everyStep(), resized(), &channel));
    const auto& reports = recorder.reports;
    // At most one report per 1/512 of the whole, plus the step changes and the end.
    REQUIRE(reports.size() <= 512 + 2 * progressStepCount + 2);
    for (std::size_t index = 1; index < reports.size(); ++index) {
        const bool stepChanged = reports[index].step != reports[index - 1].step;
        const bool last = index + 1 == reports.size();
        if (!stepChanged && !last) {
            INFO("report " << index);
            REQUIRE(reports[index].fraction - reports[index - 1].fraction >= 1.0 / 512.0);
        }
    }
}

TEST_CASE("A cancelled channel stops a render before it starts", "[progress][cancel]") {
    Recorder recorder;
    ProgressChannel channel(recorder.callback());
    channel.cancel();
    REQUIRE_THROWS_AS(develop(makeSource(), everyStep(), resized(), &channel), Cancelled);
    REQUIRE(recorder.reports.empty());
    REQUIRE(detail::currentProgress() == nullptr);
}

TEST_CASE("A cancellation during a render stops it, and nothing reports after",
          "[progress][cancel]") {
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();
    // Stopped in each step in turn, the first report of it cancelling.
    for (const ProgressStep at :
         {ProgressStep::Denoise, ProgressStep::Context, ProgressStep::Pointwise,
          ProgressStep::Geometry, ProgressStep::Resize, ProgressStep::Effects}) {
        INFO("cancelled in step " << static_cast<int>(at));
        std::vector<Progress> reports;
        std::size_t cancelledAt = 0;
        ProgressChannel* self = nullptr;
        ProgressChannel channel([&](const Progress& progress) {
            reports.push_back(progress);
            if (progress.step == at && cancelledAt == 0) {
                cancelledAt = reports.size();
                self->cancel();
            }
        });
        self = &channel;
        REQUIRE_THROWS_AS(develop(source, state, resized(), &channel), Cancelled);
        REQUIRE(cancelledAt != 0);
        REQUIRE(reports.size() == cancelledAt);
        REQUIRE(reports.back().fraction < 1.0);
        REQUIRE(detail::currentProgress() == nullptr);
    }
    // And the engine is as it was: an unobserved render after gives the same bits.
    REQUIRE(sameBits(develop(source, state, resized()), develop(source, state, resized())));
}

TEST_CASE("A channel cancelled after a render finished leaves its result, and stops the next",
          "[progress][cancel]") {
    const ImageBuffer source = makeSource();
    ProgressChannel channel;
    const ImageBuffer done = develop(source, {}, {}, &channel);
    channel.cancel();
    REQUIRE(sameBits(done, develop(source, {}, {})));
    REQUIRE_THROWS_AS(develop(source, {}, {}, &channel), Cancelled);
    REQUIRE_THROWS_AS(developUntil(source, {}, Stage::Pointwise, {}, &channel), Cancelled);
}

TEST_CASE("A callback that throws stops the render with what it threw", "[progress][develop]") {
    int calls = 0;
    ProgressChannel channel([&](const Progress& progress) {
        ++calls;
        if (progress.step == ProgressStep::Pointwise) {
            throw std::runtime_error("from the callback");
        }
    });
    try {
        static_cast<void>(develop(makeSource(), everyStep(), resized(), &channel));
        FAIL("the render did not stop");
    } catch (const Cancelled&) {
        FAIL("a callback's exception came out as a cancellation");
    } catch (const std::runtime_error& error) {
        REQUIRE(std::string(error.what()) == "from the callback");
    }
    REQUIRE(calls > 0);
    REQUIRE(detail::currentProgress() == nullptr);
}

TEST_CASE("A chain of resumes on one channel reads as one render", "[progress][resume]") {
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();
    const RenderRequest request = resized();
    Recorder recorder;
    ProgressChannel channel(recorder.callback());

    std::vector<std::size_t> starts;
    starts.push_back(recorder.reports.size());
    const RenderCheckpoint denoised =
        developUntil(source, state, Stage::Denoise, request, &channel);
    starts.push_back(recorder.reports.size());
    const RenderCheckpoint pointwise =
        resumeFrom(denoised, source, state, Stage::Pointwise, request, &channel);
    starts.push_back(recorder.reports.size());
    const RenderCheckpoint geometry =
        resumeFrom(pointwise, source, state, Stage::Geometry, request, &channel);
    starts.push_back(recorder.reports.size());
    const RenderCheckpoint resizedPixels =
        resumeFrom(geometry, source, state, Stage::Resize, request, &channel);
    starts.push_back(recorder.reports.size());
    const RenderCheckpoint effects =
        resumeFrom(resizedPixels, source, state, Stage::Effects, request, &channel);

    // One monotone stream, each call ending short of one until the last.
    requireWellFormed(recorder.reports);
    for (std::size_t call = 1; call < starts.size(); ++call) {
        INFO("call " << call);
        REQUIRE(starts[call] > starts[call - 1]);
        REQUIRE(recorder.reports[starts[call] - 1].fraction < 1.0);
        // Each resume starts where the call before it ended.
        REQUIRE(recorder.reports[starts[call]].fraction ==
                recorder.reports[starts[call] - 1].fraction);
    }
    REQUIRE(sameBits(effects.readBack(), develop(source, state, request)));
}

TEST_CASE("A cancelled resume leaves the checkpoint it started from usable", "[progress][resume]") {
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();
    const RenderCheckpoint pointwise = developUntil(source, state, Stage::Pointwise, resized());
    ProgressChannel* self = nullptr;
    ProgressChannel channel([&](const Progress& progress) {
        if (progress.step == ProgressStep::Geometry && progress.fraction > 0.0) {
            self->cancel();
        }
    });
    self = &channel;
    REQUIRE_THROWS_AS(resumeFrom(pointwise, source, state, Stage::Effects, resized(), &channel),
                      Cancelled);
    const RenderCheckpoint done = resumeFrom(pointwise, source, state, Stage::Effects, resized());
    REQUIRE(sameBits(done.readBack(), develop(source, state, resized())));
}

TEST_CASE("A sample and a histogram report and can be cancelled", "[progress][sample]") {
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();
    Recorder recorder;
    ProgressChannel channel(recorder.callback());
    const ImageBuffer observed = sample(source, state, Tap::CurveInput, resized(), &channel);
    requireWellFormed(recorder.reports);
    REQUIRE(sameBits(observed, sample(source, state, Tap::CurveInput, resized())));

    Recorder counted;
    ProgressChannel histogramChannel(counted.callback());
    static_cast<void>(curveHistogram(source, state, curveHistogramRequest, &histogramChannel));
    requireWellFormed(counted.reports);
    histogramChannel.cancel();
    REQUIRE_THROWS_AS(curveHistogram(source, state, curveHistogramRequest, &histogramChannel),
                      Cancelled);
}

TEST_CASE("A decode reports its start and its end, and a cancelled one stops",
          "[progress][decode]") {
    const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
    for (const char* name : {"bayer-32x24.dng", "testcard-61x41-srgb8.png"}) {
        INFO(name);
        Recorder recorder;
        ProgressChannel channel(recorder.callback());
        const ImageBuffer observed =
            loadImage(fixtures / name, discardedDiagnostics(), {}, &channel);
        requireWellFormed(recorder.reports);
        REQUIRE(recorder.reports.front().fraction == 0.0);
        REQUIRE(std::ranges::all_of(recorder.reports, [](const Progress& progress) {
            return progress.step == ProgressStep::Decode;
        }));
        const ImageBuffer unobserved = loadImage(fixtures / name);
        REQUIRE(sameDecode(observed, unobserved));

        ProgressChannel cancelled;
        cancelled.cancel();
        REQUIRE_THROWS_AS(loadImage(fixtures / name, discardedDiagnostics(), {}, &cancelled),
                          Cancelled);
    }
}

TEST_CASE("An unobserved call inside an observed one reports nothing into it",
          "[progress][scope]") {
    Recorder recorder;
    ProgressChannel channel(recorder.callback());
    detail::StepWeights weights{};
    weights[static_cast<std::size_t>(ProgressStep::Pointwise)] = 1.0;
    {
        detail::ProgressRoot outer(&channel, weights, ProgressStep::Pointwise);
        const std::size_t before = recorder.reports.size();
        // A nested public call, passed no channel, as sample calls develop's passes.
        static_cast<void>(develop(makeSource(), everyStep(), resized()));
        REQUIRE(recorder.reports.size() == before);
        REQUIRE(detail::currentProgress() != nullptr);
        outer.finish(ProgressStep::Pointwise);
    }
    REQUIRE(detail::currentProgress() == nullptr);
    REQUIRE(recorder.reports.back().fraction == 1.0);
}

TEST_CASE("An observed loop covers every row once, on every band", "[progress][scope]") {
    constexpr std::uint32_t rows = 2000;
    constexpr std::uint32_t width = 1000;
    ProgressChannel channel([](const Progress&) {});
    detail::StepWeights weights{};
    weights[static_cast<std::size_t>(ProgressStep::Pointwise)] = 1.0;
    std::vector<int> seen(rows, 0);
    {
        detail::ProgressRoot root(&channel, weights, ProgressStep::Pointwise);
        const detail::ProgressSpan span(ProgressStep::Pointwise);
        detail::forEachRowBand(rows, width, [&](std::uint32_t first, std::uint32_t last) {
            // Unobserved inside, on the calling thread as on the workers.
            REQUIRE(detail::currentProgress() == nullptr);
            for (std::uint32_t row = first; row < last; ++row) {
                ++seen[row];
            }
        });
    }
    REQUIRE(std::ranges::all_of(seen, [](int count) { return count == 1; }));
}

TEST_CASE("An observed loop whose band fails throws what the band threw", "[progress][scope]") {
    constexpr std::uint32_t rows = 4000;
    constexpr std::uint32_t width = 1000;
    ProgressChannel channel;
    detail::StepWeights weights{};
    weights[static_cast<std::size_t>(ProgressStep::Pointwise)] = 1.0;
    std::atomic<std::uint32_t> covered{0};
    {
        detail::ProgressRoot root(&channel, weights, ProgressStep::Pointwise);
        const detail::ProgressSpan span(ProgressStep::Pointwise);
        REQUIRE_THROWS_AS(detail::forEachRowBand(rows, width,
                                                 [&](std::uint32_t first, std::uint32_t last) {
                                                     if (first == 0) {
                                                         throw std::logic_error("band 0");
                                                     }
                                                     covered += last - first;
                                                 }),
                          std::logic_error);
    }
    REQUIRE(covered.load() < rows);
    REQUIRE(detail::currentProgress() == nullptr);
}

TEST_CASE("Each pass of a CPU render runs the loops its span declares", "[progress][develop]") {
    // A debug build asserts that a span ran exactly the units it declared; each
    // case here runs a different set of loops.
    const ImageBuffer small = test::rainbow({160, 120}, PixelFormat::RgbaF32, workingEncoding);
    const ImageBuffer sixteenBit = test::rainbow({160, 120}, PixelFormat::RgbaU16, workingEncoding);
    const auto run = [](const ImageBuffer& source, const DevelopState& state) {
        Recorder recorder;
        ProgressChannel channel(recorder.callback());
        static_cast<void>(develop(source, state, resized(), &channel));
        requireWellFormed(recorder.reports);
    };
    DevelopState state;
    state.settings.noiseReduction.color = 40.0F;
    run(small, state);
    run(sixteenBit, state);
    state = {};
    state.settings.noiseReduction.luminance = 40.0F;
    run(sixteenBit, state);
    state = {};
    state.settings.presence.texture = 40.0F;
    run(small, state);
    state = {};
    state.settings.presence.dehaze = -40.0F;
    run(small, state);
    state = {};
    state.settings.presence.clarity = 40.0F;
    state.settings.geometry.rotation = QuarterTurn::Clockwise90;
    run(sixteenBit, state);
}

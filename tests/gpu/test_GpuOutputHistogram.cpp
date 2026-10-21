#include "EncodedForOutput.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "support/RenderDigest.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <LensCorrection.h>
#include <OutputHistogram.h>

#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::test;

namespace {

/// @brief Share of the pixels that may move by one code between the CPU and the GPU (ADR 055,
/// section 8; ADR 035 allowed the same).
constexpr double movedShare = 0.01;

/// @brief Counts how many pixels one histogram must move to become the other, one bin at a time.
///
/// The sum over the bin boundaries of the net count that crosses it: the earth mover's distance,
/// which is the number of pixels that moved when each moved one bin.
/// @param a First channel.
/// @param b Second channel.
/// @return The distance, in pixel-bins.
std::uint64_t binDistance(const OutputHistogram::Bins& a, const OutputHistogram::Bins& b) {
    std::int64_t running = 0;
    std::uint64_t total = 0;
    for (std::size_t bin = 0; bin < a.size(); ++bin) {
        running += static_cast<std::int64_t>(a[bin]) - static_cast<std::int64_t>(b[bin]);
        total += static_cast<std::uint64_t>(std::abs(running));
    }
    return total;
}

/// @brief Sums one bin over the three colour channels, an upper bound for the pixels in it.
std::uint64_t colourBin(const OutputHistogram& histogram, std::size_t bin) {
    return histogram.red[bin] + histogram.green[bin] + histogram.blue[bin];
}

/// @brief Requires a clipped count to differ only by pixels one code from the end in the other.
void requireClippedAgreement(std::uint64_t cpu, std::uint64_t gpu, std::uint64_t cpuNear,
                             std::uint64_t gpuNear) {
    CAPTURE(cpu, gpu, cpuNear, gpuNear);
    if (gpu >= cpu) {
        REQUIRE(gpu - cpu <= cpuNear);
    } else {
        REQUIRE(cpu - gpu <= gpuNear);
    }
}

/// @brief Requires the GPU's histogram to agree with the CPU's within the allowance.
void requireAgreement(const OutputHistogram& cpu, const OutputHistogram& gpu) {
    REQUIRE(cpu.pixels == gpu.pixels);
    const auto allowed = static_cast<std::uint64_t>(movedShare * static_cast<double>(cpu.pixels));
    const std::array channels{&OutputHistogram::luma, &OutputHistogram::red,
                              &OutputHistogram::green, &OutputHistogram::blue};
    for (const auto channel : channels) {
        const std::uint64_t moved = binDistance(cpu.*channel, gpu.*channel);
        CAPTURE(moved, allowed);
        REQUIRE(moved <= allowed);
    }
    requireClippedAgreement(cpu.highlightsClipped, gpu.highlightsClipped, colourBin(cpu, 254),
                            colourBin(gpu, 254));
    requireClippedAgreement(cpu.shadowsClipped, gpu.shadowsClipped, colourBin(cpu, 1),
                            colourBin(gpu, 1));
}

/// @brief Requires, pixel by pixel, that the two 8-bit pictures differ by at most one code in
/// any channel, and that at most `movedShare` of the samples of each channel differ at all.
///
/// This is the wording of ADR 055, section 8, which the histograms alone cannot enforce: a
/// histogram cannot tell pixels that moved two codes from pixels that moved one.
void requirePixelsWithinOneCode(const ImageBuffer& cpuRender, const ImageBuffer& gpuRender) {
    const QImage cpu = encodedForOutput(cpuRender, NamedEncoding::Srgb, QImage::Format_RGBA8888);
    const QImage gpu = encodedForOutput(gpuRender, NamedEncoding::Srgb, QImage::Format_RGBA8888);
    REQUIRE(cpu.size() == gpu.size());
    std::array<std::uint64_t, 3> moved{};
    std::uint64_t twoOrMore = 0;
    for (int y = 0; y < cpu.height(); ++y) {
        const uchar* a = cpu.constScanLine(y);
        const uchar* b = gpu.constScanLine(y);
        for (int x = 0; x < cpu.width(); ++x) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int difference = std::abs(int{a[x * 4 + channel]} - int{b[x * 4 + channel]});
                moved[channel] += difference != 0 ? 1 : 0;
                twoOrMore += difference > 1 ? 1 : 0;
            }
        }
    }
    const auto allowed = static_cast<std::uint64_t>(
        movedShare * static_cast<double>(static_cast<std::uint64_t>(cpu.width()) * cpu.height()));
    CAPTURE(moved, allowed);
    REQUIRE(twoOrMore == 0);
    for (const std::uint64_t channel : moved) {
        REQUIRE(channel <= allowed);
    }
}

/// @brief Builds the states of this test: the digest's, an exposure that clips, and the masks.
std::vector<DigestState> parityStates() {
    std::vector<DigestState> states = digestStates();
    DevelopSettings clipping;
    clipping.tone.exposure = 2.0F;
    clipping.tone.blacks = -60.0F;
    states.push_back({"clipping-exposure", DevelopState{clipping}});
    for (DigestState& state : digestMaskStates()) {
        // The brush states are the CPU's alone until the device draws strokes.
        if (state.name.find("brush") == std::string::npos) {
            states.push_back(std::move(state));
        }
    }
    return states;
}

} // namespace

TEST_CASE("GPU and CPU output histograms agree within one bin for 1 % of pixels", "[gpu]") {
    GpuContext& context = gpuContext();
    const auto sources = digestSources();
    const auto states = parityStates();
    const auto requests = digestRequests();
    for (const DigestSource& source : sources) {
        for (const DigestState& state : states) {
            for (const DigestRequest& request : requests) {
                const std::string name = source.name + "/" + state.name + "/" + request.name;
                DYNAMIC_SECTION(name) {
                    const ImageBuffer cpuRender =
                        develop(source.buffer, state.state, LensCorrection{}, request.request);
                    const ImageBuffer gpuRender =
                        developOnGpu(context, source.buffer, state.state, LensCorrection{},
                                     Stage::Effects, request.request)
                            .readBack();
                    requireAgreement(outputHistogram(cpuRender, NamedEncoding::Srgb),
                                     outputHistogram(gpuRender, NamedEncoding::Srgb));
                    requirePixelsWithinOneCode(cpuRender, gpuRender);
                }
            }
        }
    }
}

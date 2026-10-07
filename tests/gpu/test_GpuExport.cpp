#include "Cli.h"
#include "GpuContext.h"
#include "GpuTesting.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

namespace {

/// @brief One export's result: the exit code, what it said, and the image written.
struct Exported {
    int code = 0;
    std::string err;
    QImage image;
};

/// @brief Runs `arraw-cli export` in this process, on the GPU or the CPU, and reads the PNG back.
///
/// The suite already runs inside a QGuiApplication on the platform its
/// backend needs (see main.cpp), so the application factory has nothing to
/// start.
Exported exportWith(const std::string& device, const std::string& input, int bitDepth,
                    const std::vector<std::string>& develop) {
    const test::TempDir directory;
    std::vector<std::string> arguments{"export",
                                       input,
                                       "-o",
                                       directory.path().string(),
                                       "--format",
                                       "png",
                                       "--bit-depth",
                                       std::to_string(bitDepth),
                                       "--device",
                                       device,
                                       "--gpu-backend",
                                       std::string(gpuBackendName(test::gpuTestBackend())),
                                       "--allow-software"};
    arguments.insert(arguments.end(), develop.begin(), develop.end());
    std::ostringstream out;
    std::ostringstream err;
    const int code = cli::run(arguments, out, err, [](cli::ApplicationKind) {});
    Exported result{code, err.str(), {}};
    const auto file = directory.file(std::filesystem::path(input).stem().string() + ".png");
    if (code == cli::Success) {
        result.image.load(QString::fromStdString(file.string()));
    }
    return result;
}

/// @brief How far two exports of the same format are apart.
struct CodeDifference {
    /// @brief Largest difference in any sample, in code values.
    int worstCodes = 0;
    /// @brief Largest difference over what a sample is allowed; at most 1 passes.
    double worstExcess = 0.0;
};

/// @brief Compares every sample of two images of the same format.
///
/// A sample may differ by one code value, or by @p relative of its value when
/// that is more: at 16 bits the GPU's `pow` rounding shows up as tens of codes
/// (see illConditionedRelativeTolerance), which one code value in 65535 cannot
/// absorb.
CodeDifference compareCodes(const QImage& a, const QImage& b, double relative) {
    CodeDifference difference;
    const auto sample = [&](int left, int right) {
        const int codes = std::abs(left - right);
        const double allowed = std::max(1.0, relative * std::max(left, right));
        difference.worstCodes = std::max(difference.worstCodes, codes);
        difference.worstExcess = std::max(difference.worstExcess, codes / allowed);
    };
    for (int y = 0; y < a.height(); ++y) {
        const auto* left = a.constScanLine(y);
        const auto* right = b.constScanLine(y);
        if (a.format() == QImage::Format_RGBA64) {
            const auto* l = reinterpret_cast<const quint16*>(left);
            const auto* r = reinterpret_cast<const quint16*>(right);
            for (int i = 0; i < a.width() * 4; ++i) {
                sample(l[i], r[i]);
            }
        } else {
            for (int i = 0; i < a.width() * 4; ++i) {
                sample(left[i], right[i]);
            }
        }
    }
    return difference;
}

/// @brief Runs `arraw-cli export` of several inputs as JSON, in this process.
Exported exportBatch(const std::string& device, const std::vector<std::filesystem::path>& inputs,
                     const std::filesystem::path& directory,
                     const std::vector<std::string>& extra = {}) {
    std::vector<std::string> arguments{"export"};
    for (const auto& input : inputs) {
        arguments.push_back(input.string());
    }
    for (const std::string& argument : std::vector<std::string>{
             "-o", directory.string(), "--device", device, "--gpu-backend",
             std::string(gpuBackendName(test::gpuTestBackend())), "--allow-software",
             "--log-format", "json", "--format", "png", "--overwrite"}) {
        arguments.push_back(argument);
    }
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    std::ostringstream out;
    std::ostringstream err;
    const int code = cli::run(arguments, out, err, [](cli::ApplicationKind) {});
    return {code, err.str(), {}};
}

/// @brief Counts the non-overlapping occurrences of a text.
int occurrences(const std::string& text, const std::string& what) {
    int count = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("Export on the GPU stays within rounding of the CPU", "[gpu][cli][export][slow]") {
    (void)test::gpuContext(); // skips the case when there is no device at all

    struct Case {
        const char* input;
        std::vector<std::string> develop;
    };
    const std::vector<Case> cases{
        {"linear-32x24-rotated.dng",
         {"--exposure", "0.5", "--rotate", "9", "--flip-horizontal", "--temperature", "5200"}},
        {"linear-32x24-warmwb.dng", {"--rotate", "90", "--crop", "0.1,0.1,0.9,0.8"}},
        {"testcard-61x41-srgb8.png", {"--contrast", "20", "--rotate", "-7", "--flip-vertical"}},
        {"testcard-61x41-srgb8.png", {}},
        {"testcard-61x41-srgb8.png", {"--resize", "30"}},
        {"testcard-61x41-srgb8.png", {"--resize", "25%", "--resize-filter", "bilinear"}},
        {"testcard-61x41-srgb8.png", {"--resize", "100x100", "--allow-upscale"}},
        {"testcard-61x41-srgb8.png",
         {"--rotate", "90", "--crop", "0.1,0.2,0.8,0.9", "--resize", "20x40"}},
        {"linear-32x24-rotated.dng",
         {"--exposure", "0.5", "--rotate", "9", "--crop", "0.1,0.1,0.9,0.8", "--resize", "17"}},
        {"linear-32x24-warmwb.dng",
         {"--resize", "70%", "--resize-filter", "bilinear", "--flip-horizontal"}},
    };
    for (const auto& [name, develop] : cases) {
        const std::string input = test::fixture(name).string();
        for (const int bitDepth : {8, 16}) {
            DYNAMIC_SECTION(name << " at " << bitDepth << " bits, " << develop.size() / 2
                                 << " options, " << (develop.empty() ? "" : develop.back())) {
                const Exported gpu = exportWith("gpu", input, bitDepth, develop);
                const Exported cpu = exportWith("cpu", input, bitDepth, develop);
                INFO(gpu.err);
                REQUIRE(gpu.code == cli::Success);
                REQUIRE(cpu.code == cli::Success);
                REQUIRE_THAT(gpu.err, ContainsSubstring("exporting on the GPU"));
                REQUIRE_THAT(cpu.err, ContainsSubstring("exporting on the CPU"));
                REQUIRE(!gpu.image.isNull());
                REQUIRE(gpu.image.size() == cpu.image.size());
                const QImage::Format format =
                    bitDepth == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
                const CodeDifference difference = compareCodes(
                    gpu.image.convertToFormat(format), cpu.image.convertToFormat(format),
                    test::illConditionedRelativeTolerance);
                CAPTURE(difference.worstCodes, difference.worstExcess);
                REQUIRE(difference.worstExcess <= 1.0);
            }
        }
    }
}

TEST_CASE("A photograph too wide for the device falls back alone, in auto mode",
          "[gpu][cli][export]") {
    const int limit = test::gpuContext().info().maxTextureSize;
    if (limit <= 0 || limit > 32768) {
        SKIP("The device's largest texture, " << limit << " px, makes a wider image too big");
    }
    const test::TempDir directory;
    const auto good = test::fixture("testcard-61x41-srgb8.png");
    // A second photograph after the one that falls back: a copy under another
    // name, as the same file named twice is exported only once.
    const auto again = directory.file("again.png");
    std::filesystem::copy_file(good, again);
    const auto wide = directory.file("wide.png");
    QImage image(limit + 1, 2, QImage::Format_RGBA8888);
    image.fill(QColor(200, 100, 50));
    REQUIRE(image.save(QString::fromStdString(wide.string())));
    const auto out = directory.file("out");
    std::filesystem::create_directory(out);

    SECTION("auto blames the GPU for that input only") {
        if (test::gpuTestBackend() == GpuBackend::OpenGL) {
            SKIP("--gpu-backend opengl makes auto gpu (ADR 017); the gpu section covers it");
        }
        const Exported result = exportBatch("auto", {good, wide, again}, out);
        INFO(result.err);
        REQUIRE(result.code == cli::Success);
        REQUIRE(std::filesystem::exists(out / "wide.png"));
        REQUIRE(std::filesystem::exists(out / "testcard-61x41-srgb8.png"));
        REQUIRE_THAT(result.err, ContainsSubstring("exporting on the GPU"));
        REQUIRE(occurrences(result.err, "\"notice\":\"gpu_fallback\"") == 1);
        REQUIRE(occurrences(result.err, "\"notice\":\"exported\"") == 3);
        // JSON keys are sorted, so the subject precedes the notice on its line.
        const auto notice = result.err.find("\"notice\":\"gpu_fallback\"");
        const auto lineStart = result.err.rfind('\n', notice) + 1;
        REQUIRE_THAT(result.err.substr(lineStart, notice - lineStart),
                     ContainsSubstring("wide.png"));
    }
    SECTION("the input that falls back is resized on the CPU like the others") {
        if (test::gpuTestBackend() == GpuBackend::OpenGL) {
            SKIP("--gpu-backend opengl makes auto gpu (ADR 017); the gpu section covers it");
        }
        const Exported result = exportBatch("auto", {good, wide, again}, out, {"--resize", "30"});
        INFO(result.err);
        REQUIRE(result.code == cli::Success);
        REQUIRE(occurrences(result.err, "\"notice\":\"gpu_fallback\"") == 1);
        const QImage fallback(QString::fromStdString((out / "wide.png").string()));
        REQUIRE(fallback.width() == 30);
        REQUIRE(fallback.height() == 1);
        const QImage onDevice(QString::fromStdString((out / "testcard-61x41-srgb8.png").string()));
        REQUIRE(onDevice.size() == QSize(30, 20));
    }
    SECTION("gpu fails that input and exports the rest") {
        const Exported result = exportBatch("gpu", {good, wide, again}, out);
        INFO(result.err);
        REQUIRE(result.code == cli::Failed);
        REQUIRE(occurrences(result.err, "\"notice\":\"input_failed\"") == 1);
        REQUIRE(occurrences(result.err, "\"notice\":\"gpu_fallback\"") == 0);
        REQUIRE(occurrences(result.err, "\"notice\":\"exported\"") == 2);
        REQUIRE(!std::filesystem::exists(out / "wide.png"));
        REQUIRE(std::filesystem::exists(out / "testcard-61x41-srgb8.png"));
    }
}

TEST_CASE("A photograph that cannot be decoded fails, and is not blamed on the GPU",
          "[gpu][cli][export]") {
    (void)test::gpuContext();
    const test::TempDir directory;
    const auto source = test::fixture("testcard-61x41-srgb8.png");
    std::ifstream file(source, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    REQUIRE(bytes.size() > 200);
    const auto truncated = directory.file("truncated.png");
    std::ofstream(truncated, std::ios::binary) << bytes.substr(0, bytes.size() / 3);
    const auto out = directory.file("out");
    std::filesystem::create_directory(out);

    const Exported result = exportBatch("auto", {truncated, source}, out);
    INFO(result.err);
    REQUIRE(result.code == cli::Failed);
    REQUIRE(occurrences(result.err, "\"notice\":\"input_failed\"") == 1);
    REQUIRE(occurrences(result.err, "\"notice\":\"gpu_fallback\"") == 0);
    REQUIRE(!std::filesystem::exists(out / "truncated.png"));
    REQUIRE(std::filesystem::exists(out / "testcard-61x41-srgb8.png"));
}

// Untested on purpose: the branch that marks a device lost and finishes the batch on the CPU.
// `arraw-cli export` creates its own GpuContext inside cli::run, so a test could only reach
// that device through a process-wide hook in production code, and QRhi offers no portable way
// to lose a real one; the frame-failure paths that set the flag are one line each.

TEST_CASE("Export on a numbered adapter is export on the GPU, with that adapter's device",
          "[gpu][cli][export]") {
    const auto& context = test::gpuContext(); // skips the case when there is no device at all
    (void)context;
    const std::string input = test::fixture("testcard-61x41-srgb8.png").string();

    const Exported numbered = exportWith("gpu0", input, 8, {});
    INFO(numbered.err);
    REQUIRE(numbered.code == cli::Success);
    REQUIRE_THAT(numbered.err, ContainsSubstring("exporting on the GPU"));
    REQUIRE(!numbered.image.isNull());
}

TEST_CASE("Export on an adapter that does not exist fails, and does not fall back",
          "[gpu][cli][export]") {
    (void)test::gpuContext();
    const Exported missing =
        exportWith("gpu99", test::fixture("testcard-61x41-srgb8.png").string(), 8, {});

    REQUIRE(missing.code == cli::Failed);
    REQUIRE_THAT(missing.err, ContainsSubstring("no adapter 99"));
    REQUIRE_THAT(missing.err, !ContainsSubstring("exporting on the CPU"));
}

TEST_CASE("The GPU probe tests every adapter unless one is asked for", "[gpu][cli]") {
    (void)test::gpuContext();
    const auto probe = [](const std::vector<std::string>& arguments) {
        std::ostringstream out;
        std::ostringstream err;
        std::vector<std::string> command{"gpu-test", "--size", "1", "--gpu-backend",
                                         std::string(gpuBackendName(test::gpuTestBackend()))};
        command.insert(command.end(), arguments.begin(), arguments.end());
        const int code = cli::run(command, out, err, [](cli::ApplicationKind) {});
        return std::tuple{code, out.str(), err.str()};
    };

    const auto adapters = listGpuAdapters(test::gpuTestBackend());
    const auto software = static_cast<std::size_t>(std::ranges::count_if(
        adapters, [](const GpuAdapterInfo& info) { return info.kind == GpuDeviceKind::Software; }));
    // With no listed adapters the run probes the default device, which may be software too.
    const bool defaultIsSoftware =
        adapters.empty() && test::gpuContext().info().kind == GpuDeviceKind::Software;
    const std::string skipped = "Round trip:          skipped";
    const auto occurrences = [](const std::string& text, const std::string& needle) {
        std::size_t count = 0;
        for (auto at = text.find(needle); at != std::string::npos;
             at = text.find(needle, at + needle.size())) {
            ++count;
        }
        return count;
    };

    SECTION("every adapter, each labelled") {
        const auto [code, out, err] = probe({"--allow-software"});
        INFO(err);
        REQUIRE(code == cli::Success);
        REQUIRE_THAT(out, ContainsSubstring("exact"));
        for (std::size_t index = 0; index < std::max<std::size_t>(adapters.size(), 1); ++index) {
            REQUIRE_THAT(out,
                         ContainsSubstring("Adapter:             gpu" + std::to_string(index)));
        }
    }
    SECTION("a software adapter is skipped, and with nothing else the run fails") {
        const auto [code, out, err] = probe({});
        INFO(err);
        REQUIRE(occurrences(out, skipped) == software);
        if (defaultIsSoftware || (!adapters.empty() && software == adapters.size())) {
            REQUIRE(code == cli::Failed);
            REQUIRE_THAT(err, ContainsSubstring("nothing was tested"));
            REQUIRE_THAT(err, ContainsSubstring("--allow-software"));
        } else {
            REQUIRE(code == cli::Success);
        }
    }
    SECTION("a skipped adapter is logged under a name a script can match") {
        if (software == 0) {
            SKIP("no software adapter to skip");
        }
        const auto [code, out, err] = probe({"--log-format", "json"});
        REQUIRE_THAT(err, ContainsSubstring("\"notice\":\"gpu_adapter_skipped\""));
    }
    SECTION("one adapter, by number") {
        const auto [code, out, err] = probe({"--device", "gpu0", "--allow-software"});
        REQUIRE(code == cli::Success);
        REQUIRE_THAT(out, ContainsSubstring("gpu0"));
    }
    SECTION("the default device carries no label") {
        const auto [code, out, err] = probe({"--device", "gpu", "--allow-software"});
        REQUIRE(code == cli::Success);
        REQUIRE_THAT(out, !ContainsSubstring("Adapter:"));
    }
    SECTION("an adapter past the end is a failure that says how many there are") {
        const auto [code, out, err] = probe({"--device", "gpu99", "--allow-software"});
        REQUIRE(code == cli::Failed);
        REQUIRE_THAT(err, ContainsSubstring("no adapter 99"));
    }
}

#include "Cli.h"
#include "GpuTesting.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <QColor>
#include <QImage>
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
/// The suite already runs inside a QGuiApplication on arraw's headless
/// platform, which is what the binary makes for a GPU export, so the
/// application factory has nothing to start.
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

/// @brief Finds the largest difference in any sample of two images of the same format.
int worstCodeDifference(const QImage& a, const QImage& b) {
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* left = a.constScanLine(y);
        const auto* right = b.constScanLine(y);
        if (a.format() == QImage::Format_RGBA64) {
            const auto* l = reinterpret_cast<const quint16*>(left);
            const auto* r = reinterpret_cast<const quint16*>(right);
            for (int i = 0; i < a.width() * 4; ++i) {
                worst = std::max(worst, std::abs(int(l[i]) - int(r[i])));
            }
        } else {
            for (int i = 0; i < a.width() * 4; ++i) {
                worst = std::max(worst, std::abs(int(left[i]) - int(right[i])));
            }
        }
    }
    return worst;
}

/// @brief Runs `arraw-cli export` of several inputs as JSON, in this process.
Exported exportBatch(const std::string& device, const std::vector<std::filesystem::path>& inputs,
                     const std::filesystem::path& directory) {
    std::vector<std::string> arguments{"export"};
    for (const auto& input : inputs) {
        arguments.push_back(input.string());
    }
    for (const std::string& argument :
         std::vector<std::string>{"-o", directory.string(), "--device", device, "--allow-software",
                                  "--log-format", "json", "--format", "png", "--overwrite"}) {
        arguments.push_back(argument);
    }
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

TEST_CASE("Export on the GPU stays within one code value of the CPU", "[gpu][cli][export]") {
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
    };
    for (const auto& [name, develop] : cases) {
        const std::string input = test::fixture(name).string();
        for (const int bitDepth : {8, 16}) {
            DYNAMIC_SECTION(name << " at " << bitDepth << " bits, " << develop.size() / 2
                                 << " options") {
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
                const int worst = worstCodeDifference(gpu.image.convertToFormat(format),
                                                      cpu.image.convertToFormat(format));
                CAPTURE(worst);
                REQUIRE(worst <= 1);
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
    const auto wide = directory.file("wide.png");
    QImage image(limit + 1, 2, QImage::Format_RGBA8888);
    image.fill(QColor(200, 100, 50));
    REQUIRE(image.save(QString::fromStdString(wide.string())));
    const auto out = directory.file("out");
    std::filesystem::create_directory(out);

    SECTION("auto blames the GPU for that input only") {
        const Exported result = exportBatch("auto", {good, wide, good}, out);
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
    SECTION("gpu fails that input and exports the rest") {
        const Exported result = exportBatch("gpu", {good, wide, good}, out);
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

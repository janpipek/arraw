#include "Cli.h"
#include "GpuTesting.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <QImage>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
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

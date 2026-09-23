#include "GpuTestCommand.h"

#include "Cli.h"
#include "Command.h"

#include <GpuContext.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>
#include <QStringList>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

using namespace arraw;

namespace {

/// @brief Smallest test image edge, in pixels.
constexpr int smallestEdge = 1;

/// @brief Largest test image edge, in pixels: 1 GiB of RGBA32F, which every
/// backend's transfer size limit still admits.
constexpr int largestEdge = 8192;

/// @brief Test image edge when none is asked for: 16 MiB, enough to time.
constexpr int defaultEdge = 1024;

/// @brief Column the report's values line up in.
constexpr std::size_t valueColumn = 21;

/// @brief Everything the command needs, once its arguments are understood.
struct GpuTestRequest {
    GpuBackend backend = defaultGpuBackend();
    std::uint32_t edge = defaultEdge;
    bool allowSoftware = false;
};

/// @brief Reports a usage problem and the exit code that goes with it.
int usageError(std::ostream& err, const std::string& message) {
    return cli::commandUsageError(err, "gpu-test", message);
}

/// @brief Lists the backend names, for help and for errors.
std::string backendChoices() {
    return "vulkan, opengl, d3d11, d3d12, or metal";
}

/// @brief Names a device kind, as the report writes it.
std::string_view nameOf(GpuDeviceKind kind) {
    switch (kind) {
    case GpuDeviceKind::Unknown:
        return "unknown";
    case GpuDeviceKind::Integrated:
        return "integrated";
    case GpuDeviceKind::Discrete:
        return "discrete";
    case GpuDeviceKind::External:
        return "external";
    case GpuDeviceKind::Virtual:
        return "virtual";
    case GpuDeviceKind::Software:
        return "software";
    }
    return "unknown";
}

/// @brief Writes one line of the report, its value aligned with the others.
void field(std::ostream& out, std::string_view label, std::string_view value) {
    out << label << ':';
    if (label.size() + 1 < valueColumn) {
        out << std::string(valueColumn - label.size() - 1, ' ');
    } else {
        out << ' ';
    }
    out << value << '\n';
}

/// @brief Says yes or no.
std::string_view yesNo(bool value) {
    return value ? "yes" : "no";
}

/// @brief Formats a number with a fixed count of decimals.
std::string fixed(double value, int decimals) {
    return QString::number(value, 'f', decimals).toStdString();
}

/// @brief Formats an id the way driver tools print them.
std::string hex(std::uint64_t value) {
    return "0x" + QString::number(static_cast<qulonglong>(value), 16).toStdString();
}

/// @brief Scrambles an index into well-spread bits, deterministically.
constexpr std::uint32_t scramble(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

/// @brief Colour samples a lesser storage format or a converting copy would change.
///
/// What the CPU chain deliberately preserves: values above white, negatives
/// from the camera matrix, and the very dark. The half-float boundaries are
/// here because RGBA16F is the trade-off ADR 015 leaves open, and a backend
/// that quietly stored half would fail on exactly these.
constexpr std::array colourLandmarks{
    0.0F,
    -0.0F,
    1.0F,
    -1.0F,
    0.5F,
    1.0F / 3.0F,
    -0.25F,
    2.0F,
    16.0F,
    65504.0F, // largest finite half
    65520.0F, // rounds to infinity as a half
    1.0e6F,
    -1.0e6F,
    std::numeric_limits<float>::max(),
    std::numeric_limits<float>::lowest(),
    std::numeric_limits<float>::min(), // smallest normal
    1.0e-40F,                          // subnormal
    std::numeric_limits<float>::denorm_min(),
    -std::numeric_limits<float>::denorm_min(),
    6.0e-8F, // smallest half subnormal, near enough
    1.0e-8F, // below every half
};

/// @brief Alpha samples, straight and fractional, as development leaves them.
constexpr std::array alphaLandmarks{
    0.0F, 1.0F, 0.5F, 0.25F, 1.0F / 3.0F, 1.0F / 255.0F, 254.0F / 255.0F, 0.999999F,
};

/// @brief Builds the square image the round trip is judged on.
///
/// The first pixels carry every landmark in every channel; the rest are
/// scrambled bit patterns, so that every finite float class — normal,
/// subnormal, signed zero, both signs, every exponent — turns up somewhere.
/// Alpha stays within 0 to 1, as it does after development.
ImageBuffer testImage(std::uint32_t edge) {
    // Not upright, so that the description surviving the trip is checked too.
    ImageBuffer image({edge, edge}, PixelFormat::RgbaF32, workingEncoding,
                      ImageOrientation::Rotate90);
    const std::span<float> samples = image.samples<float>();
    constexpr std::size_t channels = 4;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const std::size_t pixel = index / channels;
        const std::size_t channel = index % channels;
        const std::uint32_t bits = scramble(static_cast<std::uint32_t>(index));
        if (channel == channels - 1) {
            samples[index] = pixel < alphaLandmarks.size()
                                 ? alphaLandmarks[pixel]
                                 : static_cast<float>(bits >> 8) / 16777216.0F;
        } else if (pixel < colourLandmarks.size()) {
            samples[index] = colourLandmarks[(pixel + channel) % colourLandmarks.size()];
        } else {
            // An all-ones exponent is infinity or NaN; clearing its top bit
            // keeps the pattern finite without losing any other class.
            constexpr std::uint32_t exponent = 0x7f800000U;
            const std::uint32_t finite = (bits & exponent) == exponent ? bits & ~0x40000000U : bits;
            samples[index] = std::bit_cast<float>(finite);
        }
    }
    return image;
}

/// @brief Where two buffers' samples disagree, bit for bit.
struct Comparison {
    /// @brief Number of samples whose bits differ.
    std::size_t mismatches = 0;

    /// @brief Index of the first differing sample, if any differ.
    std::size_t first = 0;
};

/// @brief Compares every sample's bits; `==` would call -0 and 0 equal.
Comparison compare(std::span<const float> sent, std::span<const float> received) {
    Comparison result;
    for (std::size_t index = 0; index < sent.size(); ++index) {
        if (std::bit_cast<std::uint32_t>(sent[index]) !=
            std::bit_cast<std::uint32_t>(received[index])) {
            if (result.mismatches == 0) {
                result.first = index;
            }
            ++result.mismatches;
        }
    }
    return result;
}

/// @brief Describes one sample, by value and by bits.
std::string describeSample(float value) {
    return QString::number(value, 'g', 9).toStdString() + " (" +
           hex(std::bit_cast<std::uint32_t>(value)) + ")";
}

/// @brief Configures the command's own parser.
void configure(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        "Check that the GPU backend works on this machine.\n"
        "\n"
        "Creates an offscreen device through one backend, never falling back to\n"
        "another, and reports what it is and what it supports. Then uploads an RGBA\n"
        "float test image and reads it back; the round trip must be exact bit for bit,\n"
        "values above white, negatives, subnormals and fractional alpha included. A\n"
        "software rasteriser is not a GPU and fails unless --allow-software is given.\n"
        "With ARRAW_DISABLE_GPU set to a value other than 0, no device is attempted\n"
        "and the probe fails. On Linux, unless QT_QPA_PLATFORM names another, the\n"
        "probe runs on arraw's headless platform, which reaches Vulkan without a\n"
        "display but has no OpenGL; set QT_QPA_PLATFORM=xcb or wayland for that.\n"
        "The exit status is 0 when all is well, 1 when it is not, 2 for a usage error.");
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(
        "backend",
        QString::fromStdString(backendChoices() + ". Default: " +
                               std::string(gpuBackendName(defaultGpuBackend())) + "."),
        "name"));
    parser.addOption({"size",
                      QString("Edge of the square test image, %1-%2. Default: %3.")
                          .arg(smallestEdge)
                          .arg(largestEdge)
                          .arg(defaultEdge),
                      "pixels"});
    parser.addOption({"allow-software", "Accept a software rasteriser, such as llvmpipe or WARP."});
    // The syntax carries the command word, which Qt's usage line otherwise
    // omits: it knows only argv[0], and the command is a positional we consumed.
    parser.addPositionalArgument("gpu-test", "The command word; gpu-test takes no inputs.",
                                 "gpu-test");
}

/// @brief Turns the parsed arguments into a ::GpuTestRequest.
/// @return The request, or `std::nullopt` after reporting the problem.
std::optional<GpuTestRequest> buildRequest(const QCommandLineParser& parser, std::ostream& err,
                                           int& code) {
    GpuTestRequest request;

    if (!parser.positionalArguments().isEmpty()) {
        code = usageError(err, "gpu-test takes no inputs, but was given '" +
                                   parser.positionalArguments().front().toStdString() + "'");
        return std::nullopt;
    }
    if (parser.isSet("backend")) {
        const auto name = parser.value("backend").toLower().toStdString();
        const auto backend = parseGpuBackend(name);
        if (!backend) {
            code = usageError(err, "unknown backend '" + name + "'; expected " + backendChoices());
            return std::nullopt;
        }
        request.backend = *backend;
    }
    if (parser.isSet("size")) {
        bool valid = false;
        const int edge = parser.value("size").toInt(&valid);
        if (!valid || edge < smallestEdge || edge > largestEdge) {
            code = usageError(err, "--size takes a whole number of pixels from " +
                                       std::to_string(smallestEdge) + " to " +
                                       std::to_string(largestEdge));
            return std::nullopt;
        }
        request.edge = static_cast<std::uint32_t>(edge);
    }
    request.allowSoftware = parser.isSet("allow-software");
    return request;
}

/// @brief Reports the device, refusing one that cannot stand in for a GPU.
/// @return `true` if the round trip is worth attempting.
bool reportDevice(const GpuDeviceInfo& info, bool allowSoftware, std::ostream& out,
                  std::ostream& err) {
    field(out, "Backend", gpuBackendName(info.backend));
    field(out, "Device", info.deviceName.empty() ? "(unnamed)" : info.deviceName);
    field(out, "Kind", nameOf(info.kind));
    field(out, "Vendor id", hex(info.vendorId));
    field(out, "Device id", hex(info.deviceId));
    field(out, "RGBA32F textures", yesNo(info.floatTextures));
    field(out, "RGBA16F textures", yesNo(info.halfFloatTextures));
    field(out, "Compute", yesNo(info.compute));
    field(out, "Any-format readback", info.anyFormatReadBack ? "promised" : "not promised");
    field(out, "Largest texture", std::to_string(info.maxTextureSize) + " px");

    if (info.kind == GpuDeviceKind::Software) {
        if (!allowSoftware) {
            err << "error: '" << info.deviceName
                << "' is a software rasteriser, not a GPU; pass --allow-software to accept it\n";
            return false;
        }
        err << "warning: '" << info.deviceName
            << "' is a software rasteriser, accepted because --allow-software was given\n";
    }
    if (!info.floatTextures) {
        err << "error: the device does not support RGBA32F textures, which development needs\n";
        return false;
    }
    if (!info.anyFormatReadBack) {
        err << "warning: the " << gpuBackendName(info.backend)
            << " backend does not promise float readback; the round trip decides\n";
    }
    return true;
}

/// @brief Uploads the test image, reads it back and judges the result.
int roundTrip(GpuContext& context, std::uint32_t edge, std::ostream& out, std::ostream& err) {
    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::duration<double, std::milli>;

    const ImageBuffer sent = testImage(edge);
    const double mebibytes = static_cast<double>(sent.byteSize()) / (1024.0 * 1024.0);
    const auto rate = [mebibytes](double milliseconds) {
        return milliseconds > 0.0 ? ", " + fixed(mebibytes / (milliseconds / 1000.0), 0) + " MiB/s"
                                  : std::string{};
    };

    const auto started = Clock::now();
    const DeviceImage uploaded = context.upload(sent);
    const auto uploadedAt = Clock::now();
    const ImageBuffer received = uploaded.readBack();
    const auto receivedAt = Clock::now();

    const double uploadTime = Milliseconds(uploadedAt - started).count();
    const double readBackTime = Milliseconds(receivedAt - uploadedAt).count();
    field(out, "Test image",
          std::to_string(edge) + "x" + std::to_string(edge) + " RGBA32F, " + fixed(mebibytes, 1) +
              " MiB");
    field(out, "Upload", fixed(uploadTime, 1) + " ms" + rate(uploadTime));
    field(out, "Readback", fixed(readBackTime, 1) + " ms" + rate(readBackTime));

    if (received.size() != sent.size() || received.format() != sent.format() ||
        !(received.encoding() == sent.encoding()) || received.orientation() != sent.orientation()) {
        field(out, "Round trip", "changed the image's description");
        err << "error: the image read back is not described as the one uploaded\n";
        return cli::Failed;
    }
    const auto samples = sent.samples<float>();
    const Comparison comparison = compare(samples, received.samples<float>());
    if (comparison.mismatches == 0) {
        field(out, "Round trip", "exact, " + std::to_string(samples.size()) + " samples");
        return cli::Success;
    }

    constexpr std::size_t channels = 4;
    const std::size_t pixel = comparison.first / channels;
    field(out, "Round trip",
          std::to_string(comparison.mismatches) + " of " + std::to_string(samples.size()) +
              " samples changed");
    err << "error: the round trip changed " << comparison.mismatches << " samples; the first, "
        << "channel " << "RGBA"[comparison.first % channels] << " of pixel (" << pixel % edge
        << ", " << pixel / edge << "), went in as " << describeSample(samples[comparison.first])
        << " and came back as " << describeSample(received.samples<float>()[comparison.first])
        << '\n';
    return cli::Failed;
}

/// @brief Creates the device, reports it and runs the round trip.
int probe(const GpuTestRequest& request, std::ostream& out, std::ostream& err) {
    std::unique_ptr<GpuContext> context;
    try {
        context = std::make_unique<GpuContext>(request.backend);
    } catch (const std::exception& problem) {
        err << "error: " << problem.what() << '\n';
        return cli::Failed;
    }
    if (!reportDevice(context->info(), request.allowSoftware, out, err)) {
        return cli::Failed;
    }
    try {
        return roundTrip(*context, request.edge, out, err);
    } catch (const std::exception& problem) {
        err << "error: " << problem.what() << '\n';
        return cli::Failed;
    }
}

} // namespace

int cli::runGpuTestCommand(const QStringList& arguments, std::ostream& out, std::ostream& err) {
    QCommandLineParser parser;
    configure(parser);

    // parse() rather than process(), as for export: process() writes to the
    // real stderr and calls exit(), neither of which a tested function may do.
    if (!parser.parse(arguments)) {
        return usageError(err, parser.errorText().toStdString());
    }
    // Qt registers --help-all as an option of its own, so it must be answered
    // here too rather than run the probe. Qt's generic options are not this
    // command's, so the text is one.
    if (parser.isSet("help") || parser.isSet("help-all")) {
        out << parser.helpText().toStdString();
        return Success;
    }

    int code = Success;
    const auto request = buildRequest(parser, err, code);
    if (!request) {
        return code;
    }
    // After the arguments, so a mistyped flag is still a usage error, and before
    // any device: main() made no QGuiApplication, and the honest report is that
    // the GPU was turned off rather than that it is missing.
    if (gpuDisabled()) {
        err << "error: the GPU is disabled by " << disableGpuVariable
            << "; unset it, or set it to 0, to probe the device\n";
        return Failed;
    }
    return probe(*request, out, err);
}

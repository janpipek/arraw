#include "GpuTestCommand.h"

#include "Cli.h"
#include "Command.h"
#include "DeviceChoice.h"
#include "GpuContext.h"
#include "StreamDiagnostics.h"
#include "TerminalStyle.h"

#include <Diagnostics.h>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
    cli::DeviceChoice device;
    std::uint32_t edge = defaultEdge;
    bool allowSoftware = false;
    cli::LogFormat logFormat = cli::LogFormat::Text;
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
    out << cli::accented(out, label, cli::Accent::Heading) << ':';
    if (label.size() + 1 < valueColumn) {
        out << std::string(valueColumn - label.size() - 1, ' ');
    } else {
        out << ' ';
    }
    out << cli::terminalText(value) << '\n';
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
        "another, and reports what it is and what it supports. By default it does so for\n"
        "every adapter the backend lists, each labelled gpu0, gpu1, and so on, and goes\n"
        "on past one that fails; --device gpu tests the default device only, and\n"
        "--device gpuN adapter N only, counting from 0 per backend. In the default\n"
        "mode a software adapter is listed and skipped unless --allow-software is\n"
        "given; the run fails if any tested adapter fails, or if none was tested.\n"
        "Each test uploads an RGBA float test image and reads it back; the round trip\n"
        "must be exact bit for bit, values above white, negatives, subnormals and\n"
        "fractional alpha included.\n"
        "With --device gpu or gpuN a software rasteriser is not a GPU and fails\n"
        "unless --allow-software is given.\n"
        "With ARRAW_DISABLE_GPU set to a value other than 0, no device is attempted\n"
        "and the probe fails. On Linux, unless QT_QPA_PLATFORM names another, the\n"
        "probe runs on arraw's headless platform, which reaches Vulkan without a\n"
        "display but has no OpenGL; set QT_QPA_PLATFORM=xcb or wayland for that.\n"
        "The exit status is 0 when all is well, 1 when it is not, 2 for a usage error.");
    parser.addHelpOption();
    parser.addOption(QCommandLineOption(
        "gpu-backend",
        QString::fromStdString(backendChoices() + ". Default: " +
                               std::string(gpuBackendName(defaultGpuBackend())) + "."),
        "name"));
    parser.addOption(QCommandLineOption("backend", "Alias of --gpu-backend.", "name"));
    parser.addOption({"device",
                      "auto, gpu, or gpuN; cpu is not a device to probe. Auto tests every "
                      "adapter the backend lists; gpu the default device; gpuN the N-th "
                      "adapter, counting from 0. Default: auto.",
                      "name"});
    parser.addOption({"size",
                      QString("Edge of the square test image, %1-%2. Default: %3.")
                          .arg(smallestEdge)
                          .arg(largestEdge)
                          .arg(defaultEdge),
                      "pixels"});
    parser.addOption({"allow-software", "Accept a software rasteriser, such as llvmpipe or WARP."});
    cli::addLogFormatOption(parser);
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
    if (parser.isSet("gpu-backend") || parser.isSet("backend")) {
        // --gpu-backend wins if both are given: it is the name that stays.
        const auto name = parser.value(parser.isSet("gpu-backend") ? "gpu-backend" : "backend")
                              .toLower()
                              .toStdString();
        const auto backend = parseGpuBackend(name);
        if (!backend) {
            code = usageError(err, "unknown backend '" + name + "'; expected " + backendChoices());
            return std::nullopt;
        }
        request.backend = *backend;
    }
    if (parser.isSet("device")) {
        const auto text = parser.value("device").toStdString();
        const auto device = cli::parseDeviceChoice(text);
        if (!device) {
            code = usageError(err, "unknown device '" + text + "'; expected auto, gpu, or gpuN");
            return std::nullopt;
        }
        if (device->kind == cli::DeviceKind::Cpu) {
            code = usageError(err, "--device cpu has no GPU to probe; expected auto, gpu, or gpuN");
            return std::nullopt;
        }
        request.device = *device;
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
    const auto logFormat = cli::readLogFormat(parser, "gpu-test", err, code);
    if (!logFormat) {
        return std::nullopt;
    }
    request.logFormat = *logFormat;
    return request;
}

/// @brief Names an adapter by the number `--device` takes.
std::string adapterLabel(std::size_t adapter) {
    return "gpu" + std::to_string(adapter);
}

/// @brief Reports the device, refusing one that cannot stand in for a GPU.
/// @return `true` if the round trip is worth attempting.
bool reportDevice(const GpuDeviceInfo& info, bool allowSoftware, std::ostream& out,
                  DiagnosticLog& log) {
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
            log.record({.notice = Notice::GpuSoftwareRefused,
                        .severity = Severity::Error,
                        .values = {info.deviceName}});
            return false;
        }
        log.record({.notice = Notice::GpuSoftwareAccepted,
                    .severity = Severity::Warning,
                    .values = {info.deviceName}});
    }
    if (!info.floatTextures) {
        log.record({.notice = Notice::GpuNoFloatTextures, .severity = Severity::Error});
        return false;
    }
    if (!info.anyFormatReadBack) {
        log.record({.notice = Notice::GpuReadBackNotPromised,
                    .severity = Severity::Warning,
                    .values = {std::string(gpuBackendName(info.backend))}});
    }
    return true;
}

/// @brief Uploads the test image, reads it back and judges the result.
int roundTrip(GpuContext& context, std::uint32_t edge, std::ostream& out, DiagnosticLog& log) {
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
        log.record({.notice = Notice::GpuRoundTripRedescribed, .severity = Severity::Error});
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
    log.record({.notice = Notice::GpuRoundTripChanged,
                .severity = Severity::Error,
                .values = {std::to_string(comparison.mismatches),
                           std::string(1, "RGBA"[comparison.first % channels]),
                           std::to_string(pixel % edge), std::to_string(pixel / edge),
                           describeSample(samples[comparison.first]),
                           describeSample(received.samples<float>()[comparison.first])}});
    return cli::Failed;
}

/// @brief Reports a failure to create or use a device.
/// @return ::arraw::cli::Failed.
int failed(const std::exception& problem, DiagnosticLog& log) {
    log.record({.notice = Notice::GpuFailed,
                .severity = Severity::Error,
                .values = {std::string(problem.what())}});
    return cli::Failed;
}

/// @brief Creates one device, reports it and runs the round trip.
/// @param adapter The adapter to test, or empty for the backend's default device.
int probe(const GpuTestRequest& request, std::optional<std::size_t> adapter, std::ostream& out,
          DiagnosticLog& log) {
    // Before the device exists, so that an adapter that cannot be created is
    // still named on stdout beside the error on stderr.
    if (adapter) {
        field(out, "Adapter", adapterLabel(*adapter));
    }
    std::unique_ptr<GpuContext> context;
    try {
        context = std::make_unique<GpuContext>(request.backend, adapter);
    } catch (const std::exception& problem) {
        return failed(problem, log);
    }
    if (!reportDevice(context->info(), request.allowSoftware, out, log)) {
        return cli::Failed;
    }
    try {
        return roundTrip(*context, request.edge, out, log);
    } catch (const std::exception& problem) {
        return failed(problem, log);
    }
}

/// @brief Reports an adapter that is not tested, and why.
void reportSkipped(std::size_t adapter, const GpuAdapterInfo& info, const GpuTestRequest& request,
                   std::ostream& out, DiagnosticLog& log) {
    field(out, "Adapter", adapterLabel(adapter));
    field(out, "Backend", gpuBackendName(request.backend));
    field(out, "Device", info.name.empty() ? "(unnamed)" : info.name);
    field(out, "Kind", nameOf(info.kind));
    field(out, "Vendor id", hex(info.vendorId));
    field(out, "Device id", hex(info.deviceId));
    field(out, "Round trip", "skipped");
    log.record({.notice = Notice::GpuAdapterSkipped,
                .severity = Severity::Warning,
                .values = {adapterLabel(adapter), info.name}});
}

/// @brief Tests every adapter the backend lists, one block each.
///
/// A software adapter is skipped unless accepting one was asked for, and a
/// failure does not stop the ones after it: the point of testing them all is
/// to learn which work.
int probeAll(const GpuTestRequest& request, std::ostream& out, DiagnosticLog& log) {
    std::vector<GpuAdapterInfo> adapters;
    try {
        adapters = listGpuAdapters(request.backend);
    } catch (const std::exception& problem) {
        return failed(problem, log);
    }
    if (adapters.empty()) {
        // A backend that does not enumerate still has its one default device.
        return probe(request, 0, out, log);
    }

    std::size_t tested = 0;
    int result = cli::Success;
    for (std::size_t adapter = 0; adapter < adapters.size(); ++adapter) {
        if (adapter > 0) {
            out << '\n';
        }
        if (adapters[adapter].kind == GpuDeviceKind::Software && !request.allowSoftware) {
            reportSkipped(adapter, adapters[adapter], request, out, log);
            continue;
        }
        ++tested;
        if (probe(request, adapter, out, log) != cli::Success) {
            result = cli::Failed;
        }
    }
    if (tested == 0) {
        log.record({.notice = Notice::GpuFailed,
                    .severity = Severity::Error,
                    .values = {"every adapter is a software rasteriser, so nothing was tested; "
                               "pass --allow-software to test one"}});
        return cli::Failed;
    }
    return result;
}

/// @brief Runs the probe the request asks for.
int probeRequested(const GpuTestRequest& request, std::ostream& out, DiagnosticLog& log) {
    if (request.device.kind == cli::DeviceKind::Auto) {
        return probeAll(request, out, log);
    }
    return probe(request, request.device.adapter, out, log);
}

} // namespace

int cli::runGpuTestCommand(const QStringList& arguments, std::ostream& out, std::ostream& err,
                           const StartApplication& start) {
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
        writeStyledHelp(out, commandHelp(parser));
        return Success;
    }

    int code = Success;
    const auto request = buildRequest(parser, err, code);
    if (!request) {
        return code;
    }
    // After the arguments, so a mistyped flag is still a usage error, and before
    // any application: the honest report is that the GPU was turned off, and
    // turning it off means no platform plugin is loaded.
    StreamDiagnostics log(err, request->logFormat);
    if (gpuDisabled()) {
        log.record({.notice = Notice::GpuDisabled,
                    .severity = Severity::Error,
                    .values = {std::string(disableGpuVariable)}});
        return Failed;
    }
    start(ApplicationKind::Gui);
    return probeRequested(*request, out, log);
}

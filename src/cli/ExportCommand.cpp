#include "ExportCommand.h"

#include "Cli.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <ImageExport.h>
#include <ImageImport.h>
#include <WhiteBalance.h>

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

#include <exception>
#include <filesystem>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace arraw;

namespace {

/// @brief File extension a format is written with.
std::string_view extensionFor(ImageFileFormat format) {
    switch (format) {
    case ImageFileFormat::Png:
        return ".png";
    case ImageFileFormat::Jpeg:
        return ".jpg";
    case ImageFileFormat::Tiff:
        return ".tif";
    }
    return ".bin";
}

/// @brief Everything the command needs, once its arguments are understood.
struct ExportRequest {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path outputDirectory;
    ImageFileFormat format = ImageFileFormat::Jpeg;
    DevelopSettings settings;
    ExportOptions options;
    bool overwrite = false;
    bool quiet = false;
};

/// @brief Reports a usage problem and the exit code that goes with it.
int usageError(std::ostream& err, const std::string& message) {
    err << "error: " << message << "\n\nTry 'arraw-cli export --help'.\n";
    return cli::UsageError;
}

/// @brief Reads a named option as an integer.
/// @return `true` if the option was absent or parsed; `false` if it was malformed.
bool readInteger(const QCommandLineParser& parser, const char* name, int& value) {
    if (!parser.isSet(name)) {
        return true;
    }
    bool valid = false;
    const int parsed = parser.value(name).toInt(&valid);
    if (valid) {
        value = parsed;
    }
    return valid;
}

/// @brief Reads a named option as a number within its modelled range.
///
/// Out of range is refused rather than clamped: a photographer is present to
/// be told, and nothing invalid should enter a session (ADR 008). The renderer
/// clamps as well, for values that arrive from a file instead.
/// @return `true` if the option was absent or acceptable; `false` otherwise.
bool readSetting(const QCommandLineParser& parser, const char* name, float lowest, float highest,
                 std::optional<float>& value, std::ostream& err, int& code) {
    if (!parser.isSet(name)) {
        return true;
    }
    bool valid = false;
    const float parsed = parser.value(name).toFloat(&valid);
    if (!valid) {
        code = usageError(err, std::string("--") + name + " takes a number");
        return false;
    }
    if (parsed < lowest || parsed > highest) {
        code = usageError(err, std::string("--") + name + " accepts " +
                                   QString::number(lowest).toStdString() + " to " +
                                   QString::number(highest).toStdString());
        return false;
    }
    value = parsed;
    return true;
}

/// @brief Configures the command's own parser.
///
/// In place rather than returned: QCommandLineParser is neither copyable nor
/// movable. Each command has its own, so this help lists export's options and
/// no other command's.
void configure(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        "Render images and write them out.\n"
        "\n"
        "Inputs are files, not directories; your shell expands the wildcards. Every\n"
        "input is attempted, so one bad frame does not abandon an overnight batch. The\n"
        "exit status is 0 when all succeeded, 1 when any failed, 2 for a usage error.\n"
        "\n"
        "With no develop settings an export is a faithful conversion of the image as\n"
        "captured rather than a rendered photograph. Exposure and white balance are\n"
        "implemented; the rest of the develop controls are not yet.");
    parser.addHelpOption();
    parser.addOption({{"o", "output"}, "Existing directory to write into.", "dir"});
    parser.addOption({"format", "png, jpeg, or tiff. Default: jpeg.", "name"});
    parser.addOption({"quality", "JPEG quality, 0-100. Default: 90.", "value"});
    parser.addOption({"bit-depth", "8 or 16. Default: 8.", "value"});
    parser.addOption({"encoding", "srgb, display-p3, or adobe-rgb. Default: srgb.", "name"});
    parser.addOption({"exposure", "Exposure adjustment in EV, -5 to 5.", "stops"});
    parser.addOption({"temperature", "White balance in kelvin, 2000 to 12000. RAW only.", "k"});
    parser.addOption({"tint", "Green to magenta, -150 to 150. RAW only.", "amount"});
    parser.addOption({"no-profile", "Convert colour but do not embed the output profile."});
    parser.addOption({"overwrite", "Replace outputs that already exist."});
    parser.addOption({{"q", "quiet"}, "Do not report each file as it is written."});
    // The syntax carries the command word, which Qt's usage line otherwise
    // omits: it knows only argv[0], and the command is a positional we consumed.
    parser.addPositionalArgument("input", "Files to export.", "export <input>...");
}

/// @brief Turns the parsed arguments into an ::ExportRequest.
///
/// Only the values arraw itself must interpret are checked here -- a format
/// name has to become an enumerator, so an unknown one cannot be passed on.
/// Ranges are left to ::arraw::exportImage, which already rejects them and is
/// the single place that knows what it accepts.
/// @return The request, or `std::nullopt` after reporting the problem.
std::optional<ExportRequest> buildRequest(const QCommandLineParser& parser, std::ostream& err,
                                          int& code) {
    ExportRequest request;

    for (const QString& input : parser.positionalArguments()) {
        request.inputs.emplace_back(input.toStdU16String());
    }
    if (request.inputs.empty()) {
        code = usageError(err, "no input files given");
        return std::nullopt;
    }
    if (!parser.isSet("output")) {
        code = usageError(err, "no output directory given; pass -o <dir>");
        return std::nullopt;
    }

    request.outputDirectory = parser.value("output").toStdU16String();
    if (!std::filesystem::is_directory(request.outputDirectory)) {
        code = usageError(err, "not a directory: " + request.outputDirectory.string() +
                                   "\n       arraw-cli writes into an existing directory and "
                                   "does not create one");
        return std::nullopt;
    }

    if (parser.isSet("format")) {
        const auto name = parser.value("format").toLower();
        if (name == "png") {
            request.format = ImageFileFormat::Png;
        } else if (name == "jpeg" || name == "jpg") {
            request.format = ImageFileFormat::Jpeg;
        } else if (name == "tiff" || name == "tif") {
            request.format = ImageFileFormat::Tiff;
        } else {
            code = usageError(err, "unknown format '" + name.toStdString() +
                                       "'; expected png, jpeg, or tiff");
            return std::nullopt;
        }
    }
    request.options.format = request.format;

    if (parser.isSet("encoding")) {
        const auto name = parser.value("encoding").toLower();
        if (name == "srgb") {
            request.options.encoding = NamedEncoding::Srgb;
        } else if (name == "display-p3") {
            request.options.encoding = NamedEncoding::DisplayP3;
        } else if (name == "adobe-rgb") {
            request.options.encoding = NamedEncoding::AdobeRgb;
        } else {
            code = usageError(err, "unknown encoding '" + name.toStdString() +
                                       "'; expected srgb, display-p3, or adobe-rgb");
            return std::nullopt;
        }
    }

    if (!readInteger(parser, "quality", request.options.quality) ||
        !readInteger(parser, "bit-depth", request.options.bitDepth)) {
        code = usageError(err, "--quality and --bit-depth take whole numbers");
        return std::nullopt;
    }

    std::optional<float> exposure;
    if (!readSetting(parser, "exposure", darkestExposure, brightestExposure, exposure, err, code) ||
        !readSetting(parser, "temperature", warmestKelvin, coolestKelvin,
                     request.settings.temperature, err, code) ||
        !readSetting(parser, "tint", -tintLimit, tintLimit, request.settings.tint, err, code)) {
        return std::nullopt;
    }
    request.settings.exposure = exposure.value_or(0.0F);
    // Naming either half of a white balance is asking for a custom one; the
    // half left unnamed stays as the camera recorded it.
    if (request.settings.temperature.has_value() || request.settings.tint.has_value()) {
        request.settings.whiteBalance = WhiteBalanceMode::Custom;
    }

    request.options.embedProfile = !parser.isSet("no-profile");
    request.overwrite = parser.isSet("overwrite");
    request.quiet = parser.isSet("quiet");
    return request;
}

/// @brief Exports every input, continuing past the ones that fail.
int exportAll(const ExportRequest& request, std::ostream& err) {
    std::size_t failures = 0;

    for (const auto& input : request.inputs) {
        const auto destination =
            request.outputDirectory /
            (input.stem().string() + std::string(extensionFor(request.format)));

        try {
            if (!request.overwrite && std::filesystem::exists(destination)) {
                // Refused rather than replaced: the destination is usually a
                // directory of someone's photographs, and exportImage would
                // overwrite without a word.
                throw std::runtime_error(destination.string() +
                                         " already exists; pass --overwrite to replace it");
            }
            exportImage(develop(loadImage(input), request.settings), destination, request.options);
            if (!request.quiet) {
                err << input.string() << " -> " << destination.string() << '\n';
            }
        } catch (const std::exception& problem) {
            err << "error: " << input.string() << ": " << problem.what() << '\n';
            ++failures;
        }
    }

    if (failures > 0) {
        err << failures << " of " << request.inputs.size() << " failed\n";
        return cli::Failed;
    }
    return cli::Success;
}

} // namespace

int cli::runExportCommand(const QStringList& arguments, std::ostream& out, std::ostream& err) {
    QCommandLineParser parser;
    configure(parser);

    // parse() rather than process(): process() writes to the real stderr and
    // calls exit(), neither of which a tested function may do. Qt's help option
    // is therefore checked by hand rather than acted on for us.
    if (!parser.parse(arguments)) {
        return usageError(err, parser.errorText().toStdString());
    }
    if (parser.isSet("help")) {
        out << parser.helpText().toStdString();
        return Success;
    }

    int code = Success;
    const auto request = buildRequest(parser, err, code);
    if (!request) {
        return code;
    }
    return exportAll(*request, err);
}

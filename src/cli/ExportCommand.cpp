#include "ExportCommand.h"

#include "Cli.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <ImageExport.h>
#include <ImageImport.h>
#include <Photo.h>
#include <WhiteBalance.h>

#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <cmath>
#include <exception>
#include <filesystem>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace arraw;

void cli::setRotationAngle(GeometrySettings& geometry, double degrees) {
    if (!std::isfinite(degrees)) {
        throw std::invalid_argument("A rotation angle must be finite");
    }
    const double wrapped = std::fmod(degrees, 360.0);
    const int turns = static_cast<int>(std::round(wrapped / 90.0));
    constexpr QuarterTurn rotations[]{QuarterTurn::None, QuarterTurn::Clockwise90,
                                      QuarterTurn::Clockwise180, QuarterTurn::Clockwise270};
    geometry.rotation = rotations[(turns % 4 + 4) % 4];
    geometry.straighten = wrapped - static_cast<double>(turns) * 90.0;
}

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

/// @brief How a log is written out.
enum class LogFormat {
    Text, ///< One sentence a person reads.
    Json, ///< One object per line, for whatever reads the batch afterwards.
};

/// @brief Everything the command needs, once its arguments are understood.
struct ExportRequest {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path outputDirectory;
    ImageFileFormat format = ImageFileFormat::Jpeg;
    DevelopSettings settings;
    ExportOptions options;
    bool overwrite = false;
    bool quiet = false;
    LogFormat logFormat = LogFormat::Text;
};

/// @brief The name a log writes for a notice.
///
/// Stable, and not the sentence: this is what a script matches on. Exhaustive
/// and without a default, so a new notice cannot be added without one.
std::string nameOf(Notice notice) {
    switch (notice) {
    case Notice::SubstitutedWhiteBalance:
        return "substituted_white_balance";
    case Notice::Exported:
        return "exported";
    case Notice::InputFailed:
        return "input_failed";
    case Notice::BatchFinished:
        return "batch_finished";
    }
    return "unknown";
}

/// @brief The name a log writes for a severity.
std::string nameOf(Severity severity) {
    switch (severity) {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "unknown";
}

/// @brief Writes diagnostics to a stream as they happen.
///
/// Printing rather than collecting, because a batch that runs for an hour
/// should say what it is doing while it does it.
class StreamDiagnostics final : public DiagnosticLog {
public:
    StreamDiagnostics(std::ostream& stream, LogFormat format, bool quiet)
        : stream_(stream), format_(format), quiet_(quiet) {}

    /// @brief Drops the subject from a message that already begins with it.
    ///
    /// An exception carries its own context, because whoever catches it may
    /// have no idea which file it came from. A log line does know, and says so
    /// once.
    static std::string withoutSubject(const Diagnostic& diagnostic) {
        const std::string message = describe(diagnostic);
        if (!diagnostic.subject) {
            return message;
        }
        const std::string prefix = diagnostic.subject->string() + ": ";
        if (message.starts_with(prefix)) {
            return message.substr(prefix.size());
        }
        return message;
    }

    void record(const Diagnostic& diagnostic) override {
        // --quiet drops the running commentary and keeps everything that went
        // wrong, which is the distinction it has always drawn.
        if (quiet_ && diagnostic.severity == Severity::Info) {
            return;
        }
        if (format_ == LogFormat::Json) {
            QJsonObject object;
            object["notice"] = QString::fromStdString(nameOf(diagnostic.notice));
            object["severity"] = QString::fromStdString(nameOf(diagnostic.severity));
            // Left out rather than emitted empty when there is no photograph
            // to name: a reader asks whether the key is there.
            if (diagnostic.subject) {
                object["file"] = QString::fromStdString(diagnostic.subject->string());
            }
            object["message"] = QString::fromStdString(withoutSubject(diagnostic));
            stream_ << QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString() << '\n';
            return;
        }
        // A diagnostic about no particular photograph, such as a batch's own
        // summary, has no file to name.
        const std::string about =
            diagnostic.subject ? diagnostic.subject->string() + ": " : std::string{};
        if (diagnostic.severity == Severity::Info) {
            stream_ << about << withoutSubject(diagnostic) << '\n';
            return;
        }
        stream_ << nameOf(diagnostic.severity) << ": " << about << withoutSubject(diagnostic)
                << '\n';
    }

private:
    std::ostream& stream_;
    LogFormat format_;
    bool quiet_;
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
    if (!valid || !std::isfinite(parsed)) {
        code = usageError(err, std::string("--") + name + " takes a finite number");
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

/// @brief Reads geometry values without resolving or executing any transforms.
bool readGeometry(const QCommandLineParser& parser, GeometrySettings& geometry, std::ostream& err,
                  int& code) {
    if (parser.isSet("rotate")) {
        bool valid = false;
        const double degrees = parser.value("rotate").toDouble(&valid);
        if (!valid || !std::isfinite(degrees)) {
            code = usageError(err, "--rotate takes a finite angle in clockwise degrees");
            return false;
        }
        cli::setRotationAngle(geometry, degrees);
    }
    geometry.flipHorizontal = parser.isSet("flip-horizontal");
    geometry.flipVertical = parser.isSet("flip-vertical");
    if (parser.isSet("crop")) {
        const auto value = parser.value("crop").trimmed().toLower();
        if (value == "auto") {
            geometry.crop.rectangle.reset();
        } else {
            const auto edges = value.split(',');
            UprightCropRect rectangle;
            double* destinations[]{&rectangle.left, &rectangle.top, &rectangle.right,
                                   &rectangle.bottom};
            bool valid = edges.size() == 4;
            if (valid) {
                for (int index = 0; index < 4; ++index) {
                    bool parsed = false;
                    const double edge = edges[index].toDouble(&parsed);
                    valid = valid && parsed && std::isfinite(edge) && edge >= 0.0 && edge <= 1.0;
                    *destinations[index] = edge;
                }
            }
            if (!valid || rectangle.left >= rectangle.right || rectangle.top >= rectangle.bottom) {
                code = usageError(err, "--crop takes auto or left,top,right,bottom with finite "
                                       "edges from 0 to 1, left < right and top < bottom");
                return false;
            }
            geometry.crop.rectangle = rectangle;
        }
    }
    if (parser.isSet("crop-aspect")) {
        const auto value = parser.value("crop-aspect").trimmed().toLower();
        if (value == "free") {
            geometry.crop.aspect = FreeCropAspect{};
        } else if (value == "original") {
            geometry.crop.aspect = OriginalCropAspect{};
        } else {
            const auto parts = value.split(':');
            bool validWidth = false;
            bool validHeight = false;
            const double width = parts.size() == 2 ? parts[0].toDouble(&validWidth) : 0.0;
            const double height = parts.size() == 2 ? parts[1].toDouble(&validHeight) : 0.0;
            const double ratio = height > 0.0 ? width / height : 0.0;
            if (!validWidth || !validHeight || !std::isfinite(width) || !std::isfinite(height) ||
                width <= 0.0 || height <= 0.0 || !std::isfinite(ratio) || ratio <= 0.0) {
                code = usageError(err, "--crop-aspect takes free, original, or positive finite "
                                       "width:height, for example 3:2 or 2:3");
                return false;
            }
            geometry.crop.aspect = CropRatio{ratio};
        }
    }
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
        "captured, save for a gentle roll-off that bends the brightest values toward\n"
        "white instead of clipping them flat; --filmic-highlights 0 turns it off.\n"
        "Camera orientation is honoured. Rotation, flips and cropping are applied\n"
        "after colour and tone; crops always stay inside valid image content.");
    parser.addHelpOption();
    parser.addOption({{"o", "output"}, "Existing directory to write into.", "dir"});
    parser.addOption({"format", "png, jpeg, or tiff. Default: jpeg.", "name"});
    parser.addOption({"quality", "JPEG quality, 0-100. Default: 90.", "value"});
    parser.addOption({"bit-depth", "8 or 16. Default: 8.", "value"});
    parser.addOption({"encoding", "srgb, display-p3, or adobe-rgb. Default: srgb.", "name"});
    parser.addOption({"exposure", "Exposure adjustment in EV, -5 to 5.", "stops"});
    parser.addOption({"contrast", "Contrast, -100 to 100.", "amount"});
    parser.addOption({"shadows", "Lift or deepen the dark tones, -100 to 100.", "amount"});
    parser.addOption({"highlights", "Recover or raise the bright tones, -100 to 100.", "amount"});
    parser.addOption({"blacks", "Move the black point, -100 to 100.", "amount"});
    parser.addOption({"whites", "Move the white point, -100 to 100.", "amount"});
    parser.addOption({"temperature", "White balance in kelvin, 2000 to 12000. RAW only.", "k"});
    parser.addOption({"tint", "Green to magenta, -150 to 150. RAW only.", "amount"});
    parser.addOption(
        {"white-balance", "as-shot or custom. Temperature/tint imply custom.", "mode"});
    parser.addOption({"filmic-highlights", "Highlight roll-off, 0 to 100. Default: 25.", "amount"});
    parser.addOption(
        {"rotate", "Any finite clockwise angle, before flips. Default: 0.", "degrees"});
    parser.addOption({"flip-horizontal", "Flip horizontally in the upright frame."});
    parser.addOption({"flip-vertical", "Flip vertically in the upright frame."});
    parser.addOption(
        {"crop", "auto or normalised upright left,top,right,bottom. Default: auto.", "rectangle"});
    parser.addOption(
        {"crop-aspect", "free, original, or width:height (3:2, 2:3). Default: free.", "aspect"});
    parser.addOption({"no-profile", "Convert colour but do not embed the output profile."});
    parser.addOption({"overwrite", "Replace outputs that already exist."});
    parser.addOption({{"q", "quiet"}, "Do not report each file as it is written."});
    parser.addOption({"log-format", "text or json. Default: text.", "name"});
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
    std::optional<float> contrast;
    std::optional<float> shadows;
    std::optional<float> highlights;
    std::optional<float> blacks;
    std::optional<float> whites;
    std::optional<float> filmicHighlights;
    if (!readSetting(parser, "contrast", flattestContrast, steepestContrast, contrast, err, code) ||
        !readSetting(parser, "shadows", weakestToneControl, strongestToneControl, shadows, err,
                     code) ||
        !readSetting(parser, "highlights", weakestToneControl, strongestToneControl, highlights,
                     err, code) ||
        !readSetting(parser, "blacks", weakestToneControl, strongestToneControl, blacks, err,
                     code) ||
        !readSetting(parser, "whites", weakestToneControl, strongestToneControl, whites, err,
                     code) ||
        !readSetting(parser, "filmic-highlights", noFilmicHighlights, fullFilmicHighlights,
                     filmicHighlights, err, code) ||
        !readSetting(parser, "exposure", darkestExposure, brightestExposure, exposure, err, code) ||
        !readSetting(parser, "temperature", warmestKelvin, coolestKelvin,
                     request.settings.color.temperature, err, code) ||
        !readSetting(parser, "tint", -tintLimit, tintLimit, request.settings.color.tint, err,
                     code)) {
        return std::nullopt;
    }
    request.settings.tone.exposure = exposure.value_or(0.0F);
    request.settings.tone.contrast = contrast.value_or(0.0F);
    request.settings.tone.shadows = shadows.value_or(0.0F);
    request.settings.tone.highlights = highlights.value_or(0.0F);
    request.settings.tone.blacks = blacks.value_or(0.0F);
    request.settings.tone.whites = whites.value_or(0.0F);
    request.settings.tone.filmicHighlights =
        filmicHighlights.value_or(request.settings.tone.filmicHighlights);
    // Naming either half of a white balance is asking for a custom one; the
    // half left unnamed stays as the camera recorded it.
    if (request.settings.color.temperature.has_value() || request.settings.color.tint.has_value()) {
        request.settings.color.whiteBalance = WhiteBalanceMode::Custom;
    }
    if (parser.isSet("white-balance")) {
        const auto mode = parser.value("white-balance").toLower();
        if (mode == "as-shot") {
            if (parser.isSet("temperature") || parser.isSet("tint")) {
                code = usageError(err, "--white-balance as-shot cannot be combined with "
                                       "--temperature or --tint");
                return std::nullopt;
            }
            request.settings.color.whiteBalance = WhiteBalanceMode::AsShot;
        } else if (mode == "custom") {
            request.settings.color.whiteBalance = WhiteBalanceMode::Custom;
        } else {
            code = usageError(err, "--white-balance takes as-shot or custom");
            return std::nullopt;
        }
    }
    if (!readGeometry(parser, request.settings.geometry, err, code)) {
        return std::nullopt;
    }

    if (parser.isSet("log-format")) {
        const auto name = parser.value("log-format").toLower();
        if (name == "json") {
            request.logFormat = LogFormat::Json;
        } else if (name != "text") {
            code = usageError(err, "unknown log format '" + name.toStdString() +
                                       "'; expected text or json");
            return std::nullopt;
        }
    }

    request.options.embedProfile = !parser.isSet("no-profile");
    request.overwrite = parser.isSet("overwrite");
    request.quiet = parser.isSet("quiet");
    return request;
}

/// @brief Exports every input, continuing past the ones that fail.
int exportAll(const ExportRequest& request, std::ostream& err) {
    StreamDiagnostics log(err, request.logFormat, request.quiet);
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
            // The document first: what the file declares, including a white
            // balance it did not record, is said when the photograph opens
            // rather than when its pixels arrive. The command line's settings
            // are another snapshot of it, and the file on disk is untouched
            // (ADR 012). The decode is then not asked to repeat the warning.
            const Photo photo = openPhoto(input, log).with(request.settings);
            exportImage(develop(loadImage(input), photo.settings()), destination, request.options);
            log.record({.notice = Notice::Exported,
                        .severity = Severity::Info,
                        .subject = input,
                        .values = {destination.string()}});
        } catch (const std::exception& problem) {
            log.record({.notice = Notice::InputFailed,
                        .severity = Severity::Error,
                        .subject = input,
                        .values = {std::string(problem.what())}});
            ++failures;
        }
    }

    if (failures > 0) {
        // Through the log like everything else, so that --log-format json emits
        // nothing a JSON reader has to skip.
        log.record({.notice = Notice::BatchFinished,
                    .severity = Severity::Error,
                    .values = {static_cast<double>(failures),
                               static_cast<double>(request.inputs.size())}});
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

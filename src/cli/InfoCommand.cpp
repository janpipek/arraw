#include "InfoCommand.h"

#include "Cli.h"
#include "Command.h"
#include "LensOptions.h"
#include "SettingCodec.h"
#include "ShotInputs.h"
#include "SidecarWatch.h"
#include "StreamDiagnostics.h"
#include "TerminalStyle.h"

#include <ColorEncoding.h>
#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <ExifInfo.h>
#include <ImageImport.h>
#include <ImageOrientation.h>
#include <LensResolution.h>
#include <LocalAdjustmentEdits.h>
#include <MarksFilter.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <SettingDescriptors.h>
#include <SettingsJson.h>
#include <ShortestDecimal.h>
#include <Shot.h>
#include <Sidecar.h>
#include <Snapshots.h>

#include <QCommandLineParser>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using namespace arraw;

namespace {

/// @brief What `info` was asked for.
struct InfoRequest {
    std::vector<std::filesystem::path> inputs;
    /// @brief Which photographs are wanted by their marks; inactive wants all.
    MarksFilter filter;
    bool all = false;
    bool json = false;
    bool useSidecars = true;
    bool quiet = false;
    cli::LogFormat logFormat = cli::LogFormat::Text;
    /// @brief The lens database `--lens-database` named, if any.
    std::optional<std::filesystem::path> lensDatabase;
};

/// @brief The lens of a RAW as `info` reports it: which one, from where, and what its
/// corrections resolved to (ADR 050).
struct LensReport {
    /// @brief Whether there is anything to say; false for a file with no lens to speak of.
    bool shown = false;

    /// @brief The lens the corrections are for, when one was recorded or found.
    std::optional<LensIdentity> lens;

    /// @brief Whether the lens is the recorded one, rather than found now.
    bool recorded = false;

    /// @brief How a recorded lens was chosen.
    LensOrigin origin = LensOrigin::Matched;

    /// @brief Crop factor the profile is applied with.
    std::optional<double> cropFactor;

    /// @brief Stamp of the database the lens was looked up in, empty when none loaded.
    std::string database;

    /// @brief Stamp the lens was recorded against, empty when it was not recorded.
    std::string recordedDatabase;

    /// @brief Why there is no lens, when there is none.
    std::string problem;

    /// @brief What the switches that are on resolved to; empty when none is on or none resolved.
    LensCorrection correction;
};

/// @brief One photograph as `info` reports it.
struct FileReport {
    Photo photo;

    /// @brief The shot the photograph is the primary of, when it came out of a folder.
    std::optional<Shot> shot;

    /// @brief Sidecar beside the photograph, absent when it has none.
    std::optional<std::filesystem::path> sidecar;

    /// @brief Whether the sidecar was read; false when it was ignored.
    bool sidecarRead = true;

    /// @brief What the file records about its capture; empty when it records none.
    ExifInfo exif;

    /// @brief Program named in the sidecar, when it was read and says.
    std::optional<std::string> creatorTool;

    /// @brief Other tools' namespaces in the sidecar, when it was read.
    std::vector<ForeignNamespace> others;

    /// @brief The lens of a RAW, resolved; shown only when it has something to say.
    LensReport lens;

    /// @brief Snapshots the sidecar lists, when it was read.
    SnapshotCollection snapshots;

    /// @brief Snapshot the saved recipe is based on, when the sidecar says and it exists.
    std::optional<SnapshotId> savedBase;

    /// @brief Raw snapshot entries in the sidecar this arraw cannot use.
    std::size_t unreadableSnapshots = 0;
};

/// @brief Reports a usage problem of `info`.
int usageError(std::ostream& err, const std::string& message) {
    return cli::commandUsageError(err, "info", message);
}

/// @brief Declares the options and help of `info`.
void configure(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        "Show what is known about photographs, read-only.\n"
        "\n"
        "Inputs are files or folders. A folder stands for the shots in it, not\n"
        "recursively, and each shot is shown once, as its primary file (the RAW of a\n"
        "RAW+JPEG pair), with its format label and its companions. --min-rating,\n"
        "--rejected and --label list only the photographs whose sidecar marks match.\n"
        "\n"
        "For each file: its size, orientation, its sidecar, its\n"
        "rating and colour label, and the develop settings that differ from the\n"
        "defaults, one per line as key: value, in the order of the settings table,\n"
        "spelled by the same codec as the sidecar and the JSON settings document.\n"
        "Defaults, a rating of 0 and no label are not listed; --all lists every setting.\n"
        "A RAW also shows its camera colour encoding. Settings a render would not\n"
        "read (a temperature without Custom white balance, any on a non-RAW) are\n"
        "not listed. The camera, lens, exposure, time and place of the capture are\n"
        "shown when the file records them (EXIF; --json gives an \"exif\" object).\n"
        "Marks and\n"
        "settings arraw cannot store are not shown, nor is other tools' XMP beyond\n"
        "which tools wrote in the sidecar (the program that wrote it, and each\n"
        "other namespace with its prefix and number of properties).\n"
        "\n"
        "Every input is attempted, so one bad file does not hide the rest. The exit\n"
        "status is 0 when all were shown, 1 when any failed, 2 for a usage error. A\n"
        "sidecar that cannot be read fails its file, unless --no-sidecar is given,\n"
        "which reports the sidecar as ignored.\n"
        "\n"
        "A RAW that names a lens, or has a lens correction on or a lens recorded, also shows\n"
        "its lens: which one, whether it is the recorded one or was matched just now, the crop\n"
        "factor, the lens database's stamp, the corrections that are on, and the conditions\n"
        "each was resolved for (read from the file or assumed). A correction that cannot be\n"
        "applied is reported as a diagnostic that names the reason. --json gives a\n"
        "\"lensCorrection\" object.\n"
        "A photograph with snapshots lists each with its ID, parent ID, name and creation\n"
        "time (UTC), the saved base (the snapshot the saved settings are based on), and a\n"
        "note when the sidecar holds snapshot entries this arraw cannot use. --json gives\n"
        "\"snapshots\", \"savedBase\" and \"unreadableSnapshots\", with IDs as decimal\n"
        "strings. Thumbnails are never printed. `export --snapshot ID` exports one.\n"
        "The command never writes anything.");
    parser.addHelpOption();
    parser.addOption({"all", "List every develop setting, not only those that differ."});
    parser.addOption({"json",
                      "Print one JSON document for all files: {\"files\": [...]}. A file that "
                      "failed is left out of it."});
    parser.addOption({"no-sidecar", "Show each photograph as it opens without its .xmp sidecar."});
    cli::addLensOptions(parser, false);
    parser.addOption({{"q", "quiet"}, "Drop informational diagnostics, keeping what went wrong."});
    cli::addMarksFilterOptions(parser);
    cli::addLogFormatOption(parser);
    parser.addPositionalArgument("input", "Files, or folders of shots, to describe.",
                                 "info <input>...");
}

/// @brief Reads the request from the parsed command line, or reports why it cannot.
std::optional<InfoRequest> buildRequest(const QCommandLineParser& parser, std::ostream& err,
                                        int& code) {
    InfoRequest request;
    for (const QString& input : parser.positionalArguments()) {
        request.inputs.emplace_back(input.toStdString());
    }
    if (request.inputs.empty()) {
        code = usageError(err, "no input files given");
        return std::nullopt;
    }
    const auto logFormat = cli::readLogFormat(parser, "info", err, code);
    if (!logFormat) {
        return std::nullopt;
    }
    request.logFormat = *logFormat;
    request.all = parser.isSet("all");
    request.json = parser.isSet("json");
    request.useSidecars = !parser.isSet("no-sidecar");
    const auto filter = cli::readMarksFilter(parser, "info", err, code);
    if (!filter) {
        return std::nullopt;
    }
    request.filter = *filter;
    request.quiet = parser.isSet("quiet");
    const auto lens = cli::readLensOptions(parser, false, "info", err, code);
    if (!lens) {
        return std::nullopt;
    }
    request.lensDatabase = lens->database;
    return request;
}

/// @brief Names an orientation as the report spells it.
std::string_view orientationName(ImageOrientation orientation) {
    switch (orientation) {
    case ImageOrientation::Normal:
        return "normal";
    case ImageOrientation::MirrorHorizontal:
        return "mirror-horizontal";
    case ImageOrientation::Rotate180:
        return "rotate-180";
    case ImageOrientation::MirrorVertical:
        return "mirror-vertical";
    case ImageOrientation::Transpose:
        return "transpose";
    case ImageOrientation::Rotate90:
        return "rotate-90";
    case ImageOrientation::Transverse:
        return "transverse";
    case ImageOrientation::Rotate270:
        return "rotate-270";
    }
    return "unknown";
}

/// @brief Tells whether a photograph is a RAW, whose encoding is its camera's own.
bool isRaw(const ImageMetadata& metadata) {
    return !std::holds_alternative<NamedEncoding>(metadata.encoding);
}

/// @brief Names a colour label as the sidecar spells it.
std::string_view labelName(ColorLabel label) {
    for (const auto& [value, name] : colorLabelNames) {
        if (value == label) {
            return name;
        }
    }
    return "unknown";
}

/// @brief Spells a number in the shortest text that reads back the same.
std::string number(double value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, written.ptr);
}

/// @brief Spells a path as UTF-8 text, replacing bytes that are not valid UTF-8.
std::string pathText(const std::filesystem::path& path) {
    return QString::fromStdString(path.string()).toStdString();
}

/// @brief Spells an encoded value as a line of text: as the sidecar spells it, unset as "unset".
std::string textOf(const Encoded& encoded) {
    struct Speller {
        std::string operator()(std::monostate) const {
            return "unset";
        }
        std::string operator()(bool flag) const {
            return flag ? "true" : "false";
        }
        std::string operator()(double value) const {
            return number(value);
        }
        std::string operator()(const std::string& text) const {
            return text;
        }
        std::string operator()(const Compound& compound) const {
            std::string text;
            for (const auto& member : compound) {
                text += text.empty() ? "" : ",";
                text += number(member.second);
            }
            return text;
        }
        std::string operator()(const PointList& points) const {
            std::string text;
            for (const auto& [x, y] : points) {
                text += text.empty() ? "" : ";";
                text += number(x) + "," + number(y);
            }
            return text;
        }
    };
    return std::visit(Speller{}, encoded);
}

/// @brief Quotes a string as JSON.
std::string jsonString(std::string_view text) {
    std::string result = "\"";
    for (const char character : text) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                constexpr std::string_view digits = "0123456789abcdef";
                result += "\\u00";
                result += digits[static_cast<unsigned char>(character) >> 4];
                result += digits[static_cast<unsigned char>(character) & 0xF];
            } else {
                result += character;
            }
        }
    }
    return result + '"';
}

/// @brief Rounds to a number of decimals and spells the result as a number.
std::string rounded(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return number(std::round(value * scale) / scale);
}

/// @brief Spells a shutter time: "1/250 s" for a fraction of a second, else "1.3 s".
std::string exposureText(const URational& time) {
    const double seconds = time.value();
    if (!(seconds > 0.0)) {
        return std::to_string(time.numerator) + "/" + std::to_string(time.denominator) + " s";
    }
    if (seconds >= 1.0) {
        return rounded(seconds, 1) + " s";
    }
    return "1/" + rounded(1.0 / seconds, 0) + " s";
}

/// @brief Lists the capture information in the order of the report, as "label: text" lines.
std::vector<std::pair<std::string, std::string>> exifLines(const ExifInfo& exif) {
    std::vector<std::pair<std::string, std::string>> lines;
    if (exif.make || exif.model) {
        // Cameras often repeat the maker in the model ("Canon Canon EOS R5").
        std::string camera = exif.make.value_or("");
        const std::string model = exif.model.value_or("");
        if (!model.empty() && !camera.empty() && model.starts_with(camera)) {
            camera.clear();
        }
        camera += camera.empty() || model.empty() ? "" : " ";
        lines.emplace_back("camera", camera + model);
    }
    if (exif.lensModel) {
        lines.emplace_back("lens", *exif.lensModel);
    }
    std::string exposure;
    const auto add = [&exposure](const std::string& part) {
        exposure += exposure.empty() ? "" : "  ";
        exposure += part;
    };
    if (exif.exposureTime) {
        add(exposureText(*exif.exposureTime));
    }
    if (exif.fNumber) {
        add("f/" + rounded(exif.fNumber->value(), 1));
    }
    if (exif.photographicSensitivity) {
        add("ISO " + std::to_string(*exif.photographicSensitivity));
    }
    if (exif.focalLength) {
        std::string focal = rounded(exif.focalLength->value(), 1) + " mm";
        if (exif.focalLengthIn35mmFilm) {
            focal += " (" + std::to_string(*exif.focalLengthIn35mmFilm) + " mm equivalent)";
        }
        add(focal);
    }
    if (!exposure.empty()) {
        lines.emplace_back("exposure", exposure);
    }
    if (exif.exposureBiasValue) {
        lines.emplace_back("exposure bias", rounded(exif.exposureBiasValue->value(), 2) + " EV");
    }
    if (exif.flash) {
        lines.emplace_back("flash", (*exif.flash & 1U) != 0 ? "fired" : "did not fire");
    }
    if (exif.dateTimeOriginal) {
        lines.emplace_back("taken",
                           *exif.dateTimeOriginal +
                               (exif.offsetTimeOriginal ? " " + *exif.offsetTimeOriginal : ""));
    }
    if (exif.gps) {
        std::string place = rounded(exif.gps->latitude, 4) + ", " + rounded(exif.gps->longitude, 4);
        if (exif.gps->altitude) {
            place += ", " + rounded(*exif.gps->altitude, 1) + " m";
        }
        lines.emplace_back("GPS", place);
    }
    if (exif.artist) {
        lines.emplace_back("artist", *exif.artist);
    }
    if (exif.copyright) {
        lines.emplace_back("copyright", *exif.copyright);
    }
    return lines;
}

/// @brief Spells an optional fraction as a JSON object, or null.
template <class Rational> std::string jsonRational(const std::optional<Rational>& rational) {
    return rational ? "{\"numerator\": " + std::to_string(rational->numerator) +
                          ", \"denominator\": " + std::to_string(rational->denominator) + "}"
                    : "null";
}

/// @brief Spells the capture information as a JSON object of the fields present.
std::string jsonOfExif(const ExifInfo& exif) {
    std::string text = "{";
    const auto add = [&text](std::string_view key, const std::string& value) {
        text += (text.size() > 1 ? ", " : "") + jsonString(key) + ": " + value;
    };
    const auto addText = [&add](std::string_view key, const std::optional<std::string>& value) {
        if (value) {
            add(key, jsonString(*value));
        }
    };
    const auto addNumber = [&add](std::string_view key, const auto& value) {
        if (value) {
            add(key, std::to_string(*value));
        }
    };
    const auto addRational = [&add](std::string_view key, const auto& value) {
        if (value) {
            add(key, jsonRational(value));
        }
    };
    addText("make", exif.make);
    addText("model", exif.model);
    addText("lensModel", exif.lensModel);
    addText("dateTimeOriginal", exif.dateTimeOriginal);
    addText("offsetTimeOriginal", exif.offsetTimeOriginal);
    addRational("exposureTime", exif.exposureTime);
    addRational("fNumber", exif.fNumber);
    addNumber("photographicSensitivity", exif.photographicSensitivity);
    addRational("focalLength", exif.focalLength);
    addNumber("focalLengthIn35mmFilm", exif.focalLengthIn35mmFilm);
    addRational("exposureBiasValue", exif.exposureBiasValue);
    addNumber("flash", exif.flash);
    if (exif.gps) {
        add("gps", "{\"latitude\": " + number(exif.gps->latitude) +
                       ", \"longitude\": " + number(exif.gps->longitude) + ", \"altitude\": " +
                       (exif.gps->altitude ? number(*exif.gps->altitude) : "null") + "}");
    }
    addText("artist", exif.artist);
    addText("copyright", exif.copyright);
    return text + "}";
}

/// @brief A log that passes on what went wrong and drops what is merely so.
///
/// A file with no EXIF is not a problem `info` should print a line about: the
/// report shows no capture lines, and that says it. A file whose EXIF could
/// not be read is another matter.
class WarningsOnly final : public DiagnosticLog {
public:
    explicit WarningsOnly(DiagnosticLog& target) : target_(target) {}

    void record(const Diagnostic& diagnostic) override {
        if (diagnostic.severity != Severity::Info) {
            target_.record(diagnostic);
        }
    }

private:
    DiagnosticLog& target_;
};

/// @brief One setting of a photograph that is listed: its row and value.
struct ListedSetting {
    const FieldDescriptor* descriptor;
    Encoded value;
};

/// @brief Picks the settings to list, in table order.
std::vector<ListedSetting> listedSettings(const DevelopSettings& settings, bool all) {
    const DevelopSettings defaults;
    std::vector<ListedSetting> listed;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        Encoded value = encode(descriptor, settings);
        if (all || value != encode(descriptor, defaults)) {
            listed.push_back({&descriptor, std::move(value)});
        }
    }
    return listed;
}

/// @brief Writes the local adjustments of a state, one line each; nothing when it has none.
void writeMasks(std::ostream& out, const DevelopState& state) {
    if (state.localAdjustments.empty()) {
        return;
    }
    out << "  local adjustments:\n";
    for (const LocalAdjustment& mask : state.localAdjustments) {
        out << "    " << cli::terminalText(displayedMaskName(state, mask.id)) << " ("
            << maskTypeName(mask.shape) << "): " << (mask.enabled ? "enabled" : "disabled")
            << ", opacity " << shortestText(shortestDouble(mask.opacity));
        if (mask.invert) {
            out << ", inverted";
        }
        if (const auto* brush = std::get_if<BrushMask>(&mask.shape); brush) {
            const StrokeList& list = brush->strokes ? *brush->strokes : *emptyStrokeList();
            const std::size_t strokes = list.size();
            const std::size_t points = list.pointCount();
            out << ", " << strokes << (strokes == 1 ? " stroke, " : " strokes, ") << points
                << (points == 1 ? " point" : " points");
        }
        for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
            const float delta = mask.deltas.*descriptor.member;
            if (delta != 0.0F) {
                out << ", " << descriptor.key << ' ' << shortestText(shortestDouble(delta));
            }
        }
        out << '\n';
    }
}

/// @brief Resolves what `info` says of a RAW's lens.
///
/// Looked at only when there is something to say: a correction is on, a lens is recorded, the
/// file names a lens, or every setting was asked for, so a file without a lens never loads the
/// database. A correction that is on is resolved as a render would, its notices going to @p log.
LensReport lensReportOf(const Photo& photo, bool all, cli::LensDatabaseSource& source,
                        DiagnosticLog& log) {
    LensReport report;
    const LensSettings& settings = photo.negative().lens;
    const LensFacts& facts = photo.metadata().lens;
    if (!isRaw(photo.metadata()) ||
        !(all || settings.any() || settings.lens || !facts.lensNames.empty())) {
        return report;
    }
    report.shown = true;
    const LensDatabase& database = source.get();
    report.database = database.stamp();
    CollectedDiagnostics notes;
    if (const auto match = resolveLensMatch(settings, facts, database, notes, photo.path())) {
        report.lens = match->lens;
        report.cropFactor = match->cropFactor;
    }
    if (settings.lens) {
        report.recorded = true;
        report.origin = settings.lens->origin;
        report.recordedDatabase = settings.lens->database;
        // The recorded lens is shown even when the database no longer lists it.
        report.lens = LensIdentity{settings.lens->maker, settings.lens->model};
        report.cropFactor = settings.lens->cropFactor;
    }
    if (!report.lens) {
        for (const Diagnostic& note : notes.entries()) {
            if (note.severity != Severity::Info) {
                report.problem = describe(note);
                break;
            }
        }
    } else if (!notes.entries().empty() && settings.lens) {
        // A recorded lens that has left the database still says so.
        for (const Diagnostic& note : notes.entries()) {
            if (note.notice == Notice::LensProfileMissing) {
                report.problem = describe(note);
            }
        }
    }
    if (settings.any()) {
        report.correction = lensCorrectionFor(photo, database, log);
    }
    return report;
}

/// @brief Spells how a provenance was resolved, one correction's conditions.
std::string provenanceText(const LensProvenance& provenance) {
    const auto condition = [](std::string text, bool read) {
        return text + (read ? " (read)" : " (assumed)");
    };
    return condition("focal length " + shortestText(provenance.focalLength) + " mm",
                     provenance.focalLengthRead) +
           ", " +
           condition("aperture f/" + shortestText(provenance.aperture), provenance.apertureRead) +
           ", " +
           condition("distance " + shortestText(provenance.distance) + " m",
                     provenance.distanceRead);
}

/// @brief Writes the lens of a RAW as lines of text.
void writeLens(std::ostream& out, const Photo& photo, const LensReport& report) {
    const LensSettings& settings = photo.negative().lens;
    if (report.lens) {
        out << "  Lens: " << cli::terminalText(report.lens->model);
        if (report.recorded) {
            out << " (recorded, " << tokenOf(report.origin) << ")";
        } else {
            out << " (matched now)";
        }
        out << '\n';
        if (!report.problem.empty()) {
            out << "  Lens problem: " << cli::terminalText(report.problem) << '\n';
        }
        if (report.cropFactor) {
            out << "  Lens crop factor: " << shortestText(*report.cropFactor) << '\n';
        }
    } else {
        out << "  Lens: none"
            << (report.problem.empty() ? "" : " (" + cli::terminalText(report.problem) + ")")
            << '\n';
    }
    if (!report.database.empty()) {
        out << "  Lens database: " << report.database;
        if (!report.recordedDatabase.empty() && report.recordedDatabase != report.database) {
            out << " (recorded against " << report.recordedDatabase << ")";
        }
        out << '\n';
    }
    std::string vignetting = "vignetting";
    if (settings.vignettingAmount != profileVignettingAmount) {
        vignetting += " " + shortestText(shortestDouble(settings.vignettingAmount)) + "%";
    }
    std::string on;
    using Correction = std::pair<bool, std::string>;
    for (const auto& [wanted, name] : {Correction{settings.distortion, "distortion"},
                                       Correction{settings.vignettingAmount > 0.0F, vignetting},
                                       Correction{settings.lateralCa, "chromatic aberration"}}) {
        if (wanted) {
            on += (on.empty() ? "" : ", ") + name;
        }
    }
    out << "  Lens corrections: " << (on.empty() ? "off" : on) << '\n';
    for (const auto& [from, name] :
         {std::pair{&report.correction.distortionFrom, "distortion"},
          std::pair{&report.correction.vignettingFrom, "vignetting"},
          std::pair{&report.correction.lateralCaFrom, "chromatic aberration"}}) {
        if (*from) {
            out << "    " << name << ": " << provenanceText(**from) << '\n';
        }
    }
}

/// @brief Spells the lens of a RAW as a JSON object, or null when there is none to show.
std::string lensJson(const LensReport& report) {
    if (!report.shown) {
        return "null";
    }
    const auto provenance = [](const std::optional<LensProvenance>& from) {
        if (!from) {
            return std::string("null");
        }
        return "{\"focalLength\": " + number(from->focalLength) +
               ", \"focalLengthRead\": " + (from->focalLengthRead ? "true" : "false") +
               ", \"aperture\": " + number(from->aperture) +
               ", \"apertureRead\": " + (from->apertureRead ? "true" : "false") +
               ", \"distance\": " + number(from->distance) +
               ", \"distanceRead\": " + (from->distanceRead ? "true" : "false") +
               ", \"cropFactor\": " + number(from->cropFactor) +
               ", \"database\": " + jsonString(from->database) + "}";
    };
    return std::string("{\"lens\": ") +
           (report.lens ? "{\"maker\": " + jsonString(report.lens->maker) +
                              ", \"model\": " + jsonString(report.lens->model) + "}"
                        : std::string("null")) +
           ", \"source\": " +
           (report.lens ? jsonString(report.recorded ? "recorded" : "matched") : "null") +
           ", \"cropFactor\": " + (report.cropFactor ? number(*report.cropFactor) : "null") +
           ", \"database\": " + (report.database.empty() ? "null" : jsonString(report.database)) +
           ", \"problem\": " + (report.problem.empty() ? "null" : jsonString(report.problem)) +
           ", \"distortion\": " + provenance(report.correction.distortionFrom) +
           ", \"vignetting\": " + provenance(report.correction.vignettingFrom) +
           ", \"lateralCa\": " + provenance(report.correction.lateralCaFrom) + "}";
}

/// @brief Spells a snapshot's creation time as ISO 8601 in UTC, as the sidecar does.
std::string snapshotTime(std::chrono::sys_seconds time) {
    return QDateTime::fromSecsSinceEpoch(time.time_since_epoch().count(), QTimeZone::UTC)
        .toString(Qt::ISODate)
        .toStdString();
}

/// @brief Spells a snapshot ID, a decimal number.
std::string idText(SnapshotId id) {
    return std::to_string(id.value);
}

/// @brief Writes the snapshots of a photograph, nothing when it has none to report.
void writeSnapshots(std::ostream& out, const FileReport& report) {
    const SnapshotCollection& snapshots = report.snapshots;
    if (!snapshots.entries.empty()) {
        out << "  snapshots:\n";
        for (const Snapshot& snapshot : snapshots.entries) {
            out << "    " << idText(snapshot.id) << ": " << cli::terminalText(snapshot.name)
                << " (parent " << (snapshot.parent ? idText(*snapshot.parent) : std::string("none"))
                << ", created " << snapshotTime(snapshot.created) << ")\n";
        }
    }
    if (!snapshots.entries.empty() || report.savedBase) {
        out << "  saved base: " << (report.savedBase ? idText(*report.savedBase) : "none") << '\n';
    }
    if (report.unreadableSnapshots > 0) {
        out << "  unreadable snapshot entries: " << report.unreadableSnapshots
            << " (kept in the sidecar and counted toward the limit of " << maximumSnapshots
            << "; the app can discard them)\n";
    }
}

/// @brief Spells the snapshots of a photograph as the JSON members `snapshots`, `savedBase` and
/// `unreadableSnapshots`, with IDs as decimal strings and without thumbnails.
std::string snapshotsJson(const FileReport& report) {
    std::string text = "\"snapshots\": [";
    bool first = true;
    for (const Snapshot& snapshot : report.snapshots.entries) {
        text += first ? "" : ", ";
        first = false;
        text += "{\"id\": " + jsonString(idText(snapshot.id)) + ", \"parent\": " +
                (snapshot.parent ? jsonString(idText(*snapshot.parent)) : "null") +
                ", \"name\": " + jsonString(snapshot.name) +
                ", \"created\": " + jsonString(snapshotTime(snapshot.created)) + "}";
    }
    return text + "], \"savedBase\": " +
           (report.savedBase ? jsonString(idText(*report.savedBase)) : "null") +
           ", \"unreadableSnapshots\": " + std::to_string(report.unreadableSnapshots);
}

/// @brief Writes one file as lines of text.
void writeText(std::ostream& out, const FileReport& report, bool all) {
    const Photo& photo = report.photo;
    const ImageMetadata& metadata = photo.metadata();
    out << cli::accented(out, pathText(photo.path()), cli::Accent::Heading) << '\n';
    if (report.shot) {
        out << "  format: " << formatLabel(*report.shot) << '\n';
        if (!report.shot->companions.empty()) {
            out << "  companions:";
            for (const auto& companion : report.shot->companions) {
                out << ' ' << cli::terminalText(pathText(companion.filename()));
            }
            out << '\n';
        }
    }
    out << "  size: " << metadata.size.width << " x " << metadata.size.height << '\n'
        << "  orientation: " << orientationName(metadata.orientation) << '\n';
    if (isRaw(metadata)) {
        out << "  encoding: camera\n";
    }
    for (const auto& [label, line] : exifLines(report.exif)) {
        out << "  " << label << ": " << cli::terminalText(line) << '\n';
    }
    out << "  sidecar: " << (report.sidecar ? cli::terminalText(pathText(*report.sidecar)) : "none")
        << (report.sidecar && !report.sidecarRead ? " (ignored)" : "") << '\n';
    const PhotoMarks& marks = photo.marks();
    if (marks.rating != 0) {
        out << "  rating: ";
        if (marks.rating == rejectedRating) {
            out << "rejected\n";
        } else {
            out << marks.rating << '\n';
        }
    }
    if (marks.label) {
        out << "  label: " << labelName(*marks.label) << '\n';
    }
    writeSnapshots(out, report);
    if (report.creatorTool || !report.others.empty()) {
        out << "  other tools:\n";
        if (report.creatorTool) {
            out << "    written by: " << cli::terminalText(*report.creatorTool) << '\n';
        }
        for (const ForeignNamespace& other : report.others) {
            out << "    " << xmpNamespaceOwner(other.uri).value_or("unknown") << " ("
                << (other.prefix.empty() ? "no prefix" : cli::terminalText(other.prefix) + ":")
                << ", " << other.properties
                << (other.properties == 1 ? " property)\n" : " properties)\n");
        }
    }
    if (isRaw(metadata) && (all || photo.negative() != defaultNegativeFor(metadata))) {
        std::string name(tokenOf(photo.negative().demosaic));
        std::ranges::transform(name, name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        out << "  Demosaic: " << (name == "LINEAR" ? "Linear" : name) << '\n';
    }
    if (report.lens.shown) {
        writeLens(out, photo, report.lens);
    }
    const auto listed =
        listedSettings(withoutUnusedSettings(photo.state().settings, isRaw(metadata)), all);
    if (listed.empty()) {
        out << "  develop settings: defaults\n";
    } else {
        out << "  develop settings:\n";
        for (const auto& [descriptor, value] : listed) {
            out << "    " << descriptor->key << ": " << cli::terminalText(textOf(value)) << '\n';
        }
    }
    writeMasks(out, photo.state());
}

/// @brief Spells one file as a JSON object.
std::string jsonOfReport(const FileReport& report, bool all) {
    const Photo& photo = report.photo;
    const ImageMetadata& metadata = photo.metadata();
    const PhotoMarks& marks = photo.marks();
    const Shot shot = report.shot.value_or(Shot{photo.path(), {}});
    std::string companions;
    for (const auto& companion : shot.companions) {
        companions += (companions.empty() ? "" : ", ") + jsonString(pathText(companion));
    }
    std::string text =
        "{\"path\": " + jsonString(pathText(photo.path())) +
        ", \"format\": " + jsonString(formatLabel(shot)) + ", \"companions\": [" + companions +
        "], \"size\": {\"width\": " + std::to_string(metadata.size.width) +
        ", \"height\": " + std::to_string(metadata.size.height) +
        "}, \"orientation\": " + jsonString(orientationName(metadata.orientation)) +
        ", \"encoding\": " + (isRaw(metadata) ? "\"camera\"" : "null") +
        ", \"exif\": " + jsonOfExif(report.exif) +
        ", \"sidecar\": " + (report.sidecar ? jsonString(pathText(*report.sidecar)) : "null") +
        ", \"sidecarRead\": " + (report.sidecarRead ? "true" : "false") +
        ", \"marks\": {\"rating\": " + std::to_string(marks.rating) +
        ", \"label\": " + (marks.label ? jsonString(labelName(*marks.label)) : "null") +
        "}, \"creatorTool\": " + (report.creatorTool ? jsonString(*report.creatorTool) : "null") +
        ", \"others\": [";
    for (std::size_t i = 0; i < report.others.size(); ++i) {
        const ForeignNamespace& other = report.others[i];
        const auto owner = xmpNamespaceOwner(other.uri);
        text += (i > 0 ? ", " : "") + std::string("{\"uri\": ") + jsonString(other.uri) +
                ", \"prefix\": " + jsonString(other.prefix) +
                ", \"properties\": " + std::to_string(other.properties) +
                ", \"owner\": " + (owner ? jsonString(*owner) : "null") + "}";
    }
    text += "], " + snapshotsJson(report) + ", \"settings\": {";
    bool first = true;
    for (const auto& [descriptor, value] :
         listedSettings(withoutUnusedSettings(photo.state().settings, isRaw(metadata)), all)) {
        text += first ? "" : ", ";
        text += jsonString(descriptor->key) + ": " + encodedToJson(value);
        first = false;
    }
    return text + "}, \"negative\": " + negativeToJson(photo.negative()) +
           ", \"lensCorrection\": " + lensJson(report.lens) +
           ", \"localAdjustments\": " + localAdjustmentsToJson(photo.state()) + "}";
}

/// @brief Finds the sidecar beside a photograph, if it is a regular file.
std::optional<std::filesystem::path> findSidecar(const std::filesystem::path& input) {
    const auto sidecar = sidecarPath(input);
    std::error_code ignored;
    return std::filesystem::is_regular_file(sidecar, ignored) ? std::optional{sidecar}
                                                              : std::nullopt;
}

/// @brief Opens one photograph the way the request asks, failing on an unreadable sidecar.
FileReport open(const InfoRequest& request, const std::filesystem::path& input,
                DiagnosticLog& log) {
    WarningsOnly exifLog(log);
    if (!request.useSidecars) {
        Photo photo(input, readImageMetadata(input, log));
        return {std::move(photo),
                std::nullopt,
                findSidecar(input),
                false,
                readExif(input, exifLog),
                std::nullopt,
                {},
                {}};
    }
    cli::SidecarWatch watch(log);
    ImageMetadata metadata = readImageMetadata(input, watch);
    const auto sidecar = findSidecar(input);
    std::optional<SidecarContents> contents;
    if (sidecar) {
        // One read serves the photograph and the report; it fails in one place, with the hint.
        try {
            contents = readSidecar(input, watch);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string(error.what()) +
                                     "; fix the sidecar, or pass --no-sidecar to show the file "
                                     "without it");
        }
    }
    if (watch.unreadable) {
        throw std::runtime_error("its sidecar could not be read; fix it, or pass --no-sidecar to "
                                 "show the file without it");
    }
    // As openPhoto: a sidecar that records no settings leaves the kind's defaults.
    const auto photoOf = [&](ImageMetadata described) {
        if (!contents) {
            return Photo(input, std::move(described));
        }
        DevelopState state =
            contents->state ? *contents->state : defaultStateFor(described.encoding);
        const NegativeSettings negative =
            contents->negative.value_or(defaultNegativeFor(described));
        return Photo(input, std::move(described), negative, std::move(state), contents->marks);
    };
    FileReport report{photoOf(std::move(metadata)), std::nullopt, sidecar, true,
                      readExif(input, exifLog),     std::nullopt, {},      {}};
    if (contents) {
        report.creatorTool = contents->creatorTool;
        report.others = contents->others;
        report.snapshots = contents->snapshots;
        report.savedBase = contents->savedBase;
        report.unreadableSnapshots = contents->unreadableSnapshots;
    }
    return report;
}

/// @brief Describes every input, continuing past the ones that fail.
int infoAll(const InfoRequest& request, std::ostream& out, std::ostream& err) {
    cli::StreamDiagnostics log(err, request.logFormat, request.quiet);
    cli::LensDatabaseSource lenses(request.lensDatabase);
    const cli::ExpandedInputs expanded = cli::expandInputs(request.inputs, log);
    std::size_t failures = expanded.unreadableFolders;
    std::size_t shown = 0;
    std::size_t filteredOut = 0;
    if (request.json) {
        out << "{\"files\": [";
    }
    for (const auto& [input, shot] : expanded.photographs) {
        try {
            if (!cli::passesFilter(request.filter, input, request.useSidecars)) {
                ++filteredOut;
                continue;
            }
            FileReport report = open(request, input, log);
            report.shot = shot;
            report.lens = lensReportOf(report.photo, request.all, lenses, log);
            if (request.json) {
                out << (shown > 0 ? ", " : "") << jsonOfReport(report, request.all);
            } else {
                out << (shown > 0 ? "\n" : "");
                writeText(out, report, request.all);
            }
            ++shown;
        } catch (const std::exception& problem) {
            log.record({.notice = Notice::InputFailed,
                        .severity = Severity::Error,
                        .subject = input,
                        .values = {std::string(problem.what())}});
            ++failures;
        }
    }
    if (request.json) {
        out << "]}\n";
    }
    if (filteredOut > 0) {
        log.record({.notice = Notice::FilteredOut, .values = {static_cast<double>(filteredOut)}});
    }
    if (failures > 0) {
        log.record({.notice = Notice::BatchFinished,
                    .severity = Severity::Error,
                    .values = {static_cast<double>(failures),
                               static_cast<double>(expanded.photographs.size() +
                                                   expanded.unreadableFolders)}});
        return cli::Failed;
    }
    return cli::Success;
}

} // namespace

int cli::runInfoCommand(const QStringList& arguments, std::ostream& out, std::ostream& err,
                        const StartApplication& start) {
    QCommandLineParser parser;
    configure(parser);
    if (!parser.parse(arguments)) {
        return usageError(err, parser.errorText().toStdString());
    }
    if (parser.isSet("help")) {
        writeStyledHelp(out, commandHelp(parser));
        return Success;
    }
    int code = Success;
    const auto request = buildRequest(parser, err, code);
    if (!request) {
        return code;
    }
    // Qt Core only: the metadata of a non-RAW comes through Qt's image codecs,
    // which are found through an application; nothing here touches a device.
    start(ApplicationKind::Core);
    return infoAll(*request, out, err);
}

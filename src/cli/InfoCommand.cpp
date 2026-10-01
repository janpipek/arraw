#include "InfoCommand.h"

#include "Cli.h"
#include "Command.h"
#include "SettingCodec.h"
#include "SidecarWatch.h"
#include "StreamDiagnostics.h"

#include <ColorEncoding.h>
#include <DevelopSettings.h>
#include <Diagnostics.h>
#include <ImageImport.h>
#include <ImageOrientation.h>
#include <Photo.h>
#include <SettingDescriptors.h>
#include <Sidecar.h>

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

#include <charconv>
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
    bool all = false;
    bool json = false;
    bool useSidecars = true;
    bool quiet = false;
    cli::LogFormat logFormat = cli::LogFormat::Text;
};

/// @brief One photograph as `info` reports it.
struct FileReport {
    Photo photo;

    /// @brief Sidecar beside the photograph, absent when it has none.
    std::optional<std::filesystem::path> sidecar;

    /// @brief Whether the sidecar was read; false when it was ignored.
    bool sidecarRead = true;

    /// @brief Program named in the sidecar, when it was read and says.
    std::optional<std::string> creatorTool;

    /// @brief Other tools' namespaces in the sidecar, when it was read.
    std::vector<ForeignNamespace> others;
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
        "For each file: its size, orientation, its sidecar, its\n"
        "rating and colour label, and the develop settings that differ from the\n"
        "defaults, one per line as key: value, in the order of the settings table,\n"
        "spelled by the same codec as the sidecar and the JSON settings document.\n"
        "Defaults, a rating of 0 and no label are not listed; --all lists every setting.\n"
        "A RAW also shows its camera colour encoding. Settings a render would not\n"
        "read (a temperature without Custom white balance, any on a non-RAW) are\n"
        "not listed. Camera metadata beyond that is not read yet. Marks and\n"
        "settings arraw cannot store are not shown, nor is other tools' XMP beyond\n"
        "which tools wrote in the sidecar (the program that wrote it, and each\n"
        "other namespace with its prefix and number of properties).\n"
        "\n"
        "Every input is attempted, so one bad file does not hide the rest. The exit\n"
        "status is 0 when all were shown, 1 when any failed, 2 for a usage error. A\n"
        "sidecar that cannot be read fails its file, unless --no-sidecar is given,\n"
        "which reports the sidecar as ignored.\n"
        "The command never writes anything.");
    parser.addHelpOption();
    parser.addOption({"all", "List every develop setting, not only those that differ."});
    parser.addOption({"json",
                      "Print one JSON document for all files: {\"files\": [...]}. A file that "
                      "failed is left out of it."});
    parser.addOption({"no-sidecar", "Show each photograph as it opens without its .xmp sidecar."});
    parser.addOption({{"q", "quiet"}, "Drop informational diagnostics, keeping what went wrong."});
    cli::addLogFormatOption(parser);
    parser.addPositionalArgument("input", "Files to describe.", "info <input>...");
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
    request.quiet = parser.isSet("quiet");
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

/// @brief Writes one file as lines of text.
void writeText(std::ostream& out, const FileReport& report, bool all) {
    const Photo& photo = report.photo;
    const ImageMetadata& metadata = photo.metadata();
    out << pathText(photo.path()) << '\n'
        << "  size: " << metadata.size.width << " x " << metadata.size.height << '\n'
        << "  orientation: " << orientationName(metadata.orientation) << '\n';
    if (isRaw(metadata)) {
        out << "  encoding: camera\n";
    }
    out << "  sidecar: " << (report.sidecar ? pathText(*report.sidecar) : "none")
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
    if (report.creatorTool || !report.others.empty()) {
        out << "  other tools:\n";
        if (report.creatorTool) {
            out << "    written by: " << *report.creatorTool << '\n';
        }
        for (const ForeignNamespace& other : report.others) {
            out << "    " << xmpNamespaceOwner(other.uri).value_or("unknown") << " ("
                << (other.prefix.empty() ? "no prefix" : other.prefix + ":") << ", "
                << other.properties << (other.properties == 1 ? " property)\n" : " properties)\n");
        }
    }
    const auto listed =
        listedSettings(withoutUnusedSettings(photo.settings(), isRaw(metadata)), all);
    if (listed.empty()) {
        out << "  develop settings: defaults\n";
        return;
    }
    out << "  develop settings:\n";
    for (const auto& [descriptor, value] : listed) {
        out << "    " << descriptor->key << ": " << textOf(value) << '\n';
    }
}

/// @brief Spells one file as a JSON object.
std::string jsonOfReport(const FileReport& report, bool all) {
    const Photo& photo = report.photo;
    const ImageMetadata& metadata = photo.metadata();
    const PhotoMarks& marks = photo.marks();
    std::string text =
        "{\"path\": " + jsonString(pathText(photo.path())) +
        ", \"size\": {\"width\": " + std::to_string(metadata.size.width) +
        ", \"height\": " + std::to_string(metadata.size.height) +
        "}, \"orientation\": " + jsonString(orientationName(metadata.orientation)) +
        ", \"encoding\": " + (isRaw(metadata) ? "\"camera\"" : "null") +
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
    text += "], \"settings\": {";
    bool first = true;
    for (const auto& [descriptor, value] :
         listedSettings(withoutUnusedSettings(photo.settings(), isRaw(metadata)), all)) {
        text += first ? "" : ", ";
        text += jsonString(descriptor->key) + ": " + encodedToJson(value);
        first = false;
    }
    return text + "}}";
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
    if (!request.useSidecars) {
        return {Photo(input, readImageMetadata(input, log)),
                findSidecar(input),
                false,
                std::nullopt,
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
    FileReport report{contents
                          ? Photo(input, std::move(metadata), contents->settings, contents->marks)
                          : Photo(input, std::move(metadata)),
                      sidecar,
                      true,
                      std::nullopt,
                      {}};
    if (contents) {
        report.creatorTool = contents->creatorTool;
        report.others = contents->others;
    }
    return report;
}

/// @brief Describes every input, continuing past the ones that fail.
int infoAll(const InfoRequest& request, std::ostream& out, std::ostream& err) {
    cli::StreamDiagnostics log(err, request.logFormat, request.quiet);
    std::size_t failures = 0;
    std::size_t shown = 0;
    if (request.json) {
        out << "{\"files\": [";
    }
    for (const auto& input : request.inputs) {
        try {
            const FileReport report = open(request, input, log);
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
    if (failures > 0) {
        log.record({.notice = Notice::BatchFinished,
                    .severity = Severity::Error,
                    .values = {static_cast<double>(failures),
                               static_cast<double>(request.inputs.size())}});
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
        out << commandHelp(parser);
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

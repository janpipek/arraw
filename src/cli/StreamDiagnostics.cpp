#include "StreamDiagnostics.h"

#include "Command.h"

#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <ostream>
#include <string>

namespace arraw::cli {
namespace {

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
    case Notice::GpuSoftwareRefused:
        return "gpu_software_refused";
    case Notice::GpuSoftwareAccepted:
        return "gpu_software_accepted";
    case Notice::GpuAdapterSkipped:
        return "gpu_adapter_skipped";
    case Notice::GpuNoFloatTextures:
        return "gpu_no_float_textures";
    case Notice::GpuReadBackNotPromised:
        return "gpu_readback_not_promised";
    case Notice::GpuFailed:
        return "gpu_failed";
    case Notice::GpuRoundTripRedescribed:
        return "gpu_round_trip_redescribed";
    case Notice::GpuRoundTripChanged:
        return "gpu_round_trip_changed";
    case Notice::GpuDisabled:
        return "gpu_disabled";
    case Notice::GpuUsed:
        return "gpu_used";
    case Notice::CpuUsed:
        return "cpu_used";
    case Notice::GpuFallback:
        return "gpu_fallback";
    case Notice::SettingClamped:
        return "setting_clamped";
    case Notice::SettingUnknown:
        return "setting_unknown";
    case Notice::SettingMalformed:
        return "setting_malformed";
    case Notice::NewerSettingsVersion:
        return "newer_settings_version";
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

/// @brief Drops the subject from a message that already begins with it.
///
/// An exception carries its own context, because whoever catches it may
/// have no idea which file it came from. A log line does know, and says so
/// once.
std::string withoutSubject(const Diagnostic& diagnostic) {
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

} // namespace

void StreamDiagnostics::record(const Diagnostic& diagnostic) {
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
    stream_ << nameOf(diagnostic.severity) << ": " << about << withoutSubject(diagnostic) << '\n';
}

void addLogFormatOption(QCommandLineParser& parser) {
    parser.addOption({"log-format", "text or json. Default: text.", "name"});
}

std::optional<LogFormat> readLogFormat(const QCommandLineParser& parser, std::string_view command,
                                       std::ostream& err, int& code) {
    if (!parser.isSet("log-format")) {
        return LogFormat::Text;
    }
    const auto name = parser.value("log-format").toLower();
    if (name == "json") {
        return LogFormat::Json;
    }
    if (name == "text") {
        return LogFormat::Text;
    }
    code = commandUsageError(
        err, command, "unknown log format '" + name.toStdString() + "'; expected text or json");
    return std::nullopt;
}

} // namespace arraw::cli

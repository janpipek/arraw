#include "DebugLog.h"

#include "ui/ThemeColors.h"

#include <QBrush>
#include <QVariant>

#include <initializer_list>

namespace arraw::app {

namespace {

/// @brief Writes a time of day to the millisecond, enough to order a session's events.
QString timeText(const QDateTime& time) {
    return time.toString(QStringLiteral("HH:mm:ss.zzz"));
}

/// @brief Names a severity as a log column shows it.
QString severityName(Severity severity) {
    switch (severity) {
    case Severity::Info:
        return QStringLiteral("Info");
    case Severity::Warning:
        return QStringLiteral("Warning");
    case Severity::Error:
        return QStringLiteral("Error");
    }
    return {};
}

/// @brief Names a notice as the enumerator does, for matching by eye or by filter.
QString noticeName(Notice notice) {
    // Exhaustive and without a default, so that a new notice fails the build
    // here rather than showing up nameless.
    switch (notice) {
    case Notice::SubstitutedWhiteBalance:
        return QStringLiteral("SubstitutedWhiteBalance");
    case Notice::Exported:
        return QStringLiteral("Exported");
    case Notice::InputFailed:
        return QStringLiteral("InputFailed");
    case Notice::BatchFinished:
        return QStringLiteral("BatchFinished");
    case Notice::GpuSoftwareRefused:
        return QStringLiteral("GpuSoftwareRefused");
    case Notice::GpuSoftwareAccepted:
        return QStringLiteral("GpuSoftwareAccepted");
    case Notice::GpuAdapterSkipped:
        return QStringLiteral("GpuAdapterSkipped");
    case Notice::GpuNoFloatTextures:
        return QStringLiteral("GpuNoFloatTextures");
    case Notice::GpuReadBackNotPromised:
        return QStringLiteral("GpuReadBackNotPromised");
    case Notice::GpuFailed:
        return QStringLiteral("GpuFailed");
    case Notice::GpuRoundTripRedescribed:
        return QStringLiteral("GpuRoundTripRedescribed");
    case Notice::GpuRoundTripChanged:
        return QStringLiteral("GpuRoundTripChanged");
    case Notice::GpuDisabled:
        return QStringLiteral("GpuDisabled");
    case Notice::GpuUsed:
        return QStringLiteral("GpuUsed");
    case Notice::CpuUsed:
        return QStringLiteral("CpuUsed");
    case Notice::GpuFallback:
        return QStringLiteral("GpuFallback");
    case Notice::SettingClamped:
        return QStringLiteral("SettingClamped");
    case Notice::SettingUnknown:
        return QStringLiteral("SettingUnknown");
    case Notice::SettingMalformed:
        return QStringLiteral("SettingMalformed");
    case Notice::NewerSettingsVersion:
        return QStringLiteral("NewerSettingsVersion");
    case Notice::SidecarUnreadable:
        return QStringLiteral("SidecarUnreadable");
    case Notice::ExifUnreadable:
        return QStringLiteral("ExifUnreadable");
    case Notice::NoPhotographs:
        return QStringLiteral("NoPhotographs");
    case Notice::FilteredOut:
        return QStringLiteral("FilteredOut");
    case Notice::OptionIgnored:
        return QStringLiteral("OptionIgnored");
    case Notice::PreviewUnreadable:
        return QStringLiteral("PreviewUnreadable");
    case Notice::LocalAdjustmentDropped:
        return QStringLiteral("LocalAdjustmentDropped");
    case Notice::LocalAdjustmentFieldIgnored:
        return QStringLiteral("LocalAdjustmentFieldIgnored");
    case Notice::NewerLocalAdjustmentsVersion:
        return QStringLiteral("NewerLocalAdjustmentsVersion");
    case Notice::MetadataNotCarried:
        return QStringLiteral("MetadataNotCarried");
    }
    return {};
}

/// @brief Names a Qt message type as a log column shows it.
QString levelName(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("Debug");
    case QtInfoMsg:
        return QStringLiteral("Info");
    case QtWarningMsg:
        return QStringLiteral("Warning");
    case QtCriticalMsg:
        return QStringLiteral("Critical");
    case QtFatalMsg:
        return QStringLiteral("Fatal");
    }
    return {};
}

/// @brief Picks the text colour for a row by how bad it is, or none for the usual.
QVariant severityBrush(bool warning, bool error) {
    if (error) {
        return QBrush(theme::errorText);
    }
    if (warning) {
        return QBrush(theme::warningText);
    }
    return {};
}

/// @brief Names a column from a list of headers, for horizontal headers only.
QVariant header(std::initializer_list<const char*> names, int section, Qt::Orientation orientation,
                int role) {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
        section >= static_cast<int>(names.size())) {
        return {};
    }
    return QString::fromLatin1(names.begin()[section]);
}

} // namespace

void DiagnosticModel::append(const Diagnostic& diagnostic) {
    LogModel::append({QDateTime::currentDateTime(), diagnostic});
}

int DiagnosticModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : MessageColumn + 1;
}

QVariant DiagnosticModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const auto& [time, diagnostic] = entryAt(index.row());
    if (role == Qt::ForegroundRole) {
        return severityBrush(diagnostic.severity == Severity::Warning,
                             diagnostic.severity == Severity::Error);
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
        return {};
    }
    switch (index.column()) {
    case TimeColumn:
        return timeText(time);
    case SeverityColumn:
        return severityName(diagnostic.severity);
    case NoticeColumn:
        return noticeName(diagnostic.notice);
    case SubjectColumn:
        // Through UTF-16: the narrow string() is in the ANSI code page on Windows.
        return diagnostic.subject ? QString::fromStdU16String(diagnostic.subject->u16string())
                                  : QString{};
    case MessageColumn:
        return QString::fromStdString(describe(diagnostic));
    default:
        return {};
    }
}

QVariant DiagnosticModel::headerData(int section, Qt::Orientation orientation, int role) const {
    return header({"Time", "Severity", "Notice", "Subject", "Message"}, section, orientation, role);
}

void QtMessageModel::append(QtMsgType type, const QString& category, const QString& text) {
    LogModel::append({QDateTime::currentDateTime(), type, category, text});
}

int QtMessageModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : MessageColumn + 1;
}

QVariant QtMessageModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const QtMessageEntry& entry = entryAt(index.row());
    if (role == Qt::ForegroundRole) {
        return severityBrush(entry.type == QtWarningMsg,
                             entry.type == QtCriticalMsg || entry.type == QtFatalMsg);
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
        return {};
    }
    switch (index.column()) {
    case TimeColumn:
        return timeText(entry.time);
    case LevelColumn:
        return levelName(entry.type);
    case CategoryColumn:
        return entry.category;
    case MessageColumn:
        return entry.text;
    default:
        return {};
    }
}

QVariant QtMessageModel::headerData(int section, Qt::Orientation orientation, int role) const {
    return header({"Time", "Level", "Category", "Message"}, section, orientation, role);
}

} // namespace arraw::app
